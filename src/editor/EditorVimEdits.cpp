#include "EditorWidget.hpp"
#include "StoredClipboard.hpp"
#include <QApplication>
#include <QClipboard>
#include <algorithm>

void EditorWidget::editVimRange(std::size_t begin,std::size_t end,VimController::Command operation,bool linewise) {
    using C=VimController::Command;using Mode=VimController::Mode;
    if(operation==C::ToggleCase || operation==C::LowerCase || operation==C::UpperCase || operation==C::AutoIndent || operation==C::Format){transformVimRange(begin,end,operation,linewise);return;}
    if(vim_.mode()==Mode::Visual){vimPreviousVisualRows_=buffer_.lineIndexAt(end>begin?end-1:end)-buffer_.lineIndexAt(begin)+1;vimPreviousVisualWidth_=std::max<std::size_t>(1,buffer_.codePointsBefore(end)-buffer_.codePointsBefore(begin));}
    const bool forceNumbered=std::exchange(vimForceNumbered_,false);
    begin=std::min(begin,buffer_.size());end=std::clamp(end,begin,buffer_.size());
    if(begin==end && operation!=C::Change && !linewise){moveCursor(begin,false);return;}
    const auto selected=vimRegister_;auto captured=std::make_shared<VimState::Register>();captured->kind=linewise?VimState::Register::Line:VimState::Register::Character;
    const auto appendTo=selected.isUpper()?VimState::Session::instance().read(selected.toLower()):VimState::Register{};
    auto appended=std::make_shared<VimState::Register>();
    const bool largeAppend=appendTo.text && appendTo.text->size()+end-begin>=1024*1024;
    const bool smallDelete=!linewise && buffer_.lineIndexAt(begin)==buffer_.lineIndexAt(end);
    const auto original=cursorPosition_,row=buffer_.lineIndexAt(begin);
    const auto yankCursor=vim_.mode()==Mode::Visual?begin:linewise?original:std::min(begin,original);
    auto eraseBegin=begin,eraseEnd=end;std::string replacement;
    const auto eol=SmartEditing::eol(buffer_);
    const bool preserveEol=end<buffer_.size() || (end>begin && buffer_.byteAt(end-1)=='\n');
    if(linewise && operation==C::Change) {
        replacement=buffer_.slice(begin,VimMotions::firstText(buffer_,row)-begin);
        if(preserveEol)replacement+=eol;
    }
    if(linewise && operation==C::Delete && end==buffer_.size() && row>0 && (begin==end || buffer_.byteAt(end-1)!='\n'))eraseBegin=lineContentEnd(row-1);
    const auto cursorAfter=operation==C::Change?begin+(linewise?replacement.size()-(preserveEol?eol.size():0):0):eraseBegin;
    switchVimMode(operation==C::Change?Mode::Insert:Mode::Normal);
    const auto finish=[this,operation,linewise,cursorAfter,yankCursor,selected,captured,smallDelete,forceNumbered,appended](bool success) {
        if(!success){if(operation==VimController::Command::Change)switchVimMode(VimController::Mode::Normal);return;}
        storeVimRegister(selected,*captured,operation==VimController::Command::Yank,smallDelete,forceNumbered,appended->text?appended.get():nullptr);
        if(operation==VimController::Command::Yank)cursorPosition_=yankCursor;
        else cursorPosition_=std::min(cursorAfter,buffer_.size());
        if(linewise && operation==VimController::Command::Delete)cursorPosition_=VimMotions::firstText(buffer_,std::min(currentLine(),VimMotions::lastLine(buffer_,vimFinalNewline_)));
        cursors_={cursorPosition_};clearAllSelections();normalizeVimCursor();ensureCursorVisible();viewport()->update();
    };
    if(end-begin>=1024*1024 || eraseEnd-eraseBegin>=1024*1024 || largeAppend) {
        runBulkOperation(QStringLiteral("Vim…"),[begin,end,eraseBegin,eraseEnd,replacement,operation,linewise,original,cursorAfter,eol,captured,appended,appendTo,largeAppend](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
            auto clipboard=std::make_shared<ChunkedFile>();
            for(auto at=begin;at<end;at+=64*1024){clipboard->append(buffer.slice(at,std::min<std::size_t>(64*1024,end-at)));step(20);}
            if(linewise && (end==begin || buffer.byteAt(end-1)!='\n'))clipboard->append(eol);
            if(operation!=VimController::Command::Yank) {
                auto payload=std::make_shared<ChunkedFile>();
                for(auto at=eraseBegin;at<eraseEnd;at+=64*1024){payload->append(buffer.slice(at,std::min<std::size_t>(64*1024,eraseEnd-at)));step(50);}
                payload->append(replacement);buffer.erase(eraseBegin,eraseEnd-eraseBegin);buffer.insert(eraseBegin,replacement);
                history.recordOnDisk(eraseBegin,payload,eraseEnd-eraseBegin,replacement.size(),original,cursorAfter,0);
            }
            captured->text=VimState::convertNewlines(clipboard,"\n",[&]{step(85);});clipboard->dropCache();if(largeAppend)*appended=VimState::append(appendTo,*captured,[&]{step(90);});return HistoryResult{cursorAfter,{cursorAfter}};
        },operation!=C::Yank,{},finish);
        return;
    }
    auto bytes=buffer_.slice(begin,end-begin);if(linewise && (bytes.empty() || bytes.back()!='\n'))bytes+=eol;
    captured->text=std::make_shared<ChunkedFile>();captured->text->append(bytes);captured->text=VimState::convertNewlines(captured->text,"\n");
    if(operation!=C::Yank && (eraseEnd>eraseBegin || !replacement.empty()))replaceRange(eraseBegin,eraseEnd-eraseBegin,replacement);
    finish(true);
}

void EditorWidget::pasteVim(bool before,int count,bool ex) {
    refreshEditorConfig();const auto eol=editorConfig_.eol.value_or(SmartEditing::eol(buffer_));
    const auto storedValue=vimPutValue_?std::exchange(vimPutValue_,{}).value():VimState::Session::instance().read(vimRegister_);
    if(storedValue.empty())return;
    if(storedValue.kind==VimState::Register::Block){putVimBlock(storedValue,before,count);return;}
    const bool linewise=storedValue.kind==VimState::Register::Line;
    auto source=storedValue.text;
    const auto row=currentLine();const auto visual=vim_.mode()==VimController::Mode::Visual;
    const auto selection=selectionRange();auto begin=cursorPosition_,end=cursorPosition_;
    std::string prefix;
    if(visual){begin=selection.first;end=selection.second;}
    else if(linewise) {
        begin=end=before?vimLineStart(row):(row+1<buffer_.lineCount()?buffer_.lineStart(row+1):buffer_.size());
        if(!before && row+1==buffer_.lineCount()){prefix=eol;}
    } else if(!before)begin=end=std::min(lineContentEnd(row),graphemeBoundaryAt(cursorPosition_,true));
    switchVimMode(VimController::Mode::Normal);
    const auto finish=[this,begin,linewise,prefix,ex](std::size_t inserted) {
        cursorPosition_=ex && inserted?begin+inserted-1:linewise?begin+prefix.size():inserted?graphemeBoundaryAt(begin+inserted,false):begin;
        if(linewise)cursorPosition_=VimMotions::firstText(buffer_,buffer_.lineIndexAt(cursorPosition_));
        cursors_={cursorPosition_};clearAllSelections();normalizeVimCursor();ensureCursorVisible();viewport()->update();
    };
    auto inserted=std::make_shared<std::size_t>(0);const auto original=cursorPosition_;
    runBulkOperation(QStringLiteral("Vim…"),[begin,end,prefix,source,count,inserted,original,linewise,eol,ex](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
        auto input=VimState::convertNewlines(source,eol,[&]{step(5);});
        if(ex && (input->size()==0 || input->read(input->size()-1,1)!="\n"))input->append(eol);
        auto payload=std::make_shared<ChunkedFile>();for(auto at=begin;at<end;at+=64*1024){payload->append(buffer.slice(at,std::min<std::size_t>(64*1024,end-at)));step(10);}
        buffer.erase(begin,end-begin);std::size_t written=0;auto append=[&](std::string_view value){payload->append(value);buffer.insert(begin+written,value);written+=value.size();step(50);};append(prefix);
        for(int n=0;n<count;++n)for(std::size_t at=0;at<input->size();at+=64*1024){auto length=std::min<std::size_t>(64*1024,input->size()-at);append(input->read(at,length));}
        if(linewise && !prefix.empty() && written && buffer.byteAt(begin+written-1)=='\n') {
            // Keep the existing missing final newline when putting below EOF.
            std::size_t trim=1;if(written>1 && buffer.byteAt(begin+written-2)=='\r')++trim;
            buffer.erase(begin+written-trim,trim);written-=trim;
            auto corrected=std::make_shared<ChunkedFile>();for(std::size_t at=0;at<end-begin+written;at+=64*1024){corrected->append(payload->read(at,std::min<std::size_t>(64*1024,end-begin+written-at)));step(80);}payload=corrected;
        }
        *inserted=written;history.recordOnDisk(begin,payload,end-begin,written,original,begin+written,0);return HistoryResult{begin+written,{begin+written}};
    },true,{},[finish,inserted](bool success){if(success)finish(*inserted);});return;
}

void EditorWidget::indentVim(std::size_t first,std::size_t last,bool unindent,int count) {
    const auto column=cursorPosition_-vimLineStart(currentLine());
    switchVimMode(VimController::Mode::Normal);refreshEditorConfig();
    const auto unit=editorConfig_.indentation(),tabWidth=editorConfig_.tabSize();const auto width=unit*count;const bool tabs=editorConfig_.tabs.value_or(false);
    const auto start=vimLineStart(first),end=last+1<buffer_.lineCount()?buffer_.lineStart(last+1):buffer_.size();
    cursorPosition_=end;anchor_=start;cursors_={end};cursorAnchors_={anchor_};
    runSmartCommand([first,last,width,tabWidth,tabs,unindent,column](const PieceTreeBuffer& buffer,const std::function<void()>& checkpoint) {
        SmartEditing::Plan plan;
        for(auto row=first;row<=last;++row) {
            if(checkpoint)checkpoint();
            const auto begin=VimMotions::lineStart(buffer,row),text=VimMotions::firstText(buffer,row);
            if(begin==VimMotions::lineEnd(buffer,row))continue;
            if(!unindent)plan.changes.push_back({begin,{},tabs?std::string(width/tabWidth,'\t')+std::string(width%tabWidth,' '):std::string(width,' ')});
            else {auto p=begin;int columns=0;while(p<text && columns<width){columns+=buffer.byteAt(p)=='\t'?tabWidth-columns%tabWidth:1;++p;}if(p>begin)plan.changes.push_back({begin,buffer.slice(begin,p-begin),std::string(std::max(0,columns-width),' ')});}
        }
        const auto begin=VimMotions::lineStart(buffer,first);auto length=VimMotions::lineEnd(buffer,first)-begin;
        for(const auto& change:plan.changes)if(change.offset==begin)length=length-change.removed.size()+change.inserted.size();
        SmartEditing::finish(plan,{begin},{std::nullopt});plan.cursors={begin+std::min(column,length)};return plan;
    });
}
