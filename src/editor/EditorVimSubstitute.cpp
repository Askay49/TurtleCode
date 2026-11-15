#include "EditorWidget.hpp"
#include "app/I18n.hpp"
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QTimer>
#include <QScrollBar>
#include <QScopeGuard>
#include <QProgressDialog>
#include <algorithm>
#include <limits>

void EditorWidget::beginVimSubstitute(const VimEx::Invocation& command,std::shared_ptr<VimEx::LineMarks> selected){
    vimSubGlobalLast_=selected?std::optional<std::size_t>(std::size_t(command.last-1)):std::nullopt;
    auto spec=VimSearch::parseSubstitute(command.arguments);auto& state=VimState::Session::instance();
    if(!spec.error.isEmpty()){finishVimEx(spec.error);return;}
    if(spec.repeat && state.lastSubPattern.isEmpty()){finishVimEx("E33: No previous substitute regular expression");return;}
    auto flags=spec.flags;
    if(flags.startsWith('&')){flags=state.lastSubFlags+flags.mid(1);}
    auto countAt=0;while(countAt<flags.size() && !flags[countAt].isDigit())++countAt;
    const auto countText=flags.mid(countAt).trimmed();flags=flags.left(countAt).trimmed();
    int count=0;if(!countText.isEmpty()){bool ok=false;count=countText.toInt(&ok);if(!ok || count<=0){finishVimEx("E939: Positive count required");return;}}
    for(const auto flag:flags)if(!QStringLiteral("gcenpiIl#r").contains(flag)){finishVimEx("E488: Invalid substitute flag: "+QString(flag));return;}
    auto pattern=spec.repeat?state.lastSubPattern:spec.pattern.isEmpty()?state.searchPattern:spec.pattern;
    if(command.definition.name=="~" || flags.contains('r'))pattern=state.searchPattern;
    if(pattern.isEmpty()){finishVimEx("E35: No previous regular expression");return;}
    QString error;const auto program=spec.repeat?state.lastReplacement:VimSearch::expandReplacement(spec.replacement,state.lastReplacement,&error);
    if(!error.isEmpty()){finishVimEx(error);return;}
    auto options=state.searchOptions;
    if(command.definition.name=="smagic")options.magic=true;else if(command.definition.name=="snomagic")options.magic=false;
    for(const auto flag:flags){if(flag=='i')options.caseOverride=1;else if(flag=='I')options.caseOverride=-1;}
    state.lastSubPattern=pattern;state.lastReplacement=program;state.lastSubFlags=flags;state.searchPattern=pattern;state.searchOffset.clear();vimHighlightHidden_=false;state.set('/',VimState::text(pattern));
    const bool global=state.globalSubstitute!=flags.contains('g'),countOnly=flags.contains('n'),confirm=flags.contains('c') && !countOnly,ignoreError=flags.contains('e');
    const auto first=std::size_t(count?command.last-1:command.first-1),last=std::size_t(count?std::min<qint64>(VimMotions::lastLine(buffer_,vimFinalNewline_)+1,command.last+count-1)-1:command.last-1);
    const auto original=cursorPosition_;const bool finalNewline=vimFinalNewline_;refreshEditorConfig();const auto eol=editorConfig_.eol.value_or(SmartEditing::eol(buffer_));
    const auto subject=vimSearchSubjectRevision_==lspRevision_?vimSearchSubject_:nullptr;auto prepared=std::make_shared<std::shared_ptr<VimSearch::Plan>>();
    auto preparedSubject=std::make_shared<std::shared_ptr<VimSearch::Subject>>();
    runBulkOperation(I18n::text("Preparing substitutions…"),[prepared,preparedSubject,subject,pattern,program,options,first,last,global,countOnly,eol,original,finalNewline,selected](PieceTreeBuffer& buffer,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        VimSearch::Pattern expression(pattern,options);if(!expression.error().isEmpty())throw std::runtime_error(expression.error().toStdString());
        *preparedSubject=subject?subject:std::make_shared<VimSearch::Subject>(buffer,finalNewline,[&]{step(10);});
        std::optional<qint64> selectedRow=selected?selected->takeFirst():std::nullopt;
        const auto include=[&](std::size_t row){while(selectedRow && *selectedRow<qint64(row))selectedRow=selected->takeFirst();return selectedRow && *selectedRow==qint64(row);};
        *prepared=VimSearch::plan(buffer,**preparedSubject,expression,program,first,last,global,countOnly,eol,[&]{step(50);},selected?std::function<bool(std::size_t)>(include):std::function<bool(std::size_t)>{});return HistoryResult{original,{original}};
    },false,{},[this,prepared,preparedSubject,confirm,countOnly,ignoreError,flags](bool ok){
        if(!ok)return;
        vimSearchSubject_=*preparedSubject;vimSearchSubjectRevision_=lspRevision_;const auto plan=*prepared;
        if(!plan || !plan->count){if(vimSubGlobalLast_){moveCursor(VimMotions::firstText(buffer_,*vimSubGlobalLast_),false);vimSubGlobalLast_.reset();scheduleVimEx();return;}if(ignoreError || std::any_of(vimExFrames_.begin(),vimExFrames_.end(),[](const auto& frame){return frame.kind==ExFrame::Global;})){vimExMessage(I18n::text("No matches"));scheduleVimEx();}else finishVimEx("E486: Pattern not found");return;}
        if(countOnly){vimExMessage(I18n::text("%1 matches on %2 lines").arg(qulonglong(plan->count)).arg(qulonglong(plan->lines)));scheduleVimEx();updateVimSearchHighlights();return;}
        vimSubPlan_=plan;vimSubIndex_=vimSubAccepted_=0;vimSubDelta_=0;vimSubRevision_=lspRevision_;vimSubPrintFlags_=flags;
        if(confirm){vimSubConfirm_=true;nextVimSubstitution();}else applyVimSubstitutionPlan();
    });
}
void EditorWidget::applyVimSubstitutionPlan(std::size_t from,std::ptrdiff_t delta){
    const auto plan=vimSubPlan_;if(!plan || from>=plan->count){finishVimSubstitution();return;}
    const auto original=cursorPosition_;const auto count=plan->count-from;vimSubConfirm_=false;
    const bool finalNewline=vimFinalNewline_;
    runBulkOperation(I18n::text("Replacing matches…"),[plan,from,delta,original,finalNewline](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        auto payload=std::make_shared<ChunkedFile>();const auto last=plan->at(plan->count-1);auto target=std::size_t(std::ptrdiff_t(last.begin+last.length)+delta);
        for(auto upper=plan->count;upper>from;){
            const auto lower=upper-from>512?upper-512:from;std::vector<EditHistory::StoredChange> changes;changes.reserve(upper-lower);const auto payloadOffset=payload->size();
            std::string pending;
            const auto append=[&](std::string_view bytes){pending+=bytes;if(pending.size()>=64*1024){payload->append(pending);pending.clear();}};
            for(auto index=lower;index<upper;++index){
                step(20);const auto edit=plan->at(index);const auto at=std::size_t(std::ptrdiff_t(edit.begin)+delta);const auto removed=std::size_t(edit.end-edit.begin);
                for(std::size_t p=0;p<removed;p+=64*1024){step(30);append(buffer.slice(at+p,std::min<std::size_t>(64*1024,removed-p)));}
                for(std::size_t p=0;p<edit.length;p+=64*1024){step(40);append(plan->text->read(edit.offset+p,std::min<std::size_t>(64*1024,edit.length-p)));}
                changes.push_back({at,removed,std::size_t(edit.length)});
            }
            if(!pending.empty())payload->append(pending);
            for(auto index=upper;index>lower;){const auto edit=plan->at(--index);const auto at=std::size_t(std::ptrdiff_t(edit.begin)+delta);step(50);
                buffer.erase(at,edit.end-edit.begin);
                for(std::size_t p=0;p<edit.length;p+=64*1024){step(60);buffer.insert(at+p,plan->text->read(edit.offset+p,std::min<std::size_t>(64*1024,edit.length-p)));}
                if(index+1<plan->count)target=std::size_t(std::ptrdiff_t(target)+std::ptrdiff_t(edit.length)-std::ptrdiff_t(edit.end-edit.begin));
            }
            const auto after=VimMotions::firstText(buffer,std::min(VimMotions::lastLine(buffer,finalNewline),buffer.lineIndexAt(std::min(target,buffer.size()))));
            history.recordDiskBatch(changes,payload,{original},{after},0,payloadOffset);upper=lower;
        }
        const auto cursor=VimMotions::firstText(buffer,std::min(VimMotions::lastLine(buffer,finalNewline),buffer.lineIndexAt(std::min(target,buffer.size()))));payload->dropCache();return HistoryResult{cursor,{cursor}};
    },true,{},[this,count](bool ok){if(!ok){vimSubPlan_.reset();return;}vimSubAccepted_+=count;finishVimSubstitution();});
}
void EditorWidget::nextVimSubstitution(){
    if(!vimSubConfirm_ || !vimSubPlan_ || !vimExExecuting_)return;
    if(vimSearchCancelled_)vimSearchCancelled_->store(true);
    ++vimSearchGeneration_;
    if(busy_){QTimer::singleShot(5,this,&EditorWidget::nextVimSubstitution);return;}
    vimSubRevision_=lspRevision_;
    if(vimSubIndex_>=vimSubPlan_->count){finishVimSubstitution();return;}
    const auto edit=vimSubPlan_->at(vimSubIndex_);const auto begin=std::size_t(std::ptrdiff_t(edit.begin)+vimSubDelta_),end=begin+std::size_t(edit.end-edit.begin);
    moveCursor(begin,false);vimSearchHighlights_={{begin,end}};
    const auto bytes=vimSubPlan_->text->read(edit.offset,std::min<std::size_t>(edit.length,160));
    auto display=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));display.replace('\n',QStringLiteral("↵"));
    display=vimExHints_->fontMetrics().elidedText(display,Qt::ElideRight,std::max(40,viewport()->width()-120));
    vimExInput_->parentWidget()->hide();vimExHints_->setText(I18n::text("Replace with “%1”?\ny: yes · n: no · a: all · q: quit · l: last").arg(display));
    vimExBar_->show();positionVimCommandLine();viewport()->update();emit vimModeChanged();
}
bool EditorWidget::answerVimSubstitution(QKeyEvent* event){
    if(busy_){if(event->key()==Qt::Key_Escape){emit vimExInterruptRequested();for(auto* progress:findChildren<QProgressDialog*>())progress->cancel();}event->accept();return true;}
    if(beginVimKey(event)){finishVimSubstitution(true);return true;}const auto guard=qScopeGuard([this]{endVimKey();});event->accept();
    if(event->key()==Qt::Key_Control || event->key()==Qt::Key_Shift)return true;
    if(event->key()==Qt::Key_Escape || (event->modifiers()==Qt::ControlModifier && event->key()==Qt::Key_C)){finishVimSubstitution(true);return true;}
    if(vimSubRevision_!=lspRevision_){vimSubConfirm_=false;vimSubPlan_.reset();finishVimEx(I18n::text("Substitution stopped: the document changed"));return true;}
    if(event->modifiers()==Qt::ControlModifier && (event->key()==Qt::Key_E || event->key()==Qt::Key_Y)){verticalScrollBar()->setValue(verticalScrollBar()->value()+(event->key()==Qt::Key_E?1:-1));return true;}
    const auto key=event->text();
    if(key=="q"){finishVimSubstitution();return true;}
    if(key=="a"){applyVimSubstitutionPlan(vimSubIndex_,vimSubDelta_);return true;}
    if(key=="n"){++vimSubIndex_;nextVimSubstitution();return true;}
    if(key!="y" && key!="l")return true;
    const auto edit=vimSubPlan_->at(vimSubIndex_++);const auto at=std::size_t(std::ptrdiff_t(edit.begin)+vimSubDelta_);
    if(edit.length>=1024*1024 || edit.end-edit.begin>=1024*1024){
        const auto plan=vimSubPlan_;const bool last=key=="l",done=last || vimSubIndex_>=plan->count;const auto original=cursorPosition_;const bool finalNewline=vimFinalNewline_;
        runBulkOperation(I18n::text("Replacing matches…"),[plan,edit,at,original,done,finalNewline](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
            auto payload=std::make_shared<ChunkedFile>();const auto removed=std::size_t(edit.end-edit.begin);
            for(std::size_t p=0;p<removed;p+=64*1024){step(20);payload->append(buffer.slice(at+p,std::min<std::size_t>(64*1024,removed-p)));}
            buffer.erase(at,removed);
            for(std::size_t p=0;p<edit.length;p+=64*1024){step(50);const auto bytes=plan->text->read(edit.offset+p,std::min<std::size_t>(64*1024,edit.length-p));payload->append(bytes);buffer.insert(at+p,bytes);}
            const auto cursor=done?VimMotions::firstText(buffer,std::min(VimMotions::lastLine(buffer,finalNewline),buffer.lineIndexAt(std::min(at+std::size_t(edit.length),buffer.size())))):at;
            history.recordOnDisk(at,payload,removed,edit.length,original,cursor,0);return HistoryResult{cursor,{cursor}};
        },true,{},[this,edit,last](bool ok){if(!ok)return;vimSubDelta_+=std::ptrdiff_t(edit.length)-std::ptrdiff_t(edit.end-edit.begin);++vimSubAccepted_;if(last && vimSubPlan_)vimSubIndex_=vimSubPlan_->count;nextVimSubstitution();});return true;
    }
    const auto text=vimSubPlan_->text->read(edit.offset,edit.length);replaceRange(at,edit.end-edit.begin,text);
    if(!busy_){const bool done=key=="l" || vimSubIndex_>=vimSubPlan_->count;moveCursor(done?VimMotions::firstText(buffer_,std::min(VimMotions::lastLine(buffer_,vimFinalNewline_),buffer_.lineIndexAt(std::min(at+std::size_t(edit.length),buffer_.size())))):at,false);}
    vimSubDelta_+=std::ptrdiff_t(edit.length)-std::ptrdiff_t(edit.end-edit.begin);++vimSubAccepted_;vimSubRevision_=lspRevision_;
    if(key=="l"){vimSubIndex_=vimSubPlan_->count;}
    nextVimSubstitution();return true;
}
void EditorWidget::finishVimSubstitution(bool interrupted){
    if(vimSubGlobalLast_){moveCursor(VimMotions::firstText(buffer_,*vimSubGlobalLast_),false);vimSubGlobalLast_.reset();}
    vimSubConfirm_=false;vimSubPlan_.reset();if(vimExBar_)vimExBar_->hide();
    normalizeVimCursor();ensureCursorVisible();vimSearchHighlights_.clear();emit vimModeChanged();viewport()->update();
    vimExMessage(I18n::text("%1 substitutions").arg(qulonglong(vimSubAccepted_)));
    if(vimSubPrintFlags_.contains('p') || vimSubPrintFlags_.contains('#') || vimSubPrintFlags_.contains('l')){
        const auto at=vimLineStart(currentLine()),length=std::min<std::size_t>(lineContentEnd(currentLine())-at,4096);const auto bytes=buffer_.slice(at,length);auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));
        if(vimSubPrintFlags_.contains('l')){text.replace("\t","^I");text+='$';}if(vimSubPrintFlags_.contains('#'))text=QString::number(currentLine()+1)+"  "+text;vimExMessage(text);
    }
    if(interrupted){vimExRemaining_.clear();if(isVimReplaying())vimReplayCancel_=true;}
    scheduleVimEx();updateVimSearchHighlights();
}
