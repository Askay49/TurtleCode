#include "EditorWidget.hpp"
#include "app/ThemeManager.hpp"
#include "UnicodeNavigation.hpp"
#include <QPainter>
#include <QScrollBar>
#include <algorithm>
#include <climits>

namespace {
struct BlockResult {VimState::Register value,appended;std::vector<std::size_t> cursors;};
void appendRange(ChunkedFile& file,const PieceTreeBuffer& buffer,std::size_t begin,std::size_t end,const std::function<void()>& checkpoint){for(auto p=begin;p<end;p+=64*1024){if(checkpoint)checkpoint();file.append(buffer.slice(p,std::min<std::size_t>(64*1024,end-p)));}}
}
std::pair<int,int> EditorWidget::vimBlockColumns() const {
    const int cursor=vimBlockVirtualColumn_<0?VimBlock::column(buffer_,currentLine(),cursorPosition_,tabWidth_):vimBlockVirtualColumn_;
    return {std::min(vimBlockAnchorColumn_,cursor),vimBlockToEnd_?INT_MAX:std::max(vimBlockAnchorEnd_,cursor+1)};
}
std::vector<std::pair<std::size_t,std::size_t>> EditorWidget::vimBlockRanges() const {
    std::vector<std::pair<std::size_t,std::size_t>> ranges;if(!anchor_)return ranges;
    const auto first=buffer_.lineIndexAt(std::min(cursorPosition_,*anchor_)),last=buffer_.lineIndexAt(std::max(cursorPosition_,*anchor_));const auto [left,right]=vimBlockColumns();
    for(auto row=first;row<=last;++row){const auto range=VimBlock::slice(buffer_,row,left,right,tabWidth_,{},[](std::string_view){});ranges.emplace_back(range.begin,range.end);}return ranges;
}
void EditorWidget::drawVimBlock(QPainter& painter) const {
    if(!vimVisualBlock_ || !anchor_)return;
    const auto [left,right]=vimBlockColumns();
    const auto first=buffer_.lineIndexAt(std::min(cursorPosition_,*anchor_)),last=buffer_.lineIndexAt(std::max(cursorPosition_,*anchor_));
    const int cell=std::max(1,fontMetrics().horizontalAdvance('M')),height=lineHeight(),top=verticalScrollBar()->value();
    auto color=ThemeManager::instance().color("editor.selection");color.setAlpha(120);
    for(int visual=top;visual<=top+viewport()->height()/height;++visual){const auto row=documentLineForVisualRow(visual);if(row<0 || std::size_t(row)<first || std::size_t(row)>last)continue;
        const int end=right==INT_MAX?VimBlock::column(buffer_,std::size_t(row),lineContentEnd(std::size_t(row)),tabWidth_):right;
        const auto slice=screenSlice(visual);
        const int leftOffset=(slice.left || slice.right>=0) ? slice.left : horizontalScrollBar()->value();
        const qint64 x=qint64(gutterWidth()+12)+qint64(left)*cell-leftOffset;
        const qint64 finish=qint64(gutterWidth()+12)+qint64(std::max(left+1,end))*cell-leftOffset;
        painter.fillRect(QRect(int(std::clamp<qint64>(x,gutterWidth(),viewport()->width())),(visual-top)*height,std::max(0,int(std::min<qint64>(finish,viewport()->width())-std::max<qint64>(x,gutterWidth()))),height),color);
    }
}
void EditorWidget::editVimBlock(VimController::Command command,int count,const QString& replacement) {
    using C=VimController::Command;using Mode=VimController::Mode;if(!anchor_)return;
    const auto first=buffer_.lineIndexAt(std::min(cursorPosition_,*anchor_)),last=buffer_.lineIndexAt(std::max(cursorPosition_,*anchor_));
    const auto [left,right]=vimBlockColumns();const auto selected=vimRegister_;const int tabWidth=tabWidth_;
    const auto appendTo=selected.isUpper()?VimState::Session::instance().read(selected.toLower()):VimState::Register{};
    if(command==C::Indent || command==C::Unindent){indentVim(first,last,command==C::Unindent,count);return;}
    const bool yank=command==C::Yank,insert=command==C::Change || command==C::Substitute || command==C::InsertFirst || command==C::InsertLast;
    const bool onlyInsert=command==C::InsertFirst || command==C::InsertLast;
    const bool append=command==C::InsertLast;const auto original=cursorPosition_;
    switchVimMode(insert?Mode::Insert:Mode::Normal);vimBlockInsert_=insert;vimBlockChange_=command==C::Change || command==C::Substitute;
    auto result=std::make_shared<BlockResult>();result->value.kind=VimState::Register::Block;result->value.width=right==INT_MAX?0:right-left;result->value.column=left;
    runBulkOperation(QStringLiteral("Vim…"),[first,last,left,right,tabWidth,command,replacement,yank,insert,onlyInsert,append,original,result,appendTo](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
        auto payload=std::make_shared<ChunkedFile>();result->value.text=std::make_shared<ChunkedFile>();
        std::vector<EditHistory::StoredChange> edits;struct Row {std::size_t at,removed,inserted,data,caret;};std::vector<Row> rows;std::ptrdiff_t delta=0;
        for(auto row=first;row<=last;++row) {
            step(int((row-first)*50/std::max<std::size_t>(1,last-first+1)));
            std::string collected;std::size_t characters=0;
            auto flush=[&]{if(!collected.empty()){result->value.text->append(collected);collected.clear();}};
            auto sink=[&](std::string_view bytes){collected+=bytes;characters+=bytes.size()==1 || bytes.find_first_not_of(' ')==std::string_view::npos?bytes.size():1;if(collected.size()>=64*1024)flush();};
            if(row>first)result->value.text->append("\n");
            const int atColumn=append?(right==INT_MAX?VimBlock::column(buffer,row,VimMotions::lineEnd(buffer,row),tabWidth):right):left;
            auto range=VimBlock::slice(buffer,row,onlyInsert?atColumn:left,onlyInsert?atColumn:right,tabWidth,[&]{step(30);},sink);flush();
            result->value.width=std::max(result->value.width,std::min(right,range.column)-left);
            if(yank)continue;
            if(insert && !append && range.gap>0)continue;
            appendRange(*payload,buffer,range.begin,range.end,[&]{step(40);});const auto data=payload->size();
            payload->append(range.before);
            if(insert)for(int gap=range.gap;gap>0;gap-=64*1024){payload->append(std::string(std::min(gap,64*1024),' '));step(50);}
            const auto caret=payload->size()-data;
            if(command==C::ReplaceCharacter){const auto bytes=replacement.toUtf8().toStdString();std::string value;for(int n=0;n<std::max(0,std::min(right,range.column)-left);++n){value+=bytes;if(value.size()>=64*1024){payload->append(value);value.clear();step(50);}}payload->append(value);}
            payload->append(range.after);const auto inserted=payload->size()-data;
            edits.push_back({range.begin,range.end-range.begin,inserted});rows.push_back({range.begin,range.end-range.begin,inserted,data,caret});
            result->cursors.push_back(std::size_t(std::ptrdiff_t(range.begin+caret)+delta));delta+=std::ptrdiff_t(inserted)-std::ptrdiff_t(range.end-range.begin);
        }
        if(!yank) {
            for(auto it=rows.rbegin();it!=rows.rend();++it){step(70);buffer.erase(it->at,it->removed);for(std::size_t at=0;at<it->inserted;at+=64*1024){buffer.insert(it->at+at,payload->read(it->data+at,std::min<std::size_t>(64*1024,it->inserted-at)));step(80);}}
            history.recordDiskBatch(edits,payload,{original},result->cursors,0);
        }
        result->value.text->dropCache();if(appendTo.text)result->appended=VimState::append(appendTo,result->value,[&]{step(90);});return HistoryResult{result->cursors.empty()?original:result->cursors.front(),result->cursors.empty()?std::vector<std::size_t>{original}:result->cursors};
    },!yank,{},[this,result,selected,yank,insert,onlyInsert,first,last,left,tabWidth,command](bool success){
        if(!success){switchVimMode(Mode::Normal);return;}
        if(!onlyInsert && command!=C::ReplaceCharacter)storeVimRegister(selected,result->value,yank,first==last,false,result->appended.text?&result->appended:nullptr);
        if(insert){cursors_=result->cursors;cursorAnchors_.assign(cursors_.size(),std::nullopt);syncPrimaryCursor();vimInsertStart_=cursorPosition_;vimBlockInsert_=true;vimBlockExitPosition_=VimBlock::slice(buffer_,first,left,left,tabWidth).begin;}
        else {const auto range=VimBlock::slice(buffer_,std::min(first,VimMotions::lastLine(buffer_,vimFinalNewline_)),left,left,tabWidth);moveCursor(range.begin,false);}
        ensureCursorVisible();viewport()->update();
    });
}
void EditorWidget::putVimBlock(const VimState::Register& value,bool before,int count) {
    using Mode=VimController::Mode;
    const bool visual=vimVisualBlock_ && anchor_;const auto columns=visual?vimBlockColumns():std::pair<int,int>{0,0};
    const auto first=visual?buffer_.lineIndexAt(std::min(cursorPosition_,*anchor_)):currentLine();
    const auto last=visual?buffer_.lineIndexAt(std::max(cursorPosition_,*anchor_)):first;
    const int column=visual?columns.first:VimBlock::column(buffer_,currentLine(),before?cursorPosition_:std::min(lineContentEnd(currentLine()),graphemeBoundaryAt(cursorPosition_,true)),tabWidth_);
    const int tabWidth=tabWidth_;const auto original=cursorPosition_;switchVimMode(Mode::Normal);auto deleted=std::make_shared<VimState::Register>();deleted->kind=VimState::Register::Block;deleted->width=columns.second==INT_MAX?0:columns.second-columns.first;deleted->column=column;
    runBulkOperation(QStringLiteral("Vim…"),[value,first,last,visual,columns,column,count,tabWidth,original,deleted](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
        history.beginGroup(true);
        PieceTreeBuffer source;
        for(std::size_t p=0;p<value.text->size();p+=64*1024){const auto bytes=value.text->read(p,std::min<std::size_t>(64*1024,value.text->size()-p));source.insert(source.size(),bytes);step(10);}
        value.text->dropCache();
        const auto sourceRows=value.kind==VimState::Register::Line?VimMotions::lastLine(source,true)+1:source.lineCount();
        const bool characterPut=visual && value.kind==VimState::Register::Character && sourceRows==1;
        auto payload=std::make_shared<ChunkedFile>();std::vector<EditHistory::StoredChange> edits;std::vector<std::pair<std::size_t,std::size_t>> values;
        const auto needed=std::max<std::size_t>(sourceRows,visual?last-first+1:1);std::size_t cursor=original;
        const auto eol=SmartEditing::eol(buffer);
        if(first+needed>buffer.lineCount()) {
            const auto at=buffer.size();auto extra=std::make_shared<ChunkedFile>();
            for(std::size_t n=buffer.lineCount();n<first+needed;++n){extra->append(eol);if(n%256==0)step(15);}
            for(std::size_t p=0;p<extra->size();p+=64*1024){buffer.insert(at+p,extra->read(p,std::min<std::size_t>(64*1024,extra->size()-p)));step(20);}
            history.recordOnDisk(at,extra,0,extra->size(),original,original,0);
        }
        deleted->text=std::make_shared<ChunkedFile>();std::string removedText;
        const auto collect=[&](std::string_view bytes){removedText+=bytes;if(removedText.size()>=64*1024){deleted->text->append(removedText);removedText.clear();}};
        for(std::size_t n=0;n<needed;++n){step(30);const auto row=first+n;if(visual && n && row<=last)collect("\n");
            auto range=VimBlock::slice(buffer,row,column,visual && row<=last?columns.second:column,tabWidth,[&]{step(40);},[&](std::string_view bytes){if(visual && row<=last)collect(bytes);});
            if(visual && row<=last)deleted->width=std::max(deleted->width,std::max(0,std::min(columns.second,range.column)-column));
            appendRange(*payload,buffer,range.begin,range.end,[&]{step(40);});const auto data=payload->size();std::string pending;
            const auto append=[&](std::string_view bytes){pending+=bytes;if(pending.size()>=64*1024){payload->append(pending);pending.clear();step(50);}};
            append(range.before);for(int gap=range.gap;gap>0;gap-=64*1024)append(std::string(std::min(gap,64*1024),' '));
            const auto sourceRow=visual && value.kind==VimState::Register::Character && sourceRows==1?0:n;
            if(sourceRow<sourceRows)for(int k=0;k<count;++k){int width=0;VimBlock::forEachCell(source,sourceRow,tabWidth,[&](std::size_t,std::size_t,int col,int size,std::string_view bytes){if(!size)return false;width=col+size-value.column;if(bytes=="\t")append(std::string(size,' '));else append(bytes);return true;},[&]{step(50);},value.column);
                if(k+1<count)for(int gap=std::max(0,value.width-width);gap>0;gap-=64*1024)append(std::string(std::min(gap,64*1024),' '));}
            append(range.after);if(!pending.empty())payload->append(pending);const auto length=payload->size()-data;
            edits.push_back({range.begin,range.end-range.begin,length});values.emplace_back(data,length);if(!n)cursor=characterPut?range.begin+length-range.after.size():range.begin+range.before.size()+range.gap;
        }
        for(std::size_t n=edits.size();n-->0;){const auto& edit=edits[n];buffer.erase(edit.offset,edit.removed);for(std::size_t p=0;p<edit.inserted;p+=64*1024){buffer.insert(edit.offset+p,payload->read(values[n].first+p,std::min<std::size_t>(64*1024,edit.inserted-p)));step(80);}}
        if(characterPut)cursor=VimBlock::advance(buffer,cursor,-1,[&]{step(85);});
        if(!removedText.empty())deleted->text->append(removedText);
        deleted->text->dropCache();history.recordDiskBatch(edits,payload,{original},{cursor},0);history.endGroup();return HistoryResult{cursor,{cursor}};
    },true,{},[this,deleted,visual,before,first,last](bool success){if(success && visual && !before)storeVimRegister({},*deleted,false,first==last);});
}
