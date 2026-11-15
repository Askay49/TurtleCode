#include "EditorWidget.hpp"
#include "app/I18n.hpp"
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QTimer>
#include <algorithm>

bool EditorWidget::vimExSilent() const {if(vimExScope_ && vimExScope_->silent)return true;for(const auto& frame:vimExFrames_)if(frame.scope && frame.scope->silent)return true;return false;}
bool EditorWidget::vimExIgnoreErrors() const {if(vimExScope_ && vimExScope_->ignoreErrors)return true;for(const auto& frame:vimExFrames_)if(frame.scope && frame.scope->ignoreErrors)return true;return false;}
void EditorWidget::restoreVimExScope(std::optional<ExScope>& scope){
    if(scope && scope->keepPatterns){auto& state=VimState::Session::instance();state.searchPattern=scope->pattern;state.searchOffset=scope->offset;state.lastSubPattern=scope->subPattern;state.searchHistory=scope->searchHistory;state.searchForward=scope->direction;state.set('/',VimState::text(state.searchPattern));}
    scope.reset();
}
void EditorWidget::vimExMessage(const QString& message){
    if(vimExSilent())return;
    emit highlightingStatusChanged(message);
    if(vimExBar_){vimExInput_->parentWidget()->hide();vimExHints_->setText(message.left(4096));vimExBar_->show();positionVimCommandLine();}
}
void EditorWidget::projectVimExLines(std::size_t row,std::size_t column,std::size_t removed,std::size_t inserted){for(auto& frame:vimExFrames_)if(frame.marks)frame.marks->edit(qint64(row),qint64(column),qint64(removed),qint64(inserted));}
void EditorWidget::cancelVimExFrames(){for(auto it=vimExFrames_.rbegin();it!=vimExFrames_.rend();++it)restoreVimExScope(it->scope);vimExFrames_.clear();}
void EditorWidget::renameVimAnchors(const QString& path){const auto key=path.isEmpty()?QStringLiteral("buffer:%1").arg(quintptr(this)):path;VimState::Session::instance().rename(this,vimMarkKey(),key,path);}
bool EditorWidget::nextVimExFrame(){
    while(!vimExFrames_.empty()){
        auto& frame=vimExFrames_.back();
        if(frame.kind==ExFrame::ResumeNormal){
            vimExRemaining_=std::move(frame.after);vimExScope_=std::move(frame.scope);vimExFrames_.pop_back();vimExNormal_=true;
            if(!vimReplayScheduled_){vimReplayScheduled_=true;QTimer::singleShot(0,this,&EditorWidget::continueVimReplay);}return true;
        }
        if(frame.kind==ExFrame::Register){
            if(frame.index<frame.lines.size()){vimExRemaining_=frame.lines[frame.index++];scheduleVimEx();return true;}
        }else if(const auto row=frame.marks->takeFirst()){
            const auto target=std::min<std::size_t>(std::size_t(*row),VimMotions::lastLine(buffer_,vimFinalNewline_));
            if(!frame.preserveColumn)moveCursor(vimLineStart(target),false);
            if(frame.kind==ExFrame::Global){vimExRemaining_=frame.body;scheduleVimEx();return true;}
            auto inputs=VimState::keys(frame.body);VimState::Input escape;escape.key=Qt::Key_Escape;inputs.append(escape);
            vimExNormal_=true;replayVim(std::move(inputs));if(!vimReplay_.empty())vimReplay_.back().exNormal=true;return true;
        }
        auto after=std::move(frame.after);restoreVimExScope(frame.scope);vimExFrames_.pop_back();
        if(!after.isEmpty()){vimExRemaining_=std::move(after);scheduleVimEx();return true;}
    }
    return false;
}
void EditorWidget::beginVimGlobal(const VimEx::Invocation& command){
    if(vimExFrames_.size()>=100){finishVimEx("E169: Command too recursive",true);return;}
    const bool nested=std::any_of(vimExFrames_.begin(),vimExFrames_.end(),[](const auto& frame){return frame.kind==ExFrame::Global;});
    if(nested && command.explicitRange){finishVimEx("E147: Cannot do :global recursive with a range");return;}
    auto spec=VimEx::parseGlobal(command.arguments);if(!spec.error.isEmpty()){finishVimEx(spec.error);return;}
    auto& state=VimState::Session::instance();if(spec.pattern.isEmpty())spec.pattern=state.searchPattern;
    if(spec.pattern.isEmpty()){finishVimEx("E35: No previous regular expression");return;}
    state.searchPattern=state.lastSubPattern=spec.pattern;state.searchOffset.clear();state.set('/',VimState::text(spec.pattern));
    state.searchHistory.removeAll(spec.pattern);state.searchHistory.append(spec.pattern);while(state.searchHistory.size()>200)state.searchHistory.removeFirst();
    const auto first=nested?currentLine():std::size_t(command.first-1),last=nested?currentLine():std::size_t(command.last-1);
    const bool invert=(command.definition.name=="vglobal")!=command.bang,finalNewline=vimFinalNewline_;
    const auto options=state.searchOptions;const auto old=vimSearchSubjectRevision_==lspRevision_?vimSearchSubject_:nullptr;
    const auto original=cursorPosition_;auto marks=std::make_shared<VimEx::LineMarks>();auto count=std::make_shared<std::size_t>(0);auto selectedLast=std::make_shared<std::size_t>(0);
    runBulkOperation(I18n::text("Selecting global lines…"),[marks,count,selectedLast,first,last,invert,finalNewline,options,pattern=spec.pattern,old,original](PieceTreeBuffer& buffer,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        VimSearch::Pattern expression(pattern,options);if(!expression.error().isEmpty())throw std::runtime_error(expression.error().toStdString());
        const auto subject=old?old:std::make_shared<VimSearch::Subject>(buffer,finalNewline,[&]{step(10);});
        auto match=expression.find(*subject,VimMotions::lineStart(buffer,first),[&]{step(30);});
        for(auto row=first;row<=last;++row){step(50);const bool matches=match && buffer.lineIndexAt(match->begin)==row;
            if(matches!=invert){if(++*count>100000)throw std::runtime_error("Vim: global selects more than 100000 lines");marks->add(qint64(row));*selectedLast=row;}
            if(matches && row<last)match=expression.find(*subject,buffer.lineStart(row+1),[&]{step(70);});
        }
        return HistoryResult{original,{original}};
    },false,{},[this,marks,count,selectedLast,spec,nested](bool ok){
        if(!ok)return;
        if(!*count){if(!nested)vimExMessage(I18n::text("No matching global lines"));scheduleVimEx();return;}
        if(!nested)pushVimJump();
        VimEx::Context context;context.current=currentLine()+1;context.last=VimMotions::lastLine(buffer_,vimFinalNewline_)+1;
        auto body=VimEx::Registry{}.parse(spec.commands,context);const auto substitute=VimSearch::parseSubstitute(body.command.arguments);
        const auto pattern=substitute.pattern.isEmpty()?VimState::Session::instance().searchPattern:substitute.pattern;
        const bool plainFlags=std::all_of(substitute.flags.begin(),substitute.flags.end(),[](QChar ch){return QStringLiteral("giIe").contains(ch);});
        // One-line patterns/replacements commute on the frozen global selection.
        // Other bodies, confirmations and multiline expressions use the ordinary interpreter.
        if(!nested && body.error.isEmpty() && body.rest.isEmpty() && !body.command.explicitRange && body.command.definition.arguments==VimEx::Arguments::Substitute &&
           !body.command.silent && !body.command.keepJumps && !body.command.keepPatterns && !substitute.repeat && substitute.error.isEmpty() && plainFlags &&
           !pattern.contains('\\') && !pattern.contains('\n') && !substitute.replacement.contains('\\') && !substitute.replacement.contains('\n') && !substitute.replacement.contains('\r') && !substitute.replacement.contains('~') && substitute.replacement!="%"){
            auto selection=*marks;const auto first=*selection.takeFirst();moveCursor(vimLineStart(std::size_t(first)),false);
            body.command.first=first+1;body.command.last=qint64(*selectedLast+1);beginVimSubstitute(body.command,marks);return;
        }
        ExFrame frame;frame.kind=ExFrame::Global;frame.marks=marks;frame.body=spec.commands;frame.after=std::exchange(vimExRemaining_,{});frame.scope=std::exchange(vimExScope_,{});vimExFrames_.push_back(std::move(frame));scheduleVimEx();
    });
}
void EditorWidget::beginVimNormal(const VimEx::Invocation& command){
    if(command.arguments.isEmpty()){finishVimEx("E471: Argument required");return;}
    if(vimExFrames_.size()>=100){finishVimEx("E169: Command too recursive",true);return;}
    const auto first=command.explicitRange?command.first-1:qint64(currentLine()),last=command.explicitRange?command.last-1:first;
    if(last-first>=100000){finishVimEx(I18n::text("Vim: Ex execution limit reached"));return;}
    ExFrame frame;frame.kind=ExFrame::Normal;frame.marks=std::make_shared<VimEx::LineMarks>();for(auto row=first;row<=last;++row)frame.marks->add(row);
    frame.preserveColumn=!command.explicitRange;frame.body=command.arguments;frame.after=std::exchange(vimExRemaining_,{});frame.scope=std::exchange(vimExScope_,{});vimExFrames_.push_back(std::move(frame));scheduleVimEx();
}
void EditorWidget::executeVimExRegister(const VimEx::Invocation& command){
    auto reg=command.registerName;auto& state=VimState::Session::instance();if(reg.isNull() || reg=='@')reg=state.lastExRegister;
    if(reg==':' && state.previousExCommand.isEmpty()){finishVimEx("E30: No previous command line");return;}
    const auto value=reg==':'?VimState::text(state.previousExCommand):state.read(reg);
    if(!value.text || !value.text->size()){finishVimEx("E748: No previously used register");return;}
    if(value.text->size()>1024*1024 || vimExFrames_.size()>=100){finishVimEx("E169: Command too recursive or register too large",true);return;}
    if(reg==':'){state.lastCommand=state.previousExCommand;state.set(':',VimState::text(state.lastCommand));}
    state.lastExRegister=reg;const auto bytes=value.text->read(0,value.text->size());auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));text.replace("\r\n","\n");text.replace('\r','\n');
    moveCursor(VimMotions::firstText(buffer_,std::size_t(command.last-1)),false);
    ExFrame frame;frame.kind=ExFrame::Register;frame.lines=text.split('\n');frame.after=std::exchange(vimExRemaining_,{});frame.scope=std::exchange(vimExScope_,{});vimExFrames_.push_back(std::move(frame));scheduleVimEx();
}
void EditorWidget::transferVimEx(EditorWidget* target){
    if(!target || target==this || !vimExExecuting_)return;
    if(!target->vimExBar_){target->showVimCommandLine();target->closeVimCommandLine();}
    transferVimReplay(target);history_.endCompoundGroup();
    target->vimExRemaining_=std::move(vimExRemaining_);target->vimExScope_=std::move(vimExScope_);target->vimExSteps_=vimExSteps_;
    target->vimExExecuting_=true;++target->vimExGeneration_;target->history_.beginCompoundGroup();
    // Line selections belong to this document; register programs can continue in another.
    for(auto& frame:vimExFrames_)if(frame.kind!=ExFrame::Register)frame.marks=std::make_shared<VimEx::LineMarks>();
    target->vimExFrames_=std::move(vimExFrames_);target->vimExNormal_=vimExNormal_;vimExNormal_=false;
    vimExExecuting_=false;++vimExGeneration_;target->scheduleVimEx();emit vimModeChanged();
}
