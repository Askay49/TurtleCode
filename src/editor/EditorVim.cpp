#include "EditorWidget.hpp"
#include "app/I18n.hpp"
#include <QApplication>
#include <algorithm>
#include <QInputMethod>
#include <QKeyEvent>
#include <QSettings>
#include <QShortcut>
#include <QScrollBar>
#include <QClipboard>
#include <QScopeGuard>
#include "core/text/ChunkedFile.hpp"

QString EditorWidget::vimModeName() const {
    const auto name=vimSubConfirm_?QStringLiteral("CONFIRM"):vimExActive_?QStringLiteral("COMMAND"):vimVisualBlock_?QStringLiteral("V-BLOCK"):vimVisualLine_ && vim_.mode()==VimController::Mode::Visual?QStringLiteral("V-LINE"):vim_.modeName();
    const auto recording=VimState::Session::instance().recording;
    return recording.isNull()?name:I18n::text("%1 · recording @%2").arg(name,QString(recording));
}

std::size_t EditorWidget::vimLineStart(std::size_t line) const {
    const auto start=buffer_.lineStart(line);
    return start==0 && buffer_.size()>=3 && buffer_.slice(0,3)=="\xef\xbb\xbf"?3:start;
}
void EditorWidget::normalizeVimCursor() {
    if(!vim_.enabled() || vim_.acceptsText())return;
    const auto line=std::min(VimMotions::lastLine(buffer_,vimFinalNewline_),buffer_.lineIndexAt(std::min(cursorPosition_,buffer_.size())));
    const auto start=vimLineStart(line),end=lineContentEnd(line);
    auto position=std::clamp(cursorPosition_,start,std::max(start,end));
    if(position==end && end>start)position=graphemeBoundaryAt(position,false);
    if(position<end && static_cast<unsigned char>(buffer_.byteAt(position))>=128)position=graphemeBoundaryAt(graphemeBoundaryAt(position,true),false);
    cursorPosition_=position;cursors_={position};cursorAnchors_={anchor_};
}
void EditorWidget::refreshVimPresentation() {
    const bool accepts=vim_.acceptsText();
    setAttribute(Qt::WA_InputMethodEnabled,accepts);
    if(!accepts) {
        preeditText_.clear();preeditCursor_=0;
        if(hasFocus() && QGuiApplication::inputMethod())QGuiApplication::inputMethod()->reset();
    }
    if(QGuiApplication::inputMethod() && hasFocus())QGuiApplication::inputMethod()->update(Qt::ImEnabled|Qt::ImCursorRectangle);
    viewport()->update();
    emit vimModeChanged();
}
void EditorWidget::setVimEnabled(bool enabled) {
    if(vim_.enabled()==enabled)return;
    if(enabled)loadVimInsertMappings();
    else flushVimInsertMapping();
    if(!enabled){closeVimCommandLine();invalidateVimSearch();stopVimExecution();if(vimExExecuting_)finishVimEx();}
    history_.endGroup();hideCompletions();
    // Preserve the exact selected bytes when returning to ordinary editing.
    const auto selection=selectionRange();const bool selected=hasSelection();const bool reversed=anchor_ && cursorPosition_<*anchor_;
    if(!enabled){cancelVimReplay();finishVimChange();if(VimState::Session::instance().recordingOwner==this)toggleVimRecording();}
    vimInsertExitAfterPut_=vimInsertNormal_=vimInsertNormalArmed_=vimInsertCtrlX_=vimInsertCtrlG_=vimInsertJoinMovement_=false;vimInsertSavedIndent_.clear();vimLiteral_.clear();vimDigraph_.clear();vimInsertCandidates_.clear();
    vimVisualLine_=vimVisualBlock_=false;vim_.setEnabled(enabled);vimPreferredX_=-1;vimHistoryState_=history_.stateId();
    cursors_={cursorPosition_};clearAllSelections();
    if(enabled)normalizeVimCursor();
    else if(selected){anchor_=reversed?selection.second:selection.first;cursorPosition_=reversed?selection.first:selection.second;cursors_={cursorPosition_};cursorAnchors_={anchor_};}
    for(auto* shortcut:findChildren<QShortcut*>())if(shortcut->objectName().startsWith("editor.vimDisabled."))shortcut->setEnabled(!enabled);
    refreshVimPresentation();
    invalidateScreenMinimap();updateScrollBars();ensureCursorVisible();
}
void EditorWidget::switchVimMode(VimController::Mode mode,bool fromEscape) {
    const auto previous=vim_.mode();
    if((previous==VimController::Mode::Insert || previous==VimController::Mode::Replace) && mode==VimController::Mode::Normal)VimState::Session::instance().marks(vimMarkKey())['^']=vimLocation();
    if((previous==VimController::Mode::Insert || previous==VimController::Mode::Replace) && fromEscape && mode==VimController::Mode::Normal && repeatVimInsert())return;
    vimInsertCount_=1;vimInsertPrefix_.clear();
    vimInsertModeToggled_=false;vimCountedInsertKeys_.clear();vimReplaced_.clear();
    history_.endGroup();hideCompletions();vimPreferredX_=-1;
    if(previous==VimController::Mode::Visual && anchor_) {
        vimPreviousVisualAnchor_=VimState::Session::instance().anchor(document_.get(),filePath_,*anchor_,navigationPosition());
        vimPreviousVisualHead_=vimLocation();vimPreviousVisualLine_=vimVisualLine_;vimPreviousVisualBlock_=vimVisualBlock_;
        vimPreviousBlockAnchor_=vimBlockAnchorColumn_;vimPreviousBlockAnchorEnd_=vimBlockAnchorEnd_;vimPreviousBlockHead_=vimBlockVirtualColumn_;vimPreviousVisualToEnd_=vimBlockToEnd_;
        const auto [a,b]=selectionRange();
        auto& marks=VimState::Session::instance().marks(vimMarkKey());
        marks['<']=VimState::Session::instance().anchor(document_.get(),filePath_,a,QPoint(1,1));marks['>']=VimState::Session::instance().anchor(document_.get(),filePath_,b>a?graphemeBoundaryAt(b,false):b,QPoint(1,1));
    }
    vimVisualLine_=false;vimVisualBlock_=false;
    if(mode==VimController::Mode::Normal) {
        vimInsertCtrlX_=vimInsertCtrlG_=vimInsertJoinMovement_=false;vimInsertSavedIndent_.clear();vimLiteral_.clear();vimDigraph_.clear();vimInsertCandidates_.clear();
        clearAllSelections();
        if(fromEscape && vimBlockInsert_ && !vimBlockChange_)cursorPosition_=vimBlockExitPosition_;
        else if(fromEscape && (previous==VimController::Mode::Insert || previous==VimController::Mode::Replace) && cursorPosition_>vimLineStart(currentLine()))cursorPosition_=graphemeBoundaryAt(cursorPosition_,false);
        vimBlockInsert_=vimBlockChange_=false;
    } else if(mode==VimController::Mode::Visual) {
        normalizeVimCursor();
        cursors_={cursorPosition_};clearAllSelections();anchor_=cursorPosition_;cursorAnchors_={anchor_};
    } else clearAllSelections();
    vim_.setMode(mode);vimHistoryState_=history_.stateId();
    if(!vim_.acceptsText())normalizeVimCursor();
    else {cursors_={cursorPosition_};cursorAnchors_={std::nullopt};history_.beginGroup(true);vimChangeInserted_=true;vimInsertedText_.clear();vimInsertStart_=cursorPosition_;}
    refreshVimPresentation();updateScrollBars();ensureCursorVisible();
    if(mode==VimController::Mode::Normal && !vimHandlingKey_ && !busy_)finishVimChange();
}
bool EditorWidget::handleVimKey(QKeyEvent* event) {
    using C=VimController::Command;using Mode=VimController::Mode;
    if(handleVimInsertKey(event))return true;
    const auto action=vim_.feed(*event);const auto command=action.command;
    if(!action.registerName.isNull())vimRegister_=action.registerName;
    else if(command!=C::Pending && command!=C::Pass)vimRegister_={};
    if(vim_.enabled())emit vimModeChanged();
    if(command==C::Pass) {
        if(vimVisualBlock_ && event->matches(QKeySequence::Copy)){vimRegister_='+';editVimBlock(C::Yank);return true;}
        if(vim_.mode()==Mode::Replace) {
            if(event->key()==Qt::Key_Backspace){backspaceVimReplace();return true;}
            if(event->key()==Qt::Key_Tab && event->modifiers()==Qt::NoModifier){refreshEditorConfig();const auto at=cursorPosition_,end=std::min(lineContentEnd(currentLine()),graphemeBoundaryAt(at,true));const auto removed=buffer_.slice(at,end-at);const auto inserted=editorConfig_.tabs.value_or(false)?std::string("\t"):std::string(editorConfig_.indentation(),' ');replaceRange(at,end-at,inserted);vimReplaced_.push_back({at,removed,inserted});return true;}
            if(!(event->modifiers()&(Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier)) && !event->text().isEmpty() && event->text()[0].isPrint()){replaceVimText(event->text());return true;}
            if(event->key()==Qt::Key_Left || event->key()==Qt::Key_Right || event->key()==Qt::Key_Up || event->key()==Qt::Key_Down)vimReplaced_.clear();
        }
        if(!vim_.acceptsText())switch(event->key()) {
        case Qt::Key_Backspace:case Qt::Key_Delete:case Qt::Key_Return:case Qt::Key_Enter:case Qt::Key_Tab:case Qt::Key_Backtab:event->accept();return true;
        default:break;
        }
        if(vim_.enabled()) {
            if(vim_.mode()==Mode::Insert) {
                const bool motion=event->key()==Qt::Key_Left || event->key()==Qt::Key_Right || event->key()==Qt::Key_Up || event->key()==Qt::Key_Down || event->key()==Qt::Key_Home || event->key()==Qt::Key_End;
                if((motion && !vimInsertJoinedMovement_) || event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo)){history_.endGroup();vimInsertCount_=1;vimReplaced_.clear();}
                else {if(event->key()==Qt::Key_Backspace && cursorPosition_<=vimInsertStart_)vimInsertCount_=1;history_.beginGroup(true);}
            }
            if(vim_.mode()==Mode::Visual && (event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo)))switchVimMode(Mode::Normal);
        }
        return false;
    }
    event->accept();
    if(command==C::Consume || command==C::Pending)return true;
    if (command == C::Workspace) {
        emit vimWorkspaceRequested(action.text, action.count, action.explicitCount);
        return true;
    }
    if(command!=C::ScreenUp && command!=C::ScreenDown)vimScreenPreferredX_=-1;
    if(vimInsertNormal_)vimInsertNormalEnd_=command==C::End || command==C::LastText || ((command==C::Up || command==C::Down) && vimPreferredX_<0 && cursorPosition_==graphemeBoundaryAt(lineContentEnd(currentLine()),false));
    if(command==C::RestoreVisual) {
        if(vimPreviousVisualAnchor_ && vimPreviousVisualHead_) {
            const auto a=vimPreviousVisualAnchor_->offset,h=vimPreviousVisualHead_->offset;
            const bool line=vimPreviousVisualLine_,block=vimPreviousVisualBlock_,toEnd=vimPreviousVisualToEnd_;
            const int column=vimPreviousBlockAnchor_,anchorEnd=vimPreviousBlockAnchorEnd_,head=vimPreviousBlockHead_;
            if(vim_.mode()==Mode::Visual)switchVimMode(Mode::Normal);
            switchVimMode(Mode::Visual);anchor_=std::min(a,buffer_.size());cursorPosition_=std::min(h,buffer_.size());
            vimVisualLine_=line;vimVisualBlock_=block;cursors_={cursorPosition_};cursorAnchors_={anchor_};
            vimBlockAnchorColumn_=column;vimBlockAnchorEnd_=anchorEnd;vimBlockVirtualColumn_=head;vimBlockToEnd_=toEnd;
            ensureCursorVisible();refreshVimPresentation();
        }return true;
    }
    if(command==C::InsertPrevious) {
        const auto mark=VimState::Session::instance().marks(vimMarkKey()).value('^');
        if(mark)cursorPosition_=std::min(mark->offset,buffer_.size());
        beginVimChange();switchVimMode(Mode::Insert);return true;
    }
    if(command==C::SelectMatch){startVimSearch(action);return true;}
    if(command==C::CommandLine){showVimCommandLine(action.explicitCount?action.count:0);return true;}
    if(command==C::RepeatSubstitute){showVimCommandLine();closeVimCommandLine();executeVimCommandLine(action.around?"%s&":"s");return true;}
    if(!action.registerName.isNull() && QStringLiteral(".%#/: ").contains(action.registerName) && (action.operation!=C::Consume || command==C::Delete || command==C::Change || command==C::Yank || command==C::DeleteChar || command==C::DeleteBefore || command==C::Substitute || command==C::DeleteEnd || command==C::ChangeEnd || command==C::ChangeLine || command==C::YankLine)){
        if(!vimReplay_.empty())vimReplayCancel_=true;
        emit highlightingStatusChanged(I18n::text("Vim: register is read-only"));return true;
    }
    if(command==C::SearchForward || command==C::SearchBackward || command==C::SearchNext || command==C::SearchPrevious || command==C::SearchWord || command==C::SearchWordBack){startVimSearch(action);return true;}
    const auto transform=[](C command){return command==C::ToggleCase || command==C::LowerCase || command==C::UpperCase || command==C::AutoIndent || command==C::Format;};
    const bool mutating=transform(action.operation) || transform(command) || command==C::Join || command==C::Increment || command==C::Decrement ||action.operation==C::Delete || action.operation==C::Change || action.operation==C::Indent || action.operation==C::Unindent ||
        command==C::InsertZero || command==C::Insert || command==C::Append || command==C::InsertFirst || command==C::InsertLast || command==C::OpenAbove || command==C::OpenBelow || command==C::Delete || command==C::Change || command==C::Indent || command==C::Unindent || command==C::DeleteChar || command==C::DeleteBefore || command==C::Substitute || command==C::ChangeLine || command==C::ChangeEnd || command==C::DeleteEnd || command==C::PasteBefore || command==C::PasteAfter || command==C::ReplaceMode || command==C::ReplaceCharacter;
    if(mutating)beginVimChange();
    if(handleVimViewport(action))return true;
    if(command==C::ChangeBack || command==C::ChangeForward){if(action.operation==C::Consume)jumpVimChange(command==C::ChangeBack?-1:1,action.count);return true;}
    if(command==C::Join || command==C::Increment || command==C::Decrement || (transform(command) && command!=C::AutoIndent && command!=C::Format)){editVimNormal(action);return true;}
    if((command==C::AutoIndent || command==C::Format) && vim_.mode()==Mode::Visual){const auto range=selectionRange();transformVimRange(range.first,range.second,command,vimVisualLine_);return true;}
    if(command==C::RepeatChange){repeatVimChange(action.count,action.explicitCount);return true;}
    if(command==C::RecordMacro){if(!vimExNormal_)toggleVimRecording(action.character);return true;}
    if(command==C::PlayMacro){playVimMacro(action.character,action.count);return true;}
    if(command==C::SetMark){saveVimMark(action.character);return true;}
    if(command==C::JumpMark || command==C::JumpMarkLine){jumpVimMark(action.character,command==C::JumpMarkLine,action.operation,action.around);return true;}
    if(command==C::JumpBack || command==C::JumpForward){jumpVimHistory(command==C::JumpBack?-1:1,action.count);return true;}
    if(command==C::InsertRegister){putVimRegister(action.character,true,1,true);return true;}
    if ((command == C::ReplaceMode || command == C::Insert) && vim_.acceptsText()) {
        vimInsertModeToggled_ = true;
        hideCompletions();
        vim_.setMode(command == C::ReplaceMode ? Mode::Replace : Mode::Insert);
        refreshVimPresentation();
        return true;
    }
    if(command==C::ReplaceMode){switchVimMode(Mode::Replace);vimInsertCount_=action.count;vimInsertStart_=cursorPosition_;return true;}
    if(command==C::ReplaceCharacter){if(vimVisualBlock_)editVimBlock(command,action.count,action.text);else replaceVimText(action.text,true,action.count);return true;}
    if(command==C::VisualBlock){
        if(vimVisualBlock_){switchVimMode(Mode::Normal);return true;}
        if(vim_.mode()!=Mode::Visual)switchVimMode(Mode::Visual);
        vimVisualLine_=false;vimVisualBlock_=true;vimBlockToEnd_=false;
        vimBlockAnchorColumn_=VimBlock::column(buffer_,buffer_.lineIndexAt(anchor_.value_or(cursorPosition_)),anchor_.value_or(cursorPosition_),tabWidth_);
        const auto anchorPosition=anchor_.value_or(cursorPosition_);const auto anchorNext=std::min(lineContentEnd(buffer_.lineIndexAt(anchorPosition)),graphemeBoundaryAt(anchorPosition,true));
        vimBlockAnchorEnd_=vimBlockAnchorColumn_+VimBlock::width(buffer_.slice(anchorPosition,anchorNext-anchorPosition),vimBlockAnchorColumn_,tabWidth_);
        const auto column=VimBlock::column(buffer_,currentLine(),cursorPosition_,tabWidth_);const auto next=std::min(lineContentEnd(currentLine()),graphemeBoundaryAt(cursorPosition_,true));
        vimBlockVirtualColumn_=column+VimBlock::width(buffer_.slice(cursorPosition_,next-cursorPosition_),column,tabWidth_)-1;emit vimModeChanged();viewport()->update();return true;
    }
    if(vimVisualBlock_ && (command==C::Delete || command==C::Change || command==C::Yank || command==C::DeleteChar || command==C::DeleteBefore || command==C::Substitute || command==C::InsertFirst || command==C::InsertLast || command==C::Indent || command==C::Unindent)) {
        editVimBlock(command,action.count);return true;
    }
    if(vimVisualBlock_ && (command==C::DeleteEnd || command==C::ChangeEnd)){vimBlockToEnd_=true;editVimBlock(command==C::DeleteEnd?C::Delete:C::Change,action.count);return true;}
    if(command==C::Normal){
        if(vim_.mode()==Mode::Visual && (vimVisualLine_ || vimVisualBlock_) && event->text()=="v"){vimVisualLine_=vimVisualBlock_=false;emit vimModeChanged();viewport()->update();}
        else switchVimMode(Mode::Normal,true);
        return true;
    }
    if(command==C::Visual || command==C::VisualLine) {
        if(command==C::VisualLine && vim_.mode()==Mode::Visual && vimVisualLine_){switchVimMode(Mode::Normal);return true;}
        if(vim_.mode()!=Mode::Visual)switchVimMode(Mode::Visual);
        vimVisualBlock_=false;vimVisualLine_=command==C::VisualLine;
        if(action.explicitCount && !vimRepeatingDot_){
            VimController::Action motion;motion.command=vimVisualLine_ || vimPreviousVisualRows_>1?C::Down:C::Right;
            motion.count=int(std::min<std::size_t>(10000,action.count*(motion.command==C::Down?vimPreviousVisualRows_:vimPreviousVisualWidth_))-1);
            if(motion.count>0)moveCursor(VimMotions::resolve(buffer_,cursorPosition_,motion,{},vimFinalNewline_).target,true);
        }
        emit vimModeChanged();viewport()->update();return true;
    }
    if(command==C::Undo || command==C::Redo) {
        switchVimMode(Mode::Normal);
        replayVimHistory(command==C::Redo,action.count);
        return true;
    }
    if(command==C::PasteAfter || command==C::PasteBefore){putVimRegister(vimRegister_,command==C::PasteBefore,action.count);return true;}
    if(vim_.mode()==Mode::Visual && (command==C::OpenAbove || command==C::OpenBelow)) {
        if(vimVisualBlock_ && anchor_) {
            const auto anchorRow=buffer_.lineIndexAt(*anchor_),headRow=currentLine();
            const int oldAnchor=vimBlockAnchorColumn_,oldHead=VimBlock::column(buffer_,headRow,cursorPosition_,tabWidth_);
            if(command==C::OpenAbove){cursorPosition_=VimBlock::slice(buffer_,headRow,oldAnchor,oldAnchor,tabWidth_).begin;anchor_=VimBlock::slice(buffer_,anchorRow,oldHead,oldHead,tabWidth_).begin;}
            else std::swap(cursorPosition_,*anchor_);
            vimBlockAnchorColumn_=VimBlock::column(buffer_,buffer_.lineIndexAt(*anchor_),*anchor_,tabWidth_);const auto next=std::min(lineContentEnd(buffer_.lineIndexAt(*anchor_)),graphemeBoundaryAt(*anchor_,true));
            vimBlockAnchorEnd_=vimBlockAnchorColumn_+VimBlock::width(buffer_.slice(*anchor_,next-*anchor_),vimBlockAnchorColumn_,tabWidth_);vimBlockVirtualColumn_=oldAnchor;
            cursors_={cursorPosition_};cursorAnchors_={anchor_};ensureCursorVisible();viewport()->update();return true;
        }
        if(anchor_){std::swap(cursorPosition_,*anchor_);cursors_={cursorPosition_};cursorAnchors_={anchor_};ensureCursorVisible();viewport()->update();}return true;
    }
    if(vim_.mode()==Mode::Visual && (command==C::DeleteEnd || command==C::ChangeEnd || command==C::YankLine || command==C::DeleteBefore || command==C::ChangeLine)) {
        const auto [begin,end]=selectionRange();const auto first=buffer_.lineIndexAt(begin),last=buffer_.lineIndexAt(end>begin?end-1:begin);
        editVimRange(vimLineStart(first),last+1<buffer_.lineCount()?buffer_.lineStart(last+1):buffer_.size(),command==C::YankLine?C::Yank:command==C::ChangeEnd || command==C::ChangeLine?C::Change:C::Delete,true);return true;
    }
    if(command==C::InsertZero || command==C::Insert || command==C::Append || command==C::InsertFirst || command==C::InsertLast || command==C::OpenAbove || command==C::OpenBelow) {
        const auto row=command==C::OpenBelow ? std::size_t(closedFoldEnd(int(currentLine()))) : currentLine();
        if(command==C::Append)cursorPosition_=std::min(lineContentEnd(row),graphemeBoundaryAt(cursorPosition_,true));
        if(command==C::InsertZero)cursorPosition_=vimLineStart(row);
        if(command==C::InsertFirst)cursorPosition_=VimMotions::firstText(buffer_,row);
        if(command==C::InsertLast)cursorPosition_=lineContentEnd(row);
        switchVimMode(Mode::Insert);
        std::string openPrefix;
        if(command==C::OpenAbove || command==C::OpenBelow) {
            refreshEditorConfig();const auto first=VimMotions::firstText(buffer_,row),start=vimLineStart(row);
            const auto indent=buffer_.slice(start,first-start);
            const std::string eol=editorConfig_.eol.value_or(SmartEditing::eol(buffer_));
            openPrefix=eol+indent;
            const bool above=command==C::OpenAbove;
            const auto at=above?start:lineContentEnd(row);
            replaceRange(at,0,above?indent+eol:eol+indent);
            if(!busy_) {cursorPosition_=above?at+indent.size():at+eol.size()+indent.size();cursors_={cursorPosition_};ensureCursorVisible();viewport()->update();}
        }
        vimInsertCount_=action.count;vimInsertStart_=cursorPosition_;vimInsertPrefix_=std::move(openPrefix);
        return true;
    }
    if(vim_.mode()==Mode::Visual && (command==C::Delete || command==C::Change || command==C::Yank || command==C::Indent || command==C::Unindent || command==C::DeleteChar || command==C::Substitute)) {
        const auto [a,b]=selectionRange();const auto operation=command==C::DeleteChar?C::Delete:command==C::Substitute?C::Change:command;
        if(operation==C::Indent || operation==C::Unindent){const auto first=buffer_.lineIndexAt(a),last=buffer_.lineIndexAt(b>a?b-1:a);switchVimMode(Mode::Normal);indentVim(first,last,operation==C::Unindent,action.count);}
        else editVimRange(a,b,operation,vimVisualLine_);
        return true;
    }
    auto resolved=action;
    if(command==C::DeleteChar || command==C::DeleteBefore || command==C::Substitute) {
        resolved.command=command==C::DeleteBefore?C::Left:C::Right;
        resolved.operation=command==C::Substitute?C::Change:C::Delete;
        resolveVimAction(resolved);return true;
    }
    if(command==C::DeleteEnd || command==C::ChangeEnd){resolved.command=C::End;resolved.operation=command==C::DeleteEnd?C::Delete:C::Change;}
    if(command==C::ChangeLine || command==C::YankLine){resolved.command=C::Line;resolved.operation=command==C::ChangeLine?C::Change:C::Yank;}
    if(command==C::RepeatFind || command==C::ReverseFind) {
        if(vimLastFind_==C::Consume)return true;
        resolved.command=vimLastFind_;resolved.character=vimFindCharacter_;
        resolved.repeatFind=true;
        if(command==C::ReverseFind)resolved.command=vimLastFind_==C::Find?C::FindBack:vimLastFind_==C::FindBack?C::Find:vimLastFind_==C::Till?C::TillBack:C::Till;
    } else if(command==C::Find || command==C::FindBack || command==C::Till || command==C::TillBack){vimLastFind_=command;vimFindCharacter_=action.character;}
    resolveVimAction(resolved);return true;
}
bool EditorWidget::repeatVimInsert() {
    const int count=std::exchange(vimInsertCount_,1)-1;
    if(count<=0 || cursorPosition_<vimInsertStart_)return false;
    if (vimInsertModeToggled_ && vimCountedInsertKeys_.size() > 1 && vimCountedInsertKeys_.size() < 100000) {
        auto keys = std::exchange(vimCountedInsertKeys_, {});
        keys.removeLast(); // The current Escape closes the repeated insertion as well.
        replayVim(std::move(keys), count, true);
        return true;
    }
    const auto length=cursorPosition_-vimInsertStart_;const auto at=cursorPosition_;
    if(vim_.mode()==VimController::Mode::Replace){
        if(length*std::size_t(count)>=4096){auto source=std::make_shared<ChunkedFile>();for(std::size_t p=0;p<length;p+=64*1024)source->append(buffer_.slice(vimInsertStart_+p,std::min<std::size_t>(64*1024,length-p)));replaceVimPayload(source,count,true);return true;}
        const auto bytes=buffer_.slice(vimInsertStart_,length);for(int n=0;n<count;++n)replaceVimText(QString::fromUtf8(bytes.data(),qsizetype(bytes.size())));return false;
    }
    if(!length && vimInsertPrefix_.empty())return false;
    if((length+vimInsertPrefix_.size())*std::size_t(count)>=1024*1024) {
        const auto from=vimInsertStart_;const auto prefix=vimInsertPrefix_;
        runBulkOperation(QStringLiteral("Vim…"),[at,from,length,count,prefix](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
            auto payload=std::make_shared<ChunkedFile>();std::size_t written=0;
            const auto append=[&](std::string_view value){payload->append(value);buffer.insert(at+written,value);written+=value.size();step(50);};
            for(int n=0;n<count;++n){append(prefix);for(std::size_t p=0;p<length;p+=64*1024){const auto bytes=buffer.slice(from+p,std::min<std::size_t>(64*1024,length-p));append(bytes);}}
            history.recordOnDisk(at,payload,0,written,at,at+written,0);return HistoryResult{at+written,{at+written}};
        },true,{},[this](bool success){switchVimMode(VimController::Mode::Normal,success);});return true;
    }
    const auto body=vimInsertPrefix_+buffer_.slice(vimInsertStart_,length);std::string text;
    for(int n=0;n<count;++n)text+=body;
    insertText(text,EditKind::Standalone);return false;
}
void EditorWidget::replayVimHistory(bool redoEdit,int count) {
    const auto previous=std::exchange(vimApplyingHistory_,true);const auto guard=qScopeGuard([this,previous]{vimApplyingHistory_=previous;});
    if(count==1){if(redoEdit)redo();else undo();return;}
    const auto original=cursorPosition_;
    runBulkOperation(QStringLiteral("Vim…"),[count,redoEdit,original](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
        HistoryResult cursor{original,{original}};
        for(int n=0;n<count;++n){step(n*100/count);const auto result=redoEdit?history.redo(buffer,{},[&]{step(n*100/count);}):history.undo(buffer,{},[&]{step(n*100/count);});if(!result)break;cursor=*result;}
        return cursor;
    });
}
void EditorWidget::resolveVimAction(VimController::Action action) {
    using C=VimController::Command;
    action.tabWidth=tabWidth_;
    const int line=int(currentLine());
    if (action.operation != C::Consume && screenLayoutActive() && !vimFolds_.empty() &&
        (action.command == C::Line || action.command == C::Up || action.command == C::Down)) {
        const int last=int(VimMotions::lastLine(buffer_,vimFinalNewline_));
        const int direction=action.command==C::Up ? -1 : 1;
        const int count=action.count-(action.command==C::Line ? 1 : 0);
        int target=line;
        for (int step=0;step<count;++step) {
            const int candidate=direction>0 ? closedFoldEnd(target)+1 : target-1;
            if (candidate<0 || candidate>last) break;
            target=documentLineForVisualRow(visualRowForLine(candidate));
        }
        const int first=std::min(line,target), end=closedFoldEnd(std::max(line,target));
        VimMotions::Range range;
        range.target=VimMotions::firstText(buffer_,target);
        range.begin=vimLineStart(first);
        range.end=end+1<int(buffer_.lineCount()) ? buffer_.lineStart(end+1) : buffer_.size();
        range.valid=range.linewise=true;
        applyVimAction(action,range);
        return;
    }
    if(vimVisualBlock_ && action.operation==C::Consume && (action.command==C::Left || action.command==C::Right || action.command==C::Up || action.command==C::Down)) {
        auto row=currentLine();if(action.command==C::Up)row=row>std::size_t(action.count)?row-action.count:0;
        if(action.command==C::Down)row=std::min(VimMotions::lastLine(buffer_,vimFinalNewline_),row+action.count);
        if(action.command==C::Left || action.command==C::Right){
            const auto motion=VimMotions::resolve(buffer_,cursorPosition_,action,{},vimFinalNewline_);moveCursor(motion.target,true);
            const auto column=VimBlock::column(buffer_,currentLine(),cursorPosition_,tabWidth_);const auto next=std::min(lineContentEnd(currentLine()),graphemeBoundaryAt(cursorPosition_,true));
            vimBlockVirtualColumn_=column+VimBlock::width(buffer_.slice(cursorPosition_,next-cursorPosition_),column,tabWidth_)-1;vimBlockToEnd_=false;return;
        }
        const auto range=VimBlock::slice(buffer_,row,vimBlockVirtualColumn_,vimBlockVirtualColumn_,tabWidth_,{},[](std::string_view){});
        moveCursor(range.begin,true);return;
    }
    if(action.operation==C::Consume && (action.command==C::Up || action.command==C::Down)) {
        if(vimPreferredX_<0)vimPreferredX_=cursorPixelColumn();
        const auto row=currentLine(),count=std::size_t(action.count);
        const auto last=VimMotions::lastLine(buffer_,vimFinalNewline_);
        auto to=action.command==C::Up?(row>count?row-count:0):std::min(last,row+count);
        if (screenLayoutActive() && !vimFolds_.empty()) {
            to=row;
            for(std::size_t n=0;n<count;++n) {
                if(action.command==C::Up) {
                    if(!to)break;
                    to=std::size_t(documentLineForVisualRow(visualRowForLine(int(to-1))));
                } else {
                    const auto next=std::size_t(closedFoldEnd(int(to)))+1;
                    if(next>last)break;
                    to=next;
                }
            }
        }
        if(to==row && !vimReplay_.empty()){vimReplayCancel_=true;return;}
        const auto target=action.text=="\r" || action.text=="\n"?VimMotions::firstText(buffer_,to):buffer_.lineStart(to)+byteColumnAtX(gutterWidth()+12+vimPreferredX_-horizontalScrollBar()->value(),int(to));
        moveCursor(target,vim_.mode()==VimController::Mode::Visual);return;
    }
    vimPreferredX_=-1;
    const auto selection=vim_.mode()==VimController::Mode::Visual && anchor_ && *anchor_!=cursorPosition_?std::optional{selectionRange()}:std::nullopt;
    if(buffer_.size()>=1024*1024 && action.command!=C::Left && action.command!=C::Right && action.command!=C::Start && action.command!=C::End && action.command!=C::Line) {
        auto result=std::make_shared<VimMotions::Range>();const auto cursor=cursorPosition_;const auto finalNewline=vimFinalNewline_;
        runBulkOperation(QStringLiteral("Vim…"),[result,action,cursor,selection,finalNewline](PieceTreeBuffer& buffer,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
            *result=VimMotions::resolveSelection(buffer,cursor,action,selection,[&]{step(50);},finalNewline);return HistoryResult{cursor,{cursor}};
        },false,{},[this,result,action](bool success){if(success)applyVimAction(action,*result);});
    } else applyVimAction(action,VimMotions::resolveSelection(buffer_,cursorPosition_,action,selection,{},vimFinalNewline_));
}
void EditorWidget::applyVimAction(const VimController::Action& action,const VimMotions::Range& range) {
    using C=VimController::Command;using Mode=VimController::Mode;
    if(!range.valid || !vim_.enabled()){if(!vimReplay_.empty())vimReplayCancel_=true;return;}
    if(action.operation==C::Consume && !range.object && (action.command==C::FirstLine || action.command==C::LastLine || action.command==C::ParagraphNext || action.command==C::ParagraphBack || action.command==C::SentenceNext || action.command==C::SentenceBack || action.command==C::SectionNext || action.command==C::SectionBack || action.command==C::Match))pushVimJump();
    if(action.operation==C::Fold){
        createVimFold(int(buffer_.lineIndexAt(range.begin)),
                      int(buffer_.lineIndexAt(range.end>range.begin?range.end-1:range.end)));
        return;
    }
    if(action.operation==C::Indent || action.operation==C::Unindent){indentVim(buffer_.lineIndexAt(range.begin),buffer_.lineIndexAt(range.end>range.begin?range.end-1:range.begin),action.operation==C::Unindent);return;}
    if(action.operation!=C::Consume){editVimRange(range.begin,range.end,action.operation,range.linewise);return;}
    if(range.object) {
        if(vim_.mode()!=Mode::Visual)switchVimMode(Mode::Visual);
        const bool reversed=anchor_ && cursorPosition_<*anchor_;
        const auto last=range.end>range.begin?graphemeBoundaryAt(range.end,false):range.begin;
        vimVisualBlock_=false;vimVisualLine_=range.linewise;anchor_=reversed?last:range.begin;cursorPosition_=reversed?range.begin:last;
        cursors_={cursorPosition_};cursorAnchors_={anchor_};emit vimModeChanged();viewport()->update();ensureCursorVisible();return;
    }
    moveCursor(range.target,vim_.mode()==Mode::Visual);
    if(vimVisualBlock_){vimBlockVirtualColumn_=VimBlock::column(buffer_,currentLine(),cursorPosition_,tabWidth_);vimBlockToEnd_=action.command==C::End;viewport()->update();}
}
std::size_t EditorWidget::vimSelectionEnd(std::size_t position) const {
    return graphemeBoundaryAt(position,true);
}
int EditorWidget::vimCaretWidth() const {
    if(vim_.acceptsText() && vim_.mode()!=VimController::Mode::Replace)return 2;
    const auto row=currentLine(),start=buffer_.lineStart(row),end=lineContentEnd(row);
    const int fallback=std::max(1,fontMetrics().horizontalAdvance('M'));
    if(cursorPosition_>=end || end-start>64u*1024u)return fallback;
    const auto last=std::min(end,graphemeBoundaryAt(cursorPosition_,true));
    const auto bytes=buffer_.slice(start,last-start);
    return std::max(1,textWidth(QString::fromUtf8(bytes.data(),qsizetype(bytes.size())))-cursorPixelColumn());
}
