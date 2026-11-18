#include "EditorWidget.hpp"
#include "app/I18n.hpp"
#include "app/ThemeManager.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMetaMethod>
#include <QScopeGuard>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace {
class CommandInput final : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
protected:
    void keyPressEvent(QKeyEvent* event) override {QLineEdit::keyPressEvent(event);event->accept();}
};
const VimEx::Registry& exRegistry(){static const VimEx::Registry registry;return registry;}
}
bool EditorWidget::vimOwnsFocus() const {return hasFocus() || (vimExActive_ && vimExInput_ && vimExInput_->hasFocus());}
void EditorWidget::positionVimCommandLine() {
    if (!vimExBar_ || vimExBar_->isHidden()) return;

    constexpr int margin = 12;
    constexpr int preferredWidth = 720;
    const auto area = viewport()->size();
    const int width = std::clamp(area.width() - 2 * margin, 0, preferredWidth);
    const int height = std::min(vimExBar_->sizeHint().height(), area.height());
    const int top = std::min(std::max(margin, area.height() / 6),
                             std::max(0, area.height() - height - margin));
    vimExBar_->setGeometry((area.width() - width) / 2, top, width, height);
    vimExBar_->raise();
}
void EditorWidget::showVimCommandLine(int count,QChar prompt){
    vimCommandPrompt_=prompt;
    QString initial;
    if(prompt==':'){
        if(vim_.mode()==VimController::Mode::Visual){switchVimMode(VimController::Mode::Normal);initial="'<,'>";}
        else if(count)initial=count==1?QStringLiteral("."):QStringLiteral(".,.+%1").arg(count-1);
        finishVimChange();vimCommandKeys_.clear();
    }
    hideCompletions();
    if(findBar_)findBar_->hide();
    if(!vimExBar_){
        vimExBar_=new QFrame(viewport());vimExBar_->setObjectName("vimCommandBar");
        auto* layout=new QVBoxLayout(vimExBar_);layout->setContentsMargins(10,6,10,6);layout->setSpacing(4);
        auto* row=new QWidget(vimExBar_);auto* inputLayout=new QHBoxLayout(row);inputLayout->setContentsMargins(0,0,0,0);
        vimExPrompt_=new QLabel(":",row);inputLayout->addWidget(vimExPrompt_);
        vimExInput_=new CommandInput(row);vimExInput_->setObjectName("vimCommandInput");vimExInput_->setMaxLength(65536);
        vimExInput_->setAccessibleName(I18n::text("Vim command line"));vimExInput_->setClearButtonEnabled(false);
        vimExInput_->installEventFilter(this);inputLayout->addWidget(vimExInput_);layout->addWidget(row);
        vimExHints_=new QLabel(vimExBar_);vimExHints_->setObjectName("vimCommandHints");vimExHints_->setTextFormat(Qt::PlainText);layout->addWidget(vimExHints_);
        vimExHints_->setMaximumHeight(fontMetrics().height()*6);
        vimExHints_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        connect(vimExInput_,&QLineEdit::textChanged,this,[this]{updateVimExHints();});
        connect(vimExInput_,&QLineEdit::textEdited,this,[this]{vimExCompletions_.clear();vimExCompletionIndex_=-1;vimExHistoryPrefix_.clear();vimExHistoryIndex_=(vimCommandPrompt_==':'?VimState::Session::instance().commandHistory:VimState::Session::instance().searchHistory).size();});
    }
    const auto& theme=ThemeManager::instance();
    vimExBar_->setStyleSheet(QStringLiteral("QFrame#vimCommandBar { background:%1; border:1px solid %2; border-radius:4px; } QLineEdit {background:%1; color:%3; border:0; padding:2px;} QLabel {color:%3;}")
        .arg(theme.color("editor.background").name(),theme.color("editor.selection").name(),theme.color("editor.foreground").name()));
    vimExInput_->setFont(font());vimExActive_=true;vimExRegisterPending_=false;
    vimExPrompt_->setText(QString(prompt));
    vimExInput_->parentWidget()->show();vimExInput_->setText(initial);vimExInput_->setCursorPosition(initial.size());
    vimExDraft_=initial;vimExHistoryPrefix_.clear();vimExHistoryIndex_=(prompt==':'?VimState::Session::instance().commandHistory:VimState::Session::instance().searchHistory).size();
    vimExCompletions_.clear();vimExCompletionIndex_=-1;
    updateVimExHints();vimExBar_->show();positionVimCommandLine();vimExInput_->setFocus();emit vimModeChanged();
}
void EditorWidget::closeVimCommandLine(){
    const bool active=std::exchange(vimExActive_,false);vimExRegisterPending_=false;
    if(active && vimCommandPrompt_!=':'){
        if(vimSearchCancelled_)vimSearchCancelled_->store(true);
        ++vimSearchGeneration_;vimSearchRunning_=false;
        restoreVimPreviewFolds();
        cursorPosition_=std::min(vimSearchOrigin_,buffer_.size());anchor_=vimSearchOriginAnchor_;cursors_={cursorPosition_};cursorAnchors_={anchor_};
        verticalScrollBar()->setValue(vimSearchOriginTop_);horizontalScrollBar()->setValue(vimSearchOriginLeft_);viewport()->update();
    }
    if(vimExBar_)vimExBar_->hide();
    if(active){setFocus();emit vimModeChanged();if(vimCommandPrompt_!=':')QTimer::singleShot(0,this,&EditorWidget::updateVimSearchHighlights);}
}
void EditorWidget::updateVimExHints(){
    if(!vimExHints_)return;
    if(vimCommandPrompt_!=':'){previewVimSearch();return;}
    const auto prefix=vimExInput_->text();
    const auto candidates=exRegistry().complete(prefix);
    vimExHints_->setText(vimExRegisterPending_?I18n::text("Insert register…"):candidates.isEmpty()?I18n::text("Enter: execute · Esc: cancel · ↑/↓: history · Tab: complete"):candidates.join(QStringLiteral("   ")));
}
bool EditorWidget::filterVimCommandLine(QEvent* event){
    if(!vimExActive_)return false;
    if(event->type()==QEvent::ShortcutOverride){event->accept();return true;}
    if(event->type()==QEvent::InputMethod){
        auto* ime=static_cast<QInputMethodEvent*>(event);auto& state=VimState::Session::instance();
        if(!vimSendingReplay_ && !state.recording.isNull() && !ime->commitString().isEmpty()){
            VimState::Input input;input.kind=VimState::Input::Text;input.text=ime->commitString();
            if(state.recordingBytes+std::size_t(input.text.toUtf8().size())<=1024*1024 && state.recordingInputs.size()<100000){state.recordingInputs.append(input);state.recordingBytes+=std::size_t(input.text.toUtf8().size());}
            else toggleVimRecording();
        }
        return false;
    }
    if(event->type()==QEvent::FocusOut){
        QTimer::singleShot(0,this,[this]{if(vimExActive_ && !vimOwnsFocus()){closeVimCommandLine();cancelVimReplay();}});return false;
    }
    if(event->type()!=QEvent::KeyPress)return false;
    auto* key=static_cast<QKeyEvent*>(event);
    if(beginVimKey(key)){closeVimCommandLine();return true;}
    const auto guard=qScopeGuard([this]{endVimKey();});
    if(key->key()==Qt::Key_Control || key->key()==Qt::Key_Shift || key->key()==Qt::Key_Alt || key->key()==Qt::Key_Meta)return true;
    const auto control=key->modifiers()==Qt::ControlModifier;
    if(key->key()==Qt::Key_Escape || (control && (key->key()==Qt::Key_C || key->key()==Qt::Key_BracketLeft))){closeVimCommandLine();return true;}
    if(vimExRegisterPending_){
        vimExRegisterPending_=false;const auto name=key->text().isEmpty()?QChar{}:key->text()[0];
        if(name=='%' || name=='#')VimState::Session::instance().set(name,VimState::text(name=='%'?filePath_:property("vimAlternatePath").toString()));
        const auto value=VimState::Session::instance().read(name);
        if(value.text && value.text->size()<=65536){const auto bytes=value.text->read(0,value.text->size());vimExInput_->insert(QString::fromUtf8(bytes.data(),qsizetype(bytes.size())));}
        updateVimExHints();return true;
    }
    if(control && key->key()==Qt::Key_R){vimExRegisterPending_=true;updateVimExHints();return true;}
    if(key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter){
        const auto line=vimExInput_->text();
        const bool search=vimCommandPrompt_!=':';closeVimCommandLine();if(search)commitVimSearch(line);else executeVimCommandLine(line);return true;
    }
    if(key->key()==Qt::Key_Backspace && vimExInput_->text().isEmpty()){closeVimCommandLine();return true;}
    if(control && key->key()==Qt::Key_U){const auto at=vimExInput_->cursorPosition();vimExInput_->setSelection(0,at);vimExInput_->insert({});return true;}
    if(control && key->key()==Qt::Key_W){
        const auto end=vimExInput_->cursorPosition();auto begin=end;const auto text=vimExInput_->text();
        while(begin>0 && text[begin-1].isSpace())--begin;
        while(begin>0 && !text[begin-1].isSpace())--begin;
        vimExInput_->setSelection(begin,end-begin);vimExInput_->insert({});return true;
    }
    if(key->key()==Qt::Key_Tab || key->key()==Qt::Key_Backtab){if(vimCommandPrompt_==':')completeVimEx(key->key()==Qt::Key_Backtab?-1:1);else vimExInput_->insert("\t");return true;}
    if(key->key()==Qt::Key_Up || key->key()==Qt::Key_Down || (control && (key->key()==Qt::Key_P || key->key()==Qt::Key_N))){
        const auto& history=vimCommandPrompt_==':'?VimState::Session::instance().commandHistory:VimState::Session::instance().searchHistory;
        if(vimExHistoryIndex_==history.size()){vimExDraft_=vimExInput_->text();vimExHistoryPrefix_=control?QString{}:vimExDraft_.left(vimExInput_->cursorPosition());}
        const int direction=key->key()==Qt::Key_Up || key->key()==Qt::Key_P?-1:1;
        for(auto i=vimExHistoryIndex_+direction;i>=0 && i<=history.size();i+=direction){
            if(i==history.size() || history[i].startsWith(vimExHistoryPrefix_)){vimExHistoryIndex_=i;vimExInput_->setText(i==history.size()?vimExDraft_:history[i]);break;}
        }
        vimExCompletions_.clear();vimExCompletionIndex_=-1;return true;
    }
    return false;
}
void EditorWidget::completeVimEx(int direction){
    const auto text=vimExInput_->text();const auto cursor=vimExInput_->cursorPosition();
    if(vimExCompletions_.isEmpty()){
        auto start=cursor;while(start>0 && text[start-1].isLetter() && text[start-1].unicode()<128)--start;
        const auto commandStart=start>0?text.lastIndexOf('|',start-1)+1:0;
        const auto before=text.mid(commandStart,start-commandStart);
        VimEx::Context context;context.last=std::max<qint64>(1,VimMotions::lastLine(buffer_,vimFinalNewline_)+1);context.current=currentLine()+1;
        context.mark=[&](QChar)->std::optional<qint64>{return context.current;};
        const auto probe=exRegistry().parse(before+"print",context);
        if(probe.error.isEmpty() && probe.command.definition.name=="print"){
            for(const auto& candidate:exRegistry().complete(text.mid(start,cursor-start)))vimExCompletions_<<text.left(start)+candidate+text.mid(cursor);
        }else{
            const auto parsed=exRegistry().parse(text.left(cursor),context);
            if(!parsed.error.isEmpty() || parsed.command.definition.arguments!=VimEx::Arguments::File)return;
            auto from=cursor;while(from>0 && (!text[from-1].isSpace() || (from>1 && text[from-2]=='\\')))--from;
            const auto token=text.mid(from,cursor-from);QString error;
            const auto expanded=VimEx::fileArgument(token,filePath_,&error);if(!error.isEmpty())return;
            const auto absolute=token.isEmpty()?QDir::currentPath()+"/":expanded;
            const QFileInfo file(absolute);QDir directory(file.path());const auto prefix=token.isEmpty() || token.endsWith('/')?QString{}:file.fileName();
            if(token.isEmpty())directory=QDir::current();
            if(token.endsWith('/'))directory=QDir(absolute);
            QDirIterator entries(directory.path(),QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot);int scanned=0;
            while(entries.hasNext() && scanned++<2000){entries.next();const auto entry=entries.fileInfo();if(!entry.fileName().startsWith(prefix))continue;
                auto path=token.startsWith('/') || token.contains('%')?entry.absoluteFilePath():token.startsWith("~/")?QStringLiteral("~/")+QDir::home().relativeFilePath(entry.absoluteFilePath()):QDir::current().relativeFilePath(entry.absoluteFilePath());
                if(entry.isDir())path+='/';
                path.replace("\\","\\\\");
                for(const auto special:QStringLiteral(" |\"%#$*?`"))path.replace(QString(special),QStringLiteral("\\")+special);
                vimExCompletions_<<text.left(from)+path+text.mid(cursor);if(vimExCompletions_.size()>=200)break;
            }
            vimExCompletions_.sort();
        }
        vimExCompletionBase_=text;vimExCompletionIndex_=direction>0?-1:0;
    }
    if(vimExCompletions_.isEmpty())return;
    const auto count=vimExCompletions_.size();vimExCompletionIndex_=(vimExCompletionIndex_+direction+count)%count;
    vimExInput_->setText(vimExCompletions_[vimExCompletionIndex_]);
    vimExHints_->setText(vimExCompletions_.mid(0,6).join(QStringLiteral("   ")));
}
void EditorWidget::executeVimCommandLine(const QString& line){
    if(line.trimmed().isEmpty())return;
    auto& state=VimState::Session::instance();
    if(!vimSendingReplay_){state.commandHistory.removeAll(line);state.commandHistory.append(line);while(state.commandHistory.size()>200)state.commandHistory.removeFirst();}
    state.previousExCommand=state.lastCommand;state.lastCommand=line;state.set(':',VimState::text(line));
    if(vimExExecuting_ && vimExNormal_){
        ExFrame frame;frame.kind=ExFrame::ResumeNormal;frame.after=std::exchange(vimExRemaining_,{});frame.scope=std::exchange(vimExScope_,{});vimExFrames_.push_back(std::move(frame));vimExNormal_=false;
    }else {vimExSteps_=0;history_.beginCompoundGroup();}
    ++vimExGeneration_;vimExExecuting_=true;vimExRemaining_=line;continueVimEx();
}
void EditorWidget::scheduleVimEx(int delay){
    const auto generation=vimExGeneration_;
    QTimer::singleShot(delay,this,[this,generation]{if(generation==vimExGeneration_)continueVimEx();});
}
void EditorWidget::finishVimEx(const QString& error,bool fatal){
    if(!fatal && !error.isEmpty() && vimExIgnoreErrors()){restoreVimExScope(vimExScope_);scheduleVimEx();return;}
    if(!fatal && !error.isEmpty()){
        const auto global=std::find_if(vimExFrames_.rbegin(),vimExFrames_.rend(),[](const auto& frame){return frame.kind==ExFrame::Global;});
        if(global!=vimExFrames_.rend()){
            vimExMessage(error);restoreVimExScope(vimExScope_);
            const auto keep=std::size_t(std::distance(vimExFrames_.begin(),global.base()));
            int normalFrames=0;
            while(vimExFrames_.size()>keep){normalFrames+=vimExFrames_.back().kind==ExFrame::Normal;restoreVimExScope(vimExFrames_.back().scope);vimExFrames_.pop_back();}
            if(normalFrames){
                while(normalFrames && !vimReplay_.empty()){normalFrames-=vimReplay_.back().exNormal;vimReplay_.pop_back();}
                if(vimReplay_.empty())endVimReplayGroup();
                vimExNormal_=false;resetVimMode();
            }
            vimExRemaining_.clear();vimSubPlan_.reset();vimSubConfirm_=false;scheduleVimEx();return;
        }
    }
    const bool silent=vimExSilent();
    restoreVimExScope(vimExScope_);for(auto it=vimExFrames_.rbegin();it!=vimExFrames_.rend();++it)restoreVimExScope(it->scope);
    vimExFrames_.clear();vimExNormal_=false;
    ++vimExGeneration_;
    if(vimExExecuting_)history_.endCompoundGroup();
    vimExExecuting_=false;vimExRemaining_.clear();commitVimChangeLocation();resumeVimInsertNormal();
    vimSubConfirm_=false;vimSubPlan_.reset();vimSubGlobalLast_.reset();
    if(!error.isEmpty()){
        if(isVimReplaying())vimReplayCancel_=true;
        if(!silent)emit highlightingStatusChanged(error);
        if(!silent && vimExBar_){vimExInput_->parentWidget()->hide();vimExHints_->setText(error.left(4096));vimExBar_->show();positionVimCommandLine();}
    }
    emit vimModeChanged();
}
void EditorWidget::continueVimEx(){
    if(!vimExExecuting_ || vimExNormal_)return;
    if(busy_){scheduleVimEx(5);return;}
    restoreVimExScope(vimExScope_);
    if(vimExRemaining_.isEmpty()){if(nextVimExFrame())return;finishVimEx();return;}
    if(++vimExSteps_>100000){finishVimEx(I18n::text("Vim: Ex execution limit reached"),true);return;}
    {const auto& state=VimState::Session::instance();ExScope before;before.pattern=state.searchPattern;before.offset=state.searchOffset;before.subPattern=state.lastSubPattern;before.searchHistory=state.searchHistory;before.direction=state.searchForward;vimExParseState_=std::move(before);}
    VimEx::Context context;context.current=currentLine()+1;context.last=VimMotions::lastLine(buffer_,vimFinalNewline_)+1;
    QHash<QChar,qint64> marks;
    for(const auto name:QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ<>'[].^\"")){
        auto& state=VimState::Session::instance();const auto mark=name=='\''?state.jumps(window()).previous:name.isUpper()?state.globalMarks.value(name):state.marks(vimMarkKey()).value(name);
        if(mark && (mark->owner==document_.get() || (!mark->path.isEmpty() && mark->path==filePath_)))marks[name]=qint64(buffer_.lineIndexAt(std::min(mark->offset,buffer_.size()))+1);
    }
    context.mark=[marks](QChar name)->std::optional<qint64>{return marks.contains(name)?std::optional<qint64>(marks[name]):std::nullopt;};
    const auto line=std::exchange(vimExRemaining_,{});
    if(line.contains('/') || line.contains('?')){
        auto result=std::make_shared<VimEx::Parsed>();auto previous=std::make_shared<QString>(VimState::Session::instance().searchPattern);
        auto direction=std::make_shared<bool>(VimState::Session::instance().searchForward),used=std::make_shared<bool>(false);
        const auto options=VimState::Session::instance().searchOptions;const auto old=vimSearchSubjectRevision_==lspRevision_?vimSearchSubject_:nullptr;
        const bool finalNewline=vimFinalNewline_,wrap=VimState::Session::instance().wrapSearch;const auto original=cursorPosition_;
        runBulkOperation(I18n::text("Searching…"),[line,context,result,previous,direction,used,options,old,finalNewline,wrap,original](PieceTreeBuffer& buffer,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep& step) mutable {
            auto subject=old;
            context.search=[&](const QString& query,bool forward,qint64 row)->std::optional<qint64>{
                const auto pattern=query.isEmpty()?*previous:query;if(pattern.isEmpty())throw std::runtime_error("E35: No previous regular expression");
                *direction=forward;*used=true;
                VimSearch::Pattern expression(pattern,options);if(!expression.error().isEmpty())throw std::runtime_error(expression.error().toStdString());
                if(!subject)subject=std::make_shared<VimSearch::Subject>(buffer,finalNewline,[&]{step(10);});
                const auto at=forward?VimMotions::lineEnd(buffer,std::size_t(std::max<qint64>(1,row)-1)):VimMotions::lineStart(buffer,std::size_t(std::max<qint64>(1,row)-1));
                const auto found=VimSearch::navigate(*subject,expression,at,forward,1,[&]{step(50);});*previous=pattern;
                if(!found.match || (!wrap && found.wrapped))return {};
                return qint64(buffer.lineIndexAt(found.match->begin)+1);
            };
            *result=exRegistry().parse(line,context);return HistoryResult{original,{original}};
        },false,{},[this,result,previous,direction,used](bool ok){if(!ok)return;if(*used && !previous->isEmpty()){auto& state=VimState::Session::instance();state.searchPattern=*previous;state.searchOffset.clear();state.searchForward=*direction;state.set('/',VimState::text(*previous));state.searchHistory.removeAll(*previous);state.searchHistory.append(*previous);while(state.searchHistory.size()>200)state.searchHistory.removeFirst();vimLastSearchMatch_.reset();}executeVimExParsed(*result);});return;
    }
    executeVimExParsed(exRegistry().parse(line,context));
}
void EditorWidget::executeVimExParsed(const VimEx::Parsed& parsed){
    const auto& modifiers=parsed.command;
    if(modifiers.silent || modifiers.ignoreErrors || modifiers.keepJumps || modifiers.keepPatterns){
        const auto& state=VimState::Session::instance();ExScope scope;scope.silent=modifiers.silent;scope.ignoreErrors=modifiers.ignoreErrors;scope.keepJumps=modifiers.keepJumps;scope.keepPatterns=modifiers.keepPatterns;
        if(scope.keepPatterns){scope.pattern=vimExParseState_?vimExParseState_->pattern:state.searchPattern;scope.offset=vimExParseState_?vimExParseState_->offset:state.searchOffset;scope.subPattern=vimExParseState_?vimExParseState_->subPattern:state.lastSubPattern;scope.searchHistory=vimExParseState_?vimExParseState_->searchHistory:state.searchHistory;scope.direction=vimExParseState_?vimExParseState_->direction:state.searchForward;}vimExScope_=std::move(scope);
    }
    vimExRemaining_=parsed.rest;
    if(!parsed.error.isEmpty()){vimExRemaining_.clear();finishVimEx(parsed.error);return;}
    if(parsed.empty){finishVimEx();return;}
    vimExRemaining_=parsed.rest;const auto command=parsed.command;const auto name=command.definition.name;
    const auto first=std::size_t(std::max<qint64>(1,command.first)-1),last=std::size_t(std::max<qint64>(1,command.last)-1);
    if(command.definition.arguments==VimEx::Arguments::Substitute){beginVimSubstitute(command);return;}
    if(command.definition.arguments==VimEx::Arguments::Global){beginVimGlobal(command);return;}
    if(name=="normal"){beginVimNormal(command);return;}
    if(name=="@") {executeVimExRegister(command);return;}
    if(name=="copy" || name=="t" || name=="move" || name=="join" || name=="sort"){editVimExLines(command);return;}
    if(name=="read"){readVimExFile(command);return;}
    if(name=="mark" || name=="k"){
        if(command.registerName.isNull() || !command.registerName.isLetter()){finishVimEx("E191: Argument must be a letter");return;}
        auto& state=VimState::Session::instance();auto location=state.anchor(document_.get(),filePath_,vimLineStart(last),QPoint(int(last+1),1));
        if(command.registerName.isUpper())state.globalMarks[command.registerName]=location;else state.marks(vimMarkKey())[command.registerName]=location;
        scheduleVimEx();return;
    }
    if(name=="registers" || name=="display"){
        VimState::Session::instance().set('%',VimState::text(filePath_));VimState::Session::instance().set('#',VimState::text(property("vimAlternatePath").toString()));
        const auto names=command.arguments.isEmpty()?QStringLiteral("\"0123456789abcdefghijklmnopqrstuvwxyz-/.:"):command.arguments;QString output;
        for(const auto reg:names){const auto value=VimState::Session::instance().read(reg);if(!value.text)continue;const auto bytes=value.text->read(0,std::min<std::size_t>(256,value.text->size()));auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));text.replace('\n',QStringLiteral("^J"));output+='"'+QString(reg)+"   "+text+'\n';if(output.size()>4096)break;}
        vimExMessage(output.left(4096));scheduleVimEx();return;
    }
    if(name=="nohlsearch"){vimHighlightHidden_=true;if(vimSearchCancelled_)vimSearchCancelled_->store(true);++vimSearchGeneration_;vimSearchHighlights_.clear();viewport()->update();}
    else if(name=="set"){
        auto& state=VimState::Session::instance();
        for(auto option:command.arguments.split(' ',Qt::SkipEmptyParts)){
            bool value=true,toggle=option.endsWith('!'),report=option.endsWith('?');if(toggle || report)option.chop(1);
            if(option.startsWith("no")){value=false;option.remove(0,2);}
            QString* source=nullptr;const auto optionName=option.section('=',0,0);
            if(optionName=="dictionary" || optionName=="dict")source=&state.dictionary;
            else if(optionName=="thesaurus" || optionName=="tsr")source=&state.thesaurus;
            else if(optionName=="tags" || optionName=="tag")source=&state.tags;
            if(source){if(option.contains('='))*source=option.mid(option.indexOf('=')+1);else vimExMessage(optionName+"="+*source);continue;}
            if(option.startsWith("nrformats=") || option.startsWith("nf=")){
                const auto formats=option.mid(option.indexOf('=')+1).split(',',Qt::SkipEmptyParts);for(const auto& format:formats)if(!QStringList{"bin","hex","octal","alpha","unsigned"}.contains(format)){finishVimEx("E474: Invalid argument");return;}state.numberFormats=formats.join(',');continue;
            }
            if(option=="nrformats" || option=="nf"){vimExMessage("nrformats="+state.numberFormats);continue;}
            if(option.startsWith("textwidth=") || option.startsWith("tw=")){bool ok=false;const auto width=option.mid(option.indexOf('=')+1).toInt(&ok);if(!ok || width<0 || width>1000000){finishVimEx("E474: Invalid argument");return;}state.textWidth=width;continue;}
            if(option=="textwidth" || option=="tw"){vimExMessage("textwidth="+QString::number(state.textWidth));continue;}
            bool* target=nullptr;
            if(option=="wrap" || option=="foldenable" || option=="fen"){
                auto& setting=option=="wrap"?vimWrap_:vimFoldEnabled_;
                if(report)vimExMessage((setting?QString{}:QStringLiteral("no"))+option);
                else {
                    const int top=documentLineForVisualRow(verticalScrollBar()->value());
                    setting=toggle?!setting:value;
                    refreshScreenLayout(top);
                    const int line=documentLineForVisualRow(visualRowForLine(int(currentLine())));
                    if(line!=int(currentLine()))moveCursor(VimMotions::firstText(buffer_,line),false);
                    ensureCursorVisible();
                }
                continue;
            }
            if(option=="foldmethod=manual" || option=="fdm=manual")continue;
            if(option=="foldmethod" || option=="fdm"){vimExMessage("foldmethod=manual");continue;}
            if(option=="joinspaces" || option=="js")target=&state.joinSpaces;
            else if(option=="ignorecase" || option=="ic")target=&state.searchOptions.ignoreCase;
            else if(option=="smartcase" || option=="scs")target=&state.searchOptions.smartCase;
            else if(option=="magic")target=&state.searchOptions.magic;
            else if(option=="hlsearch" || option=="hls")target=&state.highlightSearch;
            else if(option=="incsearch" || option=="is")target=&state.incrementalSearch;
            else if(option=="wrapscan" || option=="ws")target=&state.wrapSearch;
            else if(option=="gdefault" || option=="gd")target=&state.globalSubstitute;
            if(!target){finishVimEx("E518: Unknown option: "+option);return;}
            if(report)vimExMessage((*target?QString{}:QStringLiteral("no"))+option);
            else *target=toggle?!*target:value;
        }
        vimHighlightHidden_=false;invalidateVimSearch();
    }
    else if(name=="fold"){createVimFold(int(first),int(last));}
    else if(name.isEmpty()){pushVimJump();moveCursor(VimMotions::firstText(buffer_,last),false);}
    else if(name=="undo" || name=="redo"){history_.endCompoundGroup();history_.endGroup();replayVimHistory(name=="redo",1);history_.beginCompoundGroup();}
    else if(name=="delete" || name=="yank"){
        if(QStringLiteral(".%#/: ").contains(command.registerName) && !command.registerName.isNull()){finishVimEx("E354: Invalid register name");return;}
        vimRegister_=command.registerName;history_.endGroup();
        editVimRange(vimLineStart(first),last+1<buffer_.lineCount()?buffer_.lineStart(last+1):buffer_.size(),name=="delete"?VimController::Command::Delete:VimController::Command::Yank,true);
    }else if(name=="put"){
        if(command.registerName=='%' || command.registerName=='#')VimState::Session::instance().set(command.registerName,VimState::text(command.registerName=='%'?filePath_:property("vimAlternatePath").toString()));
        auto value=VimState::Session::instance().read(command.registerName);
        if(value.empty()){finishVimEx("E353: Nothing in register");return;}
        value.kind=VimState::Register::Line;vimPutValue_=value;
        moveCursor(vimLineStart(last),false);history_.endGroup();pasteVim(command.bang || command.last==0,1,true);
    }else if(name==">" || name=="<"){
        if(!command.registerName.isNull()){finishVimEx("E488: Trailing characters");return;}history_.endGroup();indentVim(first,last,name=="<");
    }else if(name=="=" || name=="print" || name=="number" || name=="list" || (name=="file" && command.arguments.isEmpty())){
        QString output;
        if(name=="=")output=QString::number(command.last);
        else if(name=="file")output=QStringLiteral("%1%2 · %3").arg(filePath_.isEmpty()?I18n::text("Untitled"):filePath_,modified_?QStringLiteral(" [+]"):QString{},QString::number(VimMotions::lastLine(buffer_,vimFinalNewline_)+1));
        else {
            for(auto row=first;row<=last && output.size()<4096;++row){
                const auto start=vimLineStart(row),length=std::min<std::size_t>(lineContentEnd(row)-start,4096);
                const auto bytes=buffer_.slice(start,length);auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));
                if(name=="list"){text.replace("\t","^I");text+='$';}
                if(name=="number")text=QString::number(row+1)+"  "+text;
                if(row!=first)output+='\n';
                output+=text;
            }
            if(command.last-command.first>100 || output.size()>4096)output=output.left(4096)+QStringLiteral("\n…");
            moveCursor(VimMotions::firstText(buffer_,last),false);
        }
        vimExMessage(output);
    }else{
        if(!isSignalConnected(QMetaMethod::fromSignal(&EditorWidget::vimExRequested))){finishVimEx(I18n::text("Vim: this command requires an application window"));return;}
        QPointer<EditorWidget> guard(this);const auto generation=vimExGeneration_;
        emit vimExRequested(command,[guard,generation](QString error){if(!guard || generation!=guard->vimExGeneration_)return;if(!error.isEmpty())guard->finishVimEx(error);else guard->scheduleVimEx();});return;
    }
    scheduleVimEx();
}
