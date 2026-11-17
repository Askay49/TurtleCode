#include "EditorWidget.hpp"
#include "VimDigraphs.hpp"
#include <QKeyEvent>
#include <QScrollBar>
#include <QScopeGuard>

bool EditorWidget::handleVimInsertKey(QKeyEvent* event) {
    if(!vim_.enabled() || !vim_.acceptsText())return false;
    const int key=event->key();const bool ctrl=event->modifiers()==Qt::ControlModifier;
    if(key==Qt::Key_Control || key==Qt::Key_Shift)return false;
    const auto insert=[&](const QString& text){if(vim_.mode()==VimController::Mode::Replace)replaceVimText(text);else replaceSelection(text.toUtf8().toStdString());};
    if(vimInsertCtrlG_) {
        vimInsertCtrlG_=false;
        if(event->text()=="U"){vimInsertJoinMovement_=true;return true;}
        if(event->text()=="u"){history_.endGroup();finishVimChange();vimCommandKeys_.clear();history_.beginGroup(true);return true;}
        return true;
    }
    if((key==Qt::Key_Return || key==Qt::Key_Enter) && !vimInsertSavedIndent_.isEmpty()) {
        const auto indent=std::exchange(vimInsertSavedIndent_,{});insert(QString::fromStdString(SmartEditing::eol(buffer_))+indent);return true;
    }
    if(!vimLiteral_.isEmpty()) {
        const QString character=event->text();
        if(vimLiteral_=="pending") {
            vimLiteral_.clear();
            if(character.size()==1 && QString("xXoOuU0123456789").contains(character[0])) {
                const auto ch=character[0];vimLiteralBase_=ch=='x'||ch=='X'||ch=='u'||ch=='U'?16:ch=='o'||ch=='O'?8:10;
                vimLiteralLimit_=ch=='u'?4:ch=='U'?8:vimLiteralBase_==16?2:3;
                vimLiteral_=QString("#")+(ch.isDigit()?character:QString{});return true;
            }
            char32_t code=key==Qt::Key_Escape?27:key==Qt::Key_Return || key==Qt::Key_Enter?13:key==Qt::Key_Tab?9:ctrl && key>=Qt::Key_A && key<=Qt::Key_Z?key-Qt::Key_A+1:0;
            insert(code?QString::fromUcs4(&code,1):character);return true;
        }
        bool valid=false;if(character.size()==1)character.toUInt(&valid,vimLiteralBase_);
        if(valid){vimLiteral_+=character;if(vimLiteral_.size()-1<vimLiteralLimit_)return true;}
        bool ok=false;const char32_t code=vimLiteral_.mid(1).toUInt(&ok,vimLiteralBase_);vimLiteral_.clear();
        if(ok && code<=0x10ffff && !(code>=0xd800 && code<=0xdfff))insert(QString::fromUcs4(&code,1));
        if(valid)return true;
        return handleVimInsertKey(event);
    }
    if(!vimDigraph_.isEmpty()) {
        if(key==Qt::Key_Escape){vimDigraph_.clear();return true;}
        if(event->text().isEmpty())return true;
        vimDigraph_+=event->text();if(vimDigraph_.size()<3)return true;
        const auto a=vimDigraph_[1].unicode(),b=vimDigraph_[2].unicode();
        const auto code=vimDigraphs().value((unsigned(a)<<16)|b,vimDigraphs().value((unsigned(b)<<16)|a,b));
        vimDigraph_.clear();insert(QString::fromUcs4(&code,1));return true;
    }
    if(!vimInsertCandidates_.isEmpty() && vimCompletionTextRevision_==lspRevision_) {
        if(ctrl && key==Qt::Key_Y){
            if(!vimChangeKeys_.isEmpty())vimChangeKeys_.removeLast();auto& state=VimState::Session::instance();
            if(!state.recording.isNull() && !state.recordingInputs.isEmpty())state.recordingInputs.removeLast();
            vimInsertCandidates_.clear();return true;
        }
        if(ctrl && key==Qt::Key_E){vimInsertCandidate_=vimInsertCandidates_.size()-2;completeVimInsert(1);vimInsertCandidates_.clear();return true;}
        if(ctrl && key==vimInsertCompletionSource_.unicode()){completeVimInsert(key==Qt::Key_L?-1:1);return true;}
    }
    if(vimInsertCtrlX_) {
        vimInsertCtrlX_=false;
        if(ctrl && (key==Qt::Key_O || key==Qt::Key_U)){
            const auto removeTrigger=[](QVector<VimState::Input>& keys){if(!keys.isEmpty())keys.removeLast();if(!keys.isEmpty() && keys.back().key==Qt::Key_X && keys.back().modifiers==int(Qt::ControlModifier))keys.removeLast();};
            removeTrigger(vimChangeKeys_);auto& state=VimState::Session::instance();if(!state.recording.isNull())removeTrigger(state.recordingInputs);
            emit vimCompletionRequested();return true;
        }
        if(ctrl && (key==Qt::Key_L || key==Qt::Key_F || key==Qt::Key_N || key==Qt::Key_P || key==Qt::Key_K || key==Qt::Key_T || key==Qt::Key_I || key==Qt::Key_D || key==Qt::Key_V || key==Qt::Key_BracketRight)) {
            completeVimInsert(key==Qt::Key_P || key==Qt::Key_L?-1:1,QChar(key));return true;
        }
        if(ctrl && (key==Qt::Key_E || key==Qt::Key_Y)){verticalScrollBar()->setValue(verticalScrollBar()->value()+(key==Qt::Key_E?1:-1));return true;}
        if(ctrl && key==Qt::Key_Z)return true;
        if(key==Qt::Key_Escape)return false;
    }
    if(!ctrl){if(key!=Qt::Key_Control && key!=Qt::Key_Shift)vimInsertCandidates_.clear();return false;}
    switch(key) {
    case Qt::Key_J:case Qt::Key_M:smartEnter();return true;
    case Qt::Key_I:indentSelection(false);return true;
    case Qt::Key_At:case Qt::Key_Space:
        putVimRegister('.',false,1,true);
        if(busy_)vimInsertExitAfterPut_=true;else switchVimMode(VimController::Mode::Normal,true);
        return true;
    case Qt::Key_G:vimInsertCtrlG_=true;return true;
    case Qt::Key_V:case Qt::Key_Q:hideCompletions();vimLiteral_="pending";return true;
    case Qt::Key_K:hideCompletions();vimDigraph_="#";return true;
    case Qt::Key_X:vimInsertCtrlX_=true;vimInsertCandidates_.clear();hideCompletions();return true;
    case Qt::Key_N:case Qt::Key_P:completeVimInsert(key==Qt::Key_N?1:-1);return true;
    case Qt::Key_Y:case Qt::Key_E:{
        const auto row=currentLine();if((key==Qt::Key_Y && !row) || (key==Qt::Key_E && row+1>=buffer_.lineCount()))return true;
        const auto other=key==Qt::Key_Y?row-1:row+1;
        const int column=VimBlock::column(buffer_,row,cursorPosition_,tabWidth_);
        const auto range=VimBlock::slice(buffer_,other,column,column+1,tabWidth_);
        if(range.begin<range.end)insert(QString::fromUtf8(buffer_.slice(range.begin,range.end-range.begin).c_str()));
        return true;
    }
    case Qt::Key_H:if(vim_.mode()==VimController::Mode::Replace)backspaceVimReplace();else deleteBackward(false);return true;
    case Qt::Key_W:{
        if(!cursorPosition_)return true;
        const auto end=cursorPosition_,row=currentLine(),lineStart=vimLineStart(row);
        const auto start=end>vimInsertStart_?std::max(lineStart,vimInsertStart_):lineStart;
        if(end==lineStart){deleteBackward(false);return true;}
        VimController::Action motion;motion.command=VimController::Command::WordBack;
        const auto apply=[this,start,end](const VimMotions::Range& range){
            const auto handling=std::exchange(vimHandlingKey_,true),capture=std::exchange(vimCaptureInsertEdit_,true);
            const auto guard=qScopeGuard([&]{vimHandlingKey_=handling;vimCaptureInsertEdit_=capture;});
            const auto begin=std::max(start,range.target);
            if(vim_.mode()==VimController::Mode::Replace){while(cursorPosition_>begin)backspaceVimReplace();}
            else replaceRange(begin,end-begin,{});
        };
        if(buffer_.size()>=1024*1024){
            auto range=std::make_shared<VimMotions::Range>();
            runBulkOperation(QStringLiteral("Vim…"),[motion,end,range](PieceTreeBuffer& buffer,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep& step){*range=VimMotions::resolve(buffer,end,motion,[&]{step(50);});return HistoryResult{end,{end}};},false,{},[range,apply](bool ok){if(ok)apply(*range);});
        }else apply(VimMotions::resolve(buffer_,end,motion));
        return true;
    }
    case Qt::Key_U:{const auto begin=(cursorPosition_<=vimInsertStart_?vimLineStart(currentLine()):std::max(vimLineStart(currentLine()),vimInsertStart_));if(vim_.mode()==VimController::Mode::Replace){while(cursorPosition_>begin)backspaceVimReplace();}else replaceRange(begin,cursorPosition_-begin,{});return true;}
    case Qt::Key_T:case Qt::Key_D:{
        const auto row=currentLine();auto at=cursorPosition_;
        const auto start=vimLineStart(row);auto end=SmartEditing::indentationEnd(buffer_,row);
        refreshEditorConfig();const auto width=editorConfig_.indentation();
        bool all=false;
        if(key==Qt::Key_D && at>start && (buffer_.byteAt(at-1)=='0' || buffer_.byteAt(at-1)=='^') && at-1==end) {
            if(buffer_.byteAt(at-1)=='^')vimInsertSavedIndent_=QString::fromUtf8(buffer_.slice(start,end-start).c_str());
            replaceRange(at-1,1,{});--at;all=true;
        }
        const int columns=VimBlock::column(buffer_,row,end,tabWidth_);
        const int shifted=all?0:key==Qt::Key_T?(columns/width+1)*width:std::max(0,(columns-1)/width*width);
        const auto indent=editorConfig_.tabs.value_or(false)?std::string(shifted/tabWidth_,'\t')+std::string(shifted%tabWidth_,' '):std::string(shifted,' ');
        replaceRange(start,end-start,indent);cursorPosition_=at>=end?at+indent.size()-(end-start):start+std::min<std::size_t>(at-start,indent.size());
        cursors_={cursorPosition_};clearAllSelections();ensureCursorVisible();viewport()->update();return true;
    }
    case Qt::Key_O:
        hideCompletions();
        if(!vimChangeKeys_.isEmpty())vimChangeKeys_.removeLast();
        finishVimChange();vimCommandKeys_.clear();history_.endGroup();vimInsertCount_=1;
        vimInsertNormalReplayDepth_=vimReplay_.size();vimInsertReturn_=vim_.mode();vimInsertNormal_=vimInsertNormalArmed_=true;
        vim_.setMode(VimController::Mode::Normal);normalizeVimCursor();clearAllSelections();refreshVimPresentation();return true;
    case Qt::Key_A:putVimRegister('.',false,1,true);return true;
    default:return false;
    }
}
