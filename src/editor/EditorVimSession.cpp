#include "EditorWidget.hpp"
#include "app/I18n.hpp"
#include "StoredClipboard.hpp"
#include "UnicodeNavigation.hpp"
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QTimer>
#include <QFileInfo>
#include <QTabWidget>
#include <algorithm>
#include <limits>
#include <QTextBoundaryFinder>
#include <QLineEdit>

namespace {
VimState::Input keyInput(const QKeyEvent& key){VimState::Input input;input.key=key.key();input.modifiers=int(key.modifiers());input.text=key.text();return input;}
bool modifier(int key){return key==Qt::Key_Control || key==Qt::Key_Shift || key==Qt::Key_Alt || key==Qt::Key_Meta;}
QVector<VimState::Input> textKeys(const QString& text){return VimState::keys(text);}

}
bool EditorWidget::beginVimKey(QKeyEvent* event) {
    if(!vim_.enabled())return false;
    auto& shared=VimState::Session::instance();
    if(!vimReplay_.empty() && !vimSendingReplay_){cancelVimReplay();return true;}
    if(modifier(event->key()))return false;
    if(!vimExActive_ && !vimSubConfirm_ && !vimExNormal_ && vim_.mode()==VimController::Mode::Normal && event->text()=="q" && !shared.recording.isNull() && !vim_.hasPending()) {
        toggleVimRecording();vimCommandKeys_.clear();event->accept();return true;
    }
    vimHandlingKey_=true;
    const auto input=keyInput(*event);
    if(!vimSendingReplay_ && !shared.recording.isNull()) {
        if(shared.recordingInputs.size()<100000 && shared.recordingBytes+std::size_t(input.text.toUtf8().size())<=1024*1024){shared.recordingInputs.append(input);shared.recordingBytes+=std::size_t(input.text.toUtf8().size());}
        else {toggleVimRecording();emit highlightingStatusChanged(I18n::text("Vim: macro recording limit reached"));}
    }
    if(vimExActive_ || vimSubConfirm_){if(vimExActive_ && vimCommandPrompt_!=':' && vimSearchAction_.operation!=VimController::Command::Consume)vimCommandKeys_.append(input);return false;}
    const bool movement=event->key()==Qt::Key_Left || event->key()==Qt::Key_Right || event->key()==Qt::Key_Up || event->key()==Qt::Key_Down || event->key()==Qt::Key_Home || event->key()==Qt::Key_End;
    if(vim_.acceptsText() && movement){finishVimChange();vimCommandKeys_.clear();}
    else if(vim_.acceptsText() && !vimChangeActive_) {
        vimCommandKeys_=textKeys(vim_.mode()==VimController::Mode::Replace?"R":"i");beginVimChange();
    }
    if(vimChangeActive_){if(!vimChangeTooLarge_)vimChangeKeys_.append(input);if(vimChangeKeys_.size()>100000){vimChangeKeys_.clear();vimChangeTooLarge_=true;emit highlightingStatusChanged(I18n::text("Vim: change is too large to repeat"));}}else vimCommandKeys_.append(input);
    vimCaptureInsertEdit_=vim_.acceptsText();
    return false;
}
void EditorWidget::endVimKey() {
    vimHandlingKey_=false;vimCaptureInsertEdit_=false;
    if(!vim_.enabled())return;
    if(vimChangeActive_ && !busy_ && !vim_.acceptsText())finishVimChange();
    if(!vimChangeActive_ && !vim_.hasPending() && !vimExActive_ && vim_.mode()!=VimController::Mode::Visual)vimCommandKeys_.clear();
}
void EditorWidget::beginVimChange(){
    if(vimChangeActive_ || vimRepeatingDot_)return;
    vimChangeBefore_=history_.stateId();vimChangeKeys_=std::exchange(vimCommandKeys_,{});vimChangeActive_=true;vimChangeTooLarge_=false;vimChangeInserted_=vim_.acceptsText();vimEditPayload_.reset();
    if(vim_.acceptsText()){vimInsertedText_.clear();vimInsertStart_=cursorPosition_;}
}
void EditorWidget::finishVimChange(){
    if(!vimChangeActive_)return;
    if(history_.stateId()!=vimChangeBefore_ && !vimChangeKeys_.isEmpty() && !vimRepeatingDot_ && !vimChangeTooLarge_) {
        auto& change=VimState::Session::instance().lastChange;change.inputs=vimChangeKeys_;
        if(vim_.acceptsText()){VimState::Input escape;escape.key=Qt::Key_Escape;change.inputs.append(escape);}
        if(vimChangeInserted_ && !vimInsertedText_.empty()){VimState::Register last;last.text=std::make_shared<ChunkedFile>();last.text->append(vimInsertedText_);last.text->dropCache();VimState::Session::instance().set('.',last);}
    }
    vimChangeKeys_.clear();vimChangeActive_=false;vimEditPayload_.reset();commitVimChangeLocation();
}
void EditorWidget::captureVimEdit(std::size_t at,std::string_view removed,std::string_view inserted) {
    if(!removed.empty() || !inserted.empty())noteVimEdit(at,inserted.size());
    if(vim_.enabled() && vimCaptureInsertEdit_ && !vimHandlingKey_ && !vimSendingReplay_ && !vimChangeActive_ && at<=cursorPosition_ && at+removed.size()>=cursorPosition_){vimCommandKeys_=textKeys(vim_.mode()==VimController::Mode::Replace?"R":"i");beginVimChange();}
    if(vim_.enabled() && vimCaptureInsertEdit_ && vim_.acceptsText() && at<=cursorPosition_ && at+removed.size()>=cursorPosition_ && (!vimBlockInsert_ || at==cursorPosition_ || at+removed.size()==cursorPosition_)) {
        if(at<vimInsertStart_ || at-vimInsertStart_>vimInsertedText_.size()){vimInsertStart_=at;vimInsertedText_.clear();}
        const auto local=at-vimInsertStart_;vimInsertedText_.replace(local,std::min(removed.size(),vimInsertedText_.size()-local),inserted);vimChangeInserted_=true;
    }
    if(!vim_.enabled() || !vimCaptureInsertEdit_ || vimHandlingKey_ || vimSendingReplay_)return;
    if(at>cursorPosition_ || at+removed.size()<cursorPosition_)return;
    if(!vimChangeActive_){vimCommandKeys_=textKeys(vim_.mode()==VimController::Mode::Replace?"R":"i");beginVimChange();}
    VimState::Input input;input.kind=VimState::Input::Edit;
    const auto count=[](std::string_view bytes){const auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,text);int n=0;while(finder.toNextBoundary()>=0)++n;return n;};
    if(at<cursorPosition_)input.back=count(removed.substr(0,std::min(removed.size(),cursorPosition_-at)));
    if(removed.size()>cursorPosition_-std::min(cursorPosition_,at))input.forward=count(removed.substr(cursorPosition_-std::min(cursorPosition_,at)));
    if(inserted.size()<64*1024)input.text=QString::fromUtf8(inserted.data(),qsizetype(inserted.size()));
    else {if(!vimEditPayload_)vimEditPayload_=std::make_shared<ChunkedFile>();input.payload=vimEditPayload_;input.offset=input.payload->append(inserted);input.length=inserted.size();input.payload->dropCache();}
    if(!vimChangeTooLarge_)vimChangeKeys_.append(input);
    if(vimChangeKeys_.size()>100000){vimChangeKeys_.clear();vimChangeTooLarge_=true;emit highlightingStatusChanged(I18n::text("Vim: change is too large to repeat"));}auto& state=VimState::Session::instance();
    if(!state.recording.isNull()){const auto bytes=inserted.size()+std::size_t(input.back)+std::size_t(input.forward);if(state.recordingBytes+bytes<=1024*1024){state.recordingInputs.append(input);state.recordingBytes+=bytes;}else {toggleVimRecording();emit highlightingStatusChanged(I18n::text("Vim: macro recording limit reached"));}}
}
void EditorWidget::storeVimRegister(QChar selected,const VimState::Register& value,bool yank,bool smallDelete,bool forceNumbered,const VimState::Register* appended) {
    if(!value.text)return;
    value.text->dropCache();VimState::Session::instance().write(selected,value,yank,smallDelete,forceNumbered,appended);
}
void EditorWidget::putVimRegister(QChar name,bool before,int count,bool insert) {
    if(name=='%' || name=='#')VimState::Session::instance().set(name,VimState::text(name=='%'?filePath_:property("vimAlternatePath").toString()));
    const auto value=VimState::Session::instance().read(name);if(value.empty())return;
    if(insert) {
        refreshEditorConfig();const auto eol=editorConfig_.eol.value_or(SmartEditing::eol(buffer_));
        if(vim_.mode()==VimController::Mode::Replace){if(value.text->size()>=4096){replaceVimPayload(value.text);return;}const auto bytes=value.text->read(0,value.text->size());replaceVimText(QString::fromUtf8(bytes.data(),qsizetype(bytes.size())));return;}
        if(value.text->size()<1024*1024){const auto input=VimState::convertNewlines(value.text,eol);insertText(input->read(0,input->size()),EditKind::Standalone);return;}
        const auto at=cursorPosition_;auto source=value.text;
        runBulkOperation(QStringLiteral("Vim…"),[at,source,eol](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
            const auto input=VimState::convertNewlines(source,eol,[&]{step(10);});
            for(std::size_t p=0;p<input->size();p+=64*1024){const auto bytes=input->read(p,std::min<std::size_t>(64*1024,input->size()-p));buffer.insert(at+p,bytes);step(50);}
            history.recordOnDisk(at,input,0,input->size(),at,at+input->size(),0);return HistoryResult{at+input->size(),{at+input->size()}};
        });return;
    }
    vimRegister_=name;
    if(vimVisualBlock_ || value.kind==VimState::Register::Block){putVimBlock(value,before,count);return;}
    if(vim_.mode()==VimController::Mode::Visual) {
        const auto [a,b]=selectionRange();auto replaced=std::make_shared<ChunkedFile>();
        for(auto p=a;p<b;p+=64*1024)replaced->append(buffer_.slice(p,std::min<std::size_t>(64*1024,b-p)));
        VimState::Register deleted;deleted.text=replaced;deleted.kind=vimVisualLine_?VimState::Register::Line:VimState::Register::Character;
        // Snapshot the source because visual put writes the replaced text to ".
        vimPutValue_=value;
        if(!before)storeVimRegister({},deleted,false,!vimVisualLine_ && buffer_.lineIndexAt(a)==buffer_.lineIndexAt(b));
    }
    pasteVim(before,count);
}
void EditorWidget::replaceVimText(const QString& value,bool single,int count) {
    if(value.isEmpty())return;
    refreshEditorConfig();
    const auto bytes=value=="\r" || value=="\n"?editorConfig_.eol.value_or(SmartEditing::eol(buffer_)):value=="\t" && !editorConfig_.tabs.value_or(false)?std::string(editorConfig_.indentation(),' '):value.toUtf8().toStdString();
    if(single && vim_.mode()==VimController::Mode::Visual) {
        const auto [a,b]=selectionRange();
        if(b-a>=1024*1024 || buffer_.lineIndexAt(b)-buffer_.lineIndexAt(a)>4096) {
            const auto original=cursorPosition_;switchVimMode(VimController::Mode::Normal);
            runBulkOperation(QStringLiteral("Vim…"),[a,b,bytes,original](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
                auto payload=std::make_shared<ChunkedFile>();for(auto p=a;p<b;p+=64*1024){payload->append(buffer.slice(p,std::min<std::size_t>(64*1024,b-p)));step(20);}
                std::string written;std::size_t length=0;auto flush=[&]{length+=written.size();payload->append(written);written.clear();};
                const auto first=buffer.lineIndexAt(a),last=buffer.lineIndexAt(b);
                for(auto row=first;row<=last;++row){VimBlock::forEachCell(buffer,row,4,[&](std::size_t p,std::size_t,int,int size,std::string_view){if(p>=b)return false;if(size && p>=a){written+=bytes;if(written.size()>=64*1024)flush();}return true;},[&]{step(40);});
                    const auto end=VimMotions::lineEnd(buffer,row),next=row+1<buffer.lineCount()?buffer.lineStart(row+1):buffer.size();if(end>=a && end<b)written+=buffer.slice(end,std::min(b,next)-end);if(written.size()>=64*1024)flush();}
                flush();buffer.erase(a,b-a);for(std::size_t p=0;p<length;p+=64*1024){buffer.insert(a+p,payload->read(b-a+p,std::min<std::size_t>(64*1024,length-p)));step(80);}
                history.recordOnDisk(a,payload,b-a,length,original,a,0);return HistoryResult{a,{a}};
            });return;
        }
        std::string replacement;
        for(auto p=a;p<b;){const auto row=buffer_.lineIndexAt(p);if(p>=lineContentEnd(row)){const auto next=std::min(b,row+1<buffer_.lineCount()?buffer_.lineStart(row+1):buffer_.size());replacement+=buffer_.slice(p,next-p);p=next;}
            else {replacement+=bytes;p=graphemeBoundaryAt(p,true);}}
        switchVimMode(VimController::Mode::Normal);replaceRange(a,b-a,replacement);moveCursor(a,false);return;
    }
    if(single) {
        VimController::Action motion;motion.command=VimController::Command::Right;motion.count=count;
        const auto end=VimMotions::resolve(buffer_,cursorPosition_,motion,{},vimFinalNewline_).target;
        PieceTreeBuffer selected(buffer_.slice(cursorPosition_,end-cursorPosition_));int available=0;VimBlock::forEachCell(selected,0,tabWidth_,[&](std::size_t,std::size_t,int,int size,std::string_view){if(size)++available;return true;});if(available<count)return;
        const auto begin=cursorPosition_;std::string replacement;for(int n=0;n<(value=="\r" || value=="\n"?1:count);++n)replacement+=bytes;
        replaceRange(begin,end-begin,replacement);if(!busy_)moveCursor(value=="\r" || value=="\n"?VimMotions::firstText(buffer_,buffer_.lineIndexAt(begin+replacement.size())):graphemeBoundaryAt(begin+replacement.size(),false),false);return;
    }
    history_.beginGroup(true);
    const auto utf8=value.toUtf8().toStdString();
    if(utf8.size()>=4096){auto source=std::make_shared<ChunkedFile>();source->append(utf8);replaceVimPayload(source);return;}
    for(std::size_t p=0;p<utf8.size();) {
        const auto next=EditorUnicode::nextGrapheme(utf8,p);auto inserted=utf8.substr(p,next-p);p=next;
        if(inserted=="\r" || inserted=="\n")inserted=editorConfig_.eol.value_or(SmartEditing::eol(buffer_));
        const auto at=cursorPosition_,end=inserted=="\n" || inserted=="\r\n"?at:std::min(lineContentEnd(currentLine()),graphemeBoundaryAt(at,true));
        const auto removed=buffer_.slice(at,end-at);replaceRange(at,end-at,inserted);vimReplaced_.push_back({at,removed,inserted});
        if(busy_)break;
    }
}
void EditorWidget::replaceVimPayload(std::shared_ptr<ChunkedFile> source,int count,bool leaveReplace) {
    const auto original=cursorPosition_;vimReplaced_.clear();history_.beginGroup(true);refreshEditorConfig();const auto eol=editorConfig_.eol.value_or(SmartEditing::eol(buffer_));
    runBulkOperation(QStringLiteral("Vim…"),[source,count,original,eol](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        PieceTreeBuffer input;for(std::size_t p=0;p<source->size();p+=64*1024){input.insert(input.size(),source->read(p,std::min<std::size_t>(64*1024,source->size()-p)));step(10);}
        auto normalized=std::make_shared<ChunkedFile>();std::size_t characters=0;
        for(std::size_t row=0;row<input.lineCount();++row){VimBlock::forEachCell(input,row,4,[&](std::size_t,std::size_t,int,int size,std::string_view){if(size)++characters;return true;},[&]{step(20);});
            const auto first=input.lineStart(row),last=VimMotions::lineEnd(input,row);for(auto p=first;p<last;p+=64*1024){normalized->append(input.slice(p,std::min<std::size_t>(64*1024,last-p)));step(30);}if(row+1<input.lineCount())normalized->append(eol);
        }
        VimController::Action motion;motion.command=VimController::Command::Right;motion.count=int(std::min<std::size_t>(std::size_t(std::numeric_limits<int>::max()),characters*std::size_t(count)));
        const auto end=VimMotions::resolve(buffer,original,motion,[&]{step(40);}).target;const auto removed=end-original;
        auto payload=std::make_shared<ChunkedFile>();for(auto p=original;p<end;p+=64*1024){payload->append(buffer.slice(p,std::min<std::size_t>(64*1024,end-p)));step(50);}
        for(int n=0;n<count;++n)for(std::size_t p=0;p<normalized->size();p+=64*1024){payload->append(normalized->read(p,std::min<std::size_t>(64*1024,normalized->size()-p)));step(60);}
        const auto inserted=payload->size()-removed;buffer.erase(original,removed);for(std::size_t p=0;p<inserted;p+=64*1024){buffer.insert(original+p,payload->read(removed+p,std::min<std::size_t>(64*1024,inserted-p)));step(80);}
        history.recordOnDisk(original,payload,removed,inserted,original,original+inserted,0);return HistoryResult{original+inserted,{original+inserted}};
    },true,{},[this,leaveReplace](bool){if(leaveReplace)switchVimMode(VimController::Mode::Normal,true);});
}
void EditorWidget::backspaceVimReplace() {
    if(!vimReplaced_.empty() && vimReplaced_.back().begin+vimReplaced_.back().after.size()==cursorPosition_) {
        const auto step=vimReplaced_.back();vimReplaced_.pop_back();const auto capture=std::exchange(vimCaptureInsertEdit_,false);replaceRange(step.begin,step.after.size(),step.before);vimCaptureInsertEdit_=capture;if(step.begin>=vimInsertStart_ && step.begin-vimInsertStart_<=vimInsertedText_.size())vimInsertedText_.erase(step.begin-vimInsertStart_);cursorPosition_=step.begin;cursors_={cursorPosition_};clearAllSelections();ensureCursorVisible();viewport()->update();
    } else if(cursorPosition_>vimLineStart(currentLine()))moveCursor(graphemeBoundaryAt(cursorPosition_,false),false);
}
void EditorWidget::repeatVimChange(int count,bool explicitCount) {
    auto inputs=VimState::Session::instance().lastChange.inputs;if(inputs.isEmpty())return;
    if(explicitCount) {
        qsizetype from=0;if(inputs.size()>1 && inputs[0].text=="\"")from=2;
        auto end=from;while(end<inputs.size() && inputs[end].kind==VimState::Input::Key && inputs[end].text.size()==1 && inputs[end].text[0].isDigit())++end;
        auto motion=end;
        if(motion<inputs.size() && QStringLiteral("dcy<>=").contains(inputs[motion].text) && inputs[motion].text.size()==1)++motion;
        else if(motion+1<inputs.size() && inputs[motion].text=="g" && QStringLiteral("uU~q").contains(inputs[motion+1].text) && inputs[motion+1].text.size()==1)motion+=2;
        if(motion>end){auto countEnd=motion;while(countEnd<inputs.size() && inputs[countEnd].text.size()==1 && inputs[countEnd].text[0].isDigit())++countEnd;inputs.erase(inputs.begin()+motion,inputs.begin()+countEnd);}
        inputs.erase(inputs.begin()+from,inputs.begin()+end);const auto digits=textKeys(QString::number(count));for(qsizetype n=0;n<digits.size();++n)inputs.insert(from+n,digits[n]);
        VimState::Session::instance().lastChange.inputs=inputs;
    }
    vimRepeatingDot_=true;replayVim(std::move(inputs));
}
void EditorWidget::replayVim(QVector<VimState::Input> inputs,int count) {
    if(inputs.isEmpty())return;
    if(vimReplay_.size()>=100){cancelVimReplay();emit highlightingStatusChanged(I18n::text("Vim: macro recursion limit reached"));return;}
    if(vimReplay_.empty()){history_.beginCompoundGroup();vimReplaySteps_=0;vimReplayCancel_=false;}
    vimReplay_.push_back({std::move(inputs),0,count});
    if(!vimReplayScheduled_){vimReplayScheduled_=true;QTimer::singleShot(0,this,&EditorWidget::continueVimReplay);}
}
void EditorWidget::continueVimReplay() {
    vimReplayScheduled_=false;
    if(busy_ || (vimExExecuting_ && !vimSubConfirm_ && !vimExNormal_)){vimReplayScheduled_=true;QTimer::singleShot(5,this,&EditorWidget::continueVimReplay);return;}
    const bool cancelled=vimReplayCancel_ || !vim_.enabled() || !vimOwnsFocus();
    if(cancelled){vimReplay_.clear();if(vimExNormal_)finishVimEx(I18n::text("Vim: command cancelled"),true);}
    for(int batch=0;batch<128 && !vimReplay_.empty() && !busy_ && (!vimExExecuting_ || vimSubConfirm_ || vimExNormal_);++batch) {
        if(++vimReplaySteps_>100000){vimReplay_.clear();if(vimExNormal_)finishVimEx(I18n::text("Vim: replay step limit reached"),true);else emit highlightingStatusChanged(I18n::text("Vim: replay step limit reached"));break;}
        auto& frame=vimReplay_.back();
        if(frame.index==frame.inputs.size()){if(--frame.remaining>0)frame.index=0;else{const bool normal=frame.exNormal;vimReplay_.pop_back();if(normal){vimExNormal_=false;resetVimMode();scheduleVimEx();break;}continue;}}
        const auto input=frame.inputs[frame.index++];history_.beginGroup(true);
        vimSendingReplay_=true;applyVimInput(input);vimSendingReplay_=false;
        if(vimReplayCancel_ || !vimOwnsFocus())break;
    }
    if(!vimReplay_.empty()){vimReplayScheduled_=true;QTimer::singleShot(0,this,&EditorWidget::continueVimReplay);return;}
    if(vimExNormal_)finishVimEx(I18n::text("Vim: command cancelled"),true);
    history_.endCompoundGroup();vimRepeatingDot_=false;vimReplayCancel_=false;if(cancelled)switchVimMode(VimController::Mode::Normal);
    finishVimChange();vimCommandKeys_.clear();refreshVimPresentation();
}
void EditorWidget::cancelVimReplay(){if(vimReplay_.empty() && !vimReplayScheduled_)return;vimReplayCancel_=true;if(!busy_){vimReplay_.clear();history_.endCompoundGroup();vimRepeatingDot_=false;switchVimMode(VimController::Mode::Normal);finishVimChange();}}
void EditorWidget::transferVimReplay(EditorWidget* target) {
    if(!target || target==this || vimReplay_.empty())return;
    history_.endCompoundGroup();target->history_.beginCompoundGroup();target->vimReplay_=std::move(vimReplay_);target->vimReplaySteps_=vimReplaySteps_;target->vimRepeatingDot_=vimRepeatingDot_;target->vimReplayCancel_=false;
    vimRepeatingDot_=false;vimReplay_.clear();
    if(!target->vimReplayScheduled_){target->vimReplayScheduled_=true;QTimer::singleShot(0,target,&EditorWidget::continueVimReplay);}
}
void EditorWidget::applyVimInput(const VimState::Input& input) {
    if(input.kind==VimState::Input::Key){QKeyEvent key(QEvent::KeyPress,input.key,Qt::KeyboardModifiers(input.modifiers),input.text);QApplication::sendEvent(this,&key);return;}
    if(vimExActive_){vimExInput_->insert(input.text);return;}
    if(!vimRepeatingDot_){if(!vimChangeActive_){vimCommandKeys_=textKeys(vim_.mode()==VimController::Mode::Replace?"R":"i");beginVimChange();}vimChangeKeys_.append(input);}
    const auto length=input.payload?(input.length?input.length:input.payload->size()):0;
    if(length>=1024*1024 || std::size_t(input.back)+std::size_t(input.forward)>4096) {
        auto work=input;if(!work.payload){work.payload=std::make_shared<ChunkedFile>();const auto bytes=work.text.toUtf8();work.payload->append({bytes.data(),std::size_t(bytes.size())});work.offset=0;work.length=std::size_t(bytes.size());}
        const auto workLength=work.length?work.length:work.payload->size();
        const auto original=cursorPosition_;runBulkOperation(QStringLiteral("Vim…"),[input=work,original,length=workLength](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
            const auto begin=VimBlock::advance(buffer,original,-input.back,[&]{step(10);}),end=VimBlock::advance(buffer,original,input.forward,[&]{step(20);});
            auto payload=std::make_shared<ChunkedFile>();for(auto p=begin;p<end;p+=64*1024){payload->append(buffer.slice(p,std::min<std::size_t>(64*1024,end-p)));step(30);}
            for(std::size_t p=0;p<length;p+=64*1024){payload->append(input.payload->read(input.offset+p,std::min<std::size_t>(64*1024,length-p)));step(40);}
            buffer.erase(begin,end-begin);for(std::size_t p=0;p<length;p+=64*1024){buffer.insert(begin+p,input.payload->read(input.offset+p,std::min<std::size_t>(64*1024,length-p)));step(80);}
            history.recordOnDisk(begin,payload,end-begin,length,original,begin+length,0);return HistoryResult{begin+length,{begin+length}};
        });return;
    }
    auto begin=cursorPosition_,end=cursorPosition_;
    for(int n=0;n<input.back;++n)begin=graphemeBoundaryAt(begin,false);
    for(int n=0;n<input.forward;++n)end=graphemeBoundaryAt(end,true);
    if(input.kind==VimState::Input::Edit || input.payload){const auto bytes=input.payload?input.payload->read(input.offset,length):input.text.toUtf8().toStdString();const auto removed=buffer_.slice(begin,end-begin);replaceRange(begin,end-begin,bytes);if(vim_.mode()==VimController::Mode::Replace)vimReplaced_.push_back({begin,removed,bytes});if(input.payload)input.payload->dropCache();}
    else if(!input.text.isEmpty())insertText(input.text.toUtf8().toStdString());
}
void EditorWidget::toggleVimRecording(QChar name) {
    auto& shared=VimState::Session::instance();
    if(!shared.recording.isNull()) {
        VimState::Register value;value.text=std::make_shared<ChunkedFile>();value.macro=shared.recordingInputs;
        for(const auto& input:value.macro) {
            if(input.kind==VimState::Input::Key){auto text=input.text;if(input.key==Qt::Key_Escape)text=QChar(27);else if(input.key==Qt::Key_Return || input.key==Qt::Key_Enter)text="\r";else if(input.key==Qt::Key_Backspace)text=QChar(8);const auto bytes=text.toUtf8();value.text->append({bytes.data(),std::size_t(bytes.size())});}
            else {value.text->append(std::string(input.back,'\b'));value.text->append(std::string(input.forward,char(127)));if(input.payload){const auto length=input.length?input.length:input.payload->size();for(std::size_t at=0;at<length;at+=64*1024)value.text->append(input.payload->read(input.offset+at,std::min<std::size_t>(64*1024,length-at)));input.payload->dropCache();}else {const auto bytes=input.text.toUtf8();value.text->append({bytes.data(),std::size_t(bytes.size())});}}
        }
        value.text->dropCache();shared.set(shared.recording,value);shared.recording={};shared.recordingOwner=nullptr;shared.recordingInputs.clear();shared.recordingBytes=0;
    } else if(name.isLetterOrNumber() && name.unicode()<128){const auto old=shared.read(name);if(name.isUpper() && old.text && old.text->size()>1024*1024){emit highlightingStatusChanged(I18n::text("Vim: macro register is too large to append"));return;}shared.recording=name;shared.recordingOwner=this;shared.recordingInputs.clear();shared.recordingBytes=0;}
    emit vimModeChanged();
}
void EditorWidget::playVimMacro(QChar name,int count) {
    auto& shared=VimState::Session::instance();if(name=='@')name=shared.lastMacro;if(name.isNull())return;
    if(name==':' && !shared.lastCommand.isEmpty()){shared.lastMacro=name;replayVim(textKeys(":"+shared.lastCommand+"\r"),count);return;}
    const auto value=shared.read(name);if(value.empty()){if(!vimReplay_.empty())vimReplayCancel_=true;return;}shared.lastMacro=name;
    if(!value.macro.isEmpty()){replayVim(value.macro,count);return;}
    if(value.text->size()>1024*1024){if(!vimReplay_.empty())vimReplayCancel_=true;emit highlightingStatusChanged(I18n::text("Vim: macro is too large"));return;}
    const auto bytes=value.text->read(0,value.text->size());replayVim(textKeys(QString::fromUtf8(bytes.data(),qsizetype(bytes.size()))),count);
}
QString EditorWidget::vimMarkKey() const{return filePath_.isEmpty()?QStringLiteral("buffer:%1").arg(quintptr(this)):filePath_;}
VimState::Anchor EditorWidget::vimLocation(){return VimState::Session::instance().anchor(this,filePath_,cursorPosition_,navigationPosition());}
void EditorWidget::attachVimAnchors(){buffer_.editObserver=[this](std::size_t at,std::size_t removed,std::size_t inserted){projectVimAnchors(at,removed,inserted);};buffer_.lineEditObserver=[this](std::size_t row,std::size_t column,std::size_t removed,std::size_t inserted){projectVimExLines(row,column,removed,inserted);};VimState::Session::instance().attach(this,filePath_);}
void EditorWidget::projectVimAnchors(std::size_t at,std::size_t removed,std::size_t inserted){for(auto& p:VimState::Session::instance().anchors(this)){
    if(removed && p->offset>=at && p->offset<=at+removed && !p->lostPositions.contains(history_.stateId())){p->lostPositions.insert(history_.stateId(),p->offset);if(p->lostPositions.size()>256)p->lostPositions.erase(p->lostPositions.begin());}
    p->offset=VimState::Session::project(p->offset,at,removed,inserted);
}}
void EditorWidget::persistVimAnchors(){VimState::Session::instance().marks(vimMarkKey())['"']=vimLocation();for(auto& p:VimState::Session::instance().anchors(this)){const auto at=std::min(p->offset,buffer_.size()),row=buffer_.lineIndexAt(at),start=buffer_.lineStart(row);p->path=filePath_;p->position=QPoint(int(row+1),int(buffer_.utf16Before(at)-buffer_.utf16Before(start)+1));}}
void EditorWidget::saveVimMark(QChar name){if(!name.isLetter() || name.unicode()>127)return;auto& shared=VimState::Session::instance();if(name.isUpper())shared.globalMarks[name]=vimLocation();else shared.marks(vimMarkKey())[name]=vimLocation();}
void EditorWidget::pushVimJump(){if(!vim_.enabled())return;if(vimExScope_ && vimExScope_->keepJumps)return;for(const auto& frame:vimExFrames_)if(frame.kind==ExFrame::Global || (frame.scope && frame.scope->keepJumps))return;auto& jumps=VimState::Session::instance().jumps(window());auto here=vimLocation();jumps.previous=here;
    while(jumps.entries.size()>jumps.index)jumps.entries.removeLast();
    if(jumps.entries.isEmpty() || jumps.entries.back()->owner!=this || jumps.entries.back()->offset!=cursorPosition_)jumps.entries.append(here);
    if(jumps.entries.size()>500)jumps.entries.removeFirst();
    jumps.index=jumps.entries.size();
}
void EditorWidget::jumpVimMark(QChar name,bool linewise,VimController::Command operation,bool keepJumps){
    if(QStringLiteral("(){}").contains(name)){
        VimController::Action action;action.command=name=='('?VimController::Command::SentenceBack:name==')'?VimController::Command::SentenceNext:name=='{'?VimController::Command::ParagraphBack:VimController::Command::ParagraphNext;
        const auto current=cursorPosition_;const auto finalNewline=vimFinalNewline_;auto offset=std::make_shared<std::size_t>(current);
        runBulkOperation(QStringLiteral("Vim…"),[offset,action,current,finalNewline](PieceTreeBuffer& buffer,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep& step){*offset=VimMotions::resolve(buffer,current,action,[&]{step(50);},finalNewline).target;return HistoryResult{current,{current}};},false,{},[this,offset,linewise,operation,keepJumps](bool ok){if(ok)applyVimMark(VimState::Session::instance().anchor(this,filePath_,std::min(*offset,buffer_.size()),QPoint(1,1)),linewise,operation,keepJumps);});return;
    }
    auto& shared=VimState::Session::instance();const auto target=name=='\'' || name=='`'?shared.jumps(window()).previous:name.isUpper()?shared.globalMarks.value(name):shared.marks(vimMarkKey()).value(name);
    applyVimMark(target,linewise,operation,keepJumps);
}
void EditorWidget::applyVimMark(const VimState::Anchor& target,bool linewise,VimController::Command operation,bool keepJumps){
    if(!target){if(!vimReplay_.empty())vimReplayCancel_=true;emit highlightingStatusChanged(I18n::text("Vim: mark is not set"));return;}
    if(operation!=VimController::Command::Consume) {
        if(target->owner!=this){if(!vimReplay_.empty())vimReplayCancel_=true;emit highlightingStatusChanged(I18n::text("Vim: operator mark must be in this buffer"));return;}
        auto begin=std::min(cursorPosition_,target->offset),end=std::min(buffer_.size(),std::max(cursorPosition_,target->offset));
        if(linewise){const auto first=buffer_.lineIndexAt(begin),last=buffer_.lineIndexAt(end);begin=vimLineStart(first);end=last+1<buffer_.lineCount()?buffer_.lineStart(last+1):buffer_.size();}
        if(operation==VimController::Command::Indent || operation==VimController::Command::Unindent)indentVim(buffer_.lineIndexAt(begin),buffer_.lineIndexAt(end>begin?end-1:begin),operation==VimController::Command::Unindent);
        else {vimForceNumbered_=true;editVimRange(begin,end,operation,linewise);}
        return;
    }
    if(!keepJumps || target->owner!=this)pushVimJump();
    restoreVimLocation(target,linewise);
}
void EditorWidget::jumpVimHistory(int direction,int count){auto& jumps=VimState::Session::instance().jumps(window());if(jumps.entries.isEmpty())return;
    if(jumps.index>=jumps.entries.size()){jumps.entries.append(vimLocation());jumps.index=jumps.entries.size()-1;}
    const auto index=std::clamp(jumps.index+direction*count,0,int(jumps.entries.size()-1));if(index==jumps.index)return;jumps.index=index;restoreVimLocation(jumps.entries[index]);
}
void EditorWidget::restoreVimLocation(const VimState::Anchor& location,bool linewise) {
    if(!location)return;
    switchVimMode(VimController::Mode::Normal);
    if(auto* editor=qobject_cast<EditorWidget*>(location->owner.data())) {
        const auto at=std::min(location->offset,editor->buffer_.size());auto row=editor->buffer_.lineIndexAt(at);
        if(editor==this){moveCursor(linewise?VimMotions::firstText(buffer_,row):at,false);return;}
        const auto start=editor->buffer_.lineStart(row);const int column=linewise?1:int(editor->buffer_.utf16Before(at)-editor->buffer_.utf16Before(start)+1);
        if(!editor->filePath_.isEmpty()){emit vimNavigateRequested(editor->filePath_,int(row+1),column);return;}
        for(auto* parent=editor->parentWidget();parent;parent=parent->parentWidget())if(auto* tabs=qobject_cast<QTabWidget*>(parent)){tabs->setCurrentWidget(editor);editor->resetVimMode();editor->moveCursor(linewise?VimMotions::firstText(editor->buffer_,row):at,false);editor->window()->activateWindow();editor->setFocus();transferVimReplay(editor);return;}
    }
    if(!location->path.isEmpty() && QFileInfo::exists(location->path))emit vimNavigateRequested(location->path,location->position.x(),linewise?1:location->position.y());
    else {if(!vimReplay_.empty())vimReplayCancel_=true;emit highlightingStatusChanged(I18n::text("Vim: marked file no longer exists"));}
}
