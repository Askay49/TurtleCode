#include "EditorWidget.hpp"
#include "VimNormal.hpp"
#include "app/I18n.hpp"
#include <QTimer>
#include <algorithm>

void EditorWidget::editVimNormal(const VimController::Action& action){
    using C=VimController::Command;const bool visual=vim_.mode()==VimController::Mode::Visual;
    const auto selected=selectionRange();const auto original=cursorPosition_;
    if(action.command==C::Join){
        const auto first=visual?buffer_.lineIndexAt(selected.first):currentLine();
        const auto last=visual?std::max(first+1,buffer_.lineIndexAt(selected.second>selected.first?selected.second-1:selected.first)):first+std::size_t(std::max(2,action.count))-1;
        if(first>=VimMotions::lastLine(buffer_,vimFinalNewline_)){if(isVimReplaying())vimReplayCancel_=true;return;}
        switchVimMode(VimController::Mode::Normal);history_.beginGroup(true);VimEx::Invocation command;command.definition.name="join";command.first=qint64(first+1);command.last=qint64(std::min(last,VimMotions::lastLine(buffer_,vimFinalNewline_))+1);command.explicitRange=true;command.bang=action.around;editVimExLines(command,true);return;
    }
    std::vector<VimMotions::Range> ranges;
    if(visual){
        if(vimVisualBlock_)for(const auto& [begin,end]:vimBlockRanges())ranges.push_back({begin,begin,end,true});
        else if(action.command==C::Increment || action.command==C::Decrement){
            const auto first=buffer_.lineIndexAt(selected.first),last=buffer_.lineIndexAt(selected.second>selected.first?selected.second-1:selected.first);
            for(auto row=first;row<=last;++row){const auto begin=std::max(selected.first,vimLineStart(row)),end=std::min(selected.second,lineContentEnd(row));ranges.push_back({begin,begin,end,true});}
        }else ranges.push_back({selected.first,selected.first,selected.second,true,vimVisualLine_});
    }else{
        const auto begin=cursorPosition_;auto end=begin;
        if(action.command==C::ToggleCase)for(int n=0;n<action.count && end<lineContentEnd(currentLine());++n)end=graphemeBoundaryAt(end,true);
        else end=lineContentEnd(currentLine());
        ranges.push_back({begin,begin,end,true});
    }
    if(ranges.empty())return;
    refreshEditorConfig();VimNormal::Options options;const auto& state=VimState::Session::instance();options.numberFormats=state.numberFormats;
    if((action.command==C::Increment || action.command==C::Decrement) && buffer_.size()<65536){
        bool candidate=false;for(const auto& range:ranges){const auto row=buffer_.lineIndexAt(range.begin);const auto bytes=buffer_.slice(vimLineStart(row),lineContentEnd(row)-vimLineStart(row));for(const auto c:bytes)if((c>='0' && c<='9') || (options.numberFormats.contains("alpha") && ((c>='a' && c<='z') || (c>='A' && c<='Z')))){candidate=true;break;}}
        if(!candidate){if(visual)switchVimMode(VimController::Mode::Normal);return;}
    }
    const auto target=visual?ranges.front().begin:action.command==C::ToggleCase?ranges.back().end:original;
    const auto anchors=VimState::Session::instance().anchors(document_.get());std::vector<std::size_t> positions;for(const auto& anchor:anchors)positions.push_back(anchor->offset);
    auto projected=std::make_shared<std::vector<std::size_t>>();auto bounds=std::make_shared<std::optional<std::pair<std::size_t,std::size_t>>>();
    switchVimMode(VimController::Mode::Normal);history_.beginGroup(true);auto after=std::make_shared<std::size_t>(target);
    runBulkOperation(QStringLiteral("Vim…"),[ranges,action,options,target,original,visual,after,positions,projected,bounds](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        const auto plan=action.command==C::Increment || action.command==C::Decrement?VimNormal::number(buffer,ranges,action.count,action.command==C::Decrement,action.around,visual,options,[&]{step(30);}):VimNormal::changeCase(buffer,ranges,action.command,target,[&]{step(30);});
        if(action.command!=C::Increment && action.command!=C::Decrement)for(const auto position:positions){step(50);projected->push_back(VimNormal::project(plan,position));}
        if(plan.edits->count && action.command!=C::Increment && action.command!=C::Decrement)*bounds=std::pair{VimNormal::project(plan,ranges.front().begin),VimNormal::project(plan,ranges.back().end)};
        *after=plan.cursor;return VimNormal::apply(plan,buffer,history,original,[&]{step(70);});
    },true,{},[this,after,anchors,projected,bounds](bool ok){if(!ok)return;for(std::size_t i=0;i<projected->size();++i)anchors[qsizetype(i)]->offset=(*projected)[i];if(*bounds)setVimEditMarks((**bounds).first,(**bounds).second);moveCursor(std::min(*after,buffer_.size()),false);});
}
void EditorWidget::transformVimRange(std::size_t begin,std::size_t end,VimController::Command operation,bool linewise){
    using C=VimController::Command;const auto original=cursorPosition_,first=buffer_.lineIndexAt(begin),last=buffer_.lineIndexAt(end>begin?end-1:begin);
    refreshEditorConfig();VimNormal::Options options;options.shiftWidth=editorConfig_.indentation();options.tabWidth=editorConfig_.tabSize();options.tabs=editorConfig_.tabs.value_or(false);
    const auto& state=VimState::Session::instance();options.joinSpaces=state.joinSpaces;options.textWidth=state.textWidth?state.textWidth:79;
    const auto eol=editorConfig_.eol.value_or(SmartEditing::eol(buffer_));const auto anchors=VimState::Session::instance().anchors(document_.get());std::vector<std::size_t> positions;for(const auto& anchor:anchors)positions.push_back(anchor->offset);auto projected=std::make_shared<std::vector<std::size_t>>();auto bounds=std::make_shared<std::optional<std::pair<std::size_t,std::size_t>>>();
    switchVimMode(VimController::Mode::Normal);history_.beginGroup(true);auto after=std::make_shared<std::size_t>(begin);
    runBulkOperation(QStringLiteral("Vim…"),[begin,end,operation,linewise,options,eol,original,first,last,after,positions,projected,bounds](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        auto plan=operation==C::AutoIndent?VimNormal::indent(buffer,first,last,options,[&]{step(30);}):operation==C::Format?VimNormal::format(buffer,first,last,options,eol,[&]{step(30);}):VimNormal::changeCase(buffer,{{begin,begin,end,true,linewise}},operation,begin,[&]{step(30);});
        if(operation==C::ToggleCase || operation==C::LowerCase || operation==C::UpperCase)for(const auto position:positions){step(50);projected->push_back(VimNormal::project(plan,position));}
        if(plan.edits->count && (operation==C::ToggleCase || operation==C::LowerCase || operation==C::UpperCase))*bounds=std::pair{VimNormal::project(plan,begin),VimNormal::project(plan,end)};
        auto result=VimNormal::apply(plan,buffer,history,original,[&]{step(70);});
        if(operation==C::AutoIndent || (linewise && operation!=C::Format)){result.cursorPosition=VimMotions::firstText(buffer,first);result.cursorPositions={result.cursorPosition};}*after=result.cursorPosition;return result;
    },true,{},[this,after,anchors,projected,bounds](bool ok){if(!ok)return;for(std::size_t i=0;i<projected->size();++i)anchors[qsizetype(i)]->offset=(*projected)[i];if(*bounds)setVimEditMarks((**bounds).first,(**bounds).second);moveCursor(std::min(*after,buffer_.size()),false);});
}
void EditorWidget::setVimEditMarks(std::size_t begin,std::size_t end){
    auto& state=VimState::Session::instance();auto& marks=state.marks(vimMarkKey());begin=std::min(begin,buffer_.size());end=std::min(end,buffer_.size());
    marks['[']=state.anchor(document_.get(),filePath_,begin,QPoint(1,1));marks[']']=state.anchor(document_.get(),filePath_,end>begin?graphemeBoundaryAt(end,false):begin,QPoint(1,1));marks['.']=marks['['];vimPendingChange_=marks['.'];
}
void EditorWidget::noteVimEdit(std::size_t begin,std::size_t length){
    if(!vim_.enabled() || vimApplyingHistory_)return;
    begin=std::min(begin,buffer_.size());const auto end=std::min(begin+length,buffer_.size());const auto last=end>begin?graphemeBoundaryAt(end,false):begin;
    auto& state=VimState::Session::instance();auto& marks=state.marks(vimMarkKey());
    if(!vimPendingChange_)marks['[']=state.anchor(document_.get(),filePath_,begin,QPoint(1,1));
    marks[']']=state.anchor(document_.get(),filePath_,vim_.acceptsText()?end:last,QPoint(1,1));marks['.']=marks['['];vimPendingChange_=marks['.'];
    if(!vimChangeActive_ && !vimExExecuting_ && !isVimReplaying())commitVimChangeLocation();
}
void EditorWidget::commitVimChangeLocation(){
    if(!vimPendingChange_)return;
    auto location=std::exchange(vimPendingChange_,{});
    if(!vimChanges_.isEmpty() && buffer_.lineIndexAt(vimChanges_.back()->offset)==buffer_.lineIndexAt(location->offset) && std::max(vimChanges_.back()->offset,location->offset)-std::min(vimChanges_.back()->offset,location->offset)<79)vimChanges_.back()=location;
    else {vimChanges_.append(location);if(vimChanges_.size()>100)vimChanges_.removeFirst();}
    vimChangeIndex_=vimChanges_.size();
}
void EditorWidget::jumpVimChange(int direction,int count){
    commitVimChangeLocation();if(vimChanges_.isEmpty()){emit highlightingStatusChanged(I18n::text("Vim: change list is empty"));return;}
    const auto index=std::clamp(vimChangeIndex_+direction*count,0,int(vimChanges_.size()-1));vimChangeIndex_=index;
    // The change list is buffer-local and does not modify the jump list.
    moveCursor(std::min(vimChanges_[index]->offset,buffer_.size()),false);
}
