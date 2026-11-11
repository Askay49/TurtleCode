#pragma once
#include "core/text/EditHistory.hpp"
#include <algorithm>
#include <numeric>
#include <optional>
#include <vector>

namespace LineOperations {
struct Range {std::size_t first,last;};
struct Plan {
    std::vector<EditChange> changes;
    std::vector<std::size_t> cursors;
    std::vector<std::optional<std::size_t>> anchors;
};
inline std::vector<Range> ranges(const PieceTreeBuffer& buffer,const std::vector<std::size_t>& cursors,
                                const std::vector<std::optional<std::size_t>>& anchors) {
    std::vector<Range> result;
    for(std::size_t i=0;i<cursors.size();++i) {
        const auto anchor=anchors[i].value_or(cursors[i]);
        const auto begin=std::min(anchor,cursors[i]),end=std::max(anchor,cursors[i]);
        auto last=buffer.lineIndexAt(end);
        if(end>begin && end==buffer.lineStart(last))--last;
        result.push_back({buffer.lineIndexAt(begin),last});
    }
    std::sort(result.begin(),result.end(),[](auto a,auto b){return a.first<b.first;});
    std::vector<Range> merged;
    for(auto range:result) {
        if(!merged.empty() && range.first<=merged.back().last+1)merged.back().last=std::max(merged.back().last,range.last);
        else merged.push_back(range);
    }
    return merged;
}
inline std::size_t contentStart(const PieceTreeBuffer& buffer,std::size_t row) {
    auto start=buffer.lineStart(row);
    if(row==0 && buffer.size()>=3 && buffer.slice(0,3)=="\xef\xbb\xbf")start=3;
    return start;
}
inline Plan prepare(const PieceTreeBuffer& buffer,const std::vector<std::size_t>& cursors,
                    const std::vector<std::optional<std::size_t>>& anchors,int direction,bool copy,
                    const std::function<void()>& checkpoint = {}) {
    Plan plan;plan.cursors=cursors;plan.anchors=anchors;
    std::ptrdiff_t delta=0;
    std::string preferred="\n";
    if(buffer.lineCount()>1) {
        const auto end=buffer.lineEnd(0);
        if(end && buffer.byteAt(end-1)=='\r')preferred="\r\n";
    }
    for(const auto block:ranges(buffer,cursors,anchors)) {
        if(checkpoint)checkpoint();
        if(!copy && ((direction<0 && block.first==0) || (direction>0 && block.last+1==buffer.lineCount()))) {
            // Earlier duplicated blocks can shift untouched cursors (moves have delta zero).
            continue;
        }
        const auto first=block.first-(!copy && direction<0?1:0);
        const auto last=block.last+(!copy && direction>0?1:0);
        struct Line {std::size_t start;std::string content,eol;};
        std::vector<Line> lines;
        for(auto row=first;row<=last;++row) {
            if(checkpoint)checkpoint();
            const auto start=contentStart(buffer,row);auto end=buffer.lineEnd(row);
            std::string eol;
            if(end<buffer.size()) {
                eol="\n";
                if(end>start && buffer.byteAt(end-1)=='\r'){--end;eol="\r\n";}
            }
            lines.push_back({start,buffer.slice(start,end-start),eol});
        }
        const auto count=lines.size();
        std::vector<std::size_t> order(count);std::iota(order.begin(),order.end(),0);
        if(copy){const auto original=order;order.insert(order.end(),original.begin(),original.end());}
        else if(direction<0)std::rotate(order.begin(),order.begin()+1,order.end());
        else std::rotate(order.begin(),order.end()-1,order.end());
        const auto begin=lines.front().start;
        const auto end=last+1<buffer.lineCount()?buffer.lineStart(last+1):buffer.size();
        EditChange change{begin,buffer.slice(begin,end-begin),{}};
        std::vector<std::size_t> newStart(count),newLength(count);
        for(std::size_t i=0;i<order.size();++i) {
            if(checkpoint)checkpoint();
            const auto index=order[i];const auto& line=lines[index];
            const auto at=change.inserted.size();change.inserted+=line.content;
            std::string eol;
            if(i+1<order.size())eol=copy?(line.eol.empty()?preferred:line.eol):lines[i].eol;
            else eol=lines.back().eol;
            change.inserted+=eol;
            const bool target=!copy || (direction<0?i<count:i>=count);
            if(target){newStart[index]=at;newLength[index]=line.content.size()+eol.size();}
        }
        for(std::size_t i=0;i<cursors.size();++i) {
            const auto anchor=anchors[i].value_or(cursors[i]);
            const auto low=std::min(anchor,cursors[i]);
            const auto row=buffer.lineIndexAt(low);
            if(row<block.first || row>block.last)continue;
            const auto map=[&](std::size_t position) {
                auto sourceRow=buffer.lineIndexAt(position);
                // A selection ending at column zero does not include that next line.
                if(position>low && sourceRow>block.last && position==buffer.lineStart(sourceRow))--sourceRow;
                sourceRow=std::clamp(sourceRow,block.first,block.last);
                const auto index=sourceRow-first;
                const auto column=position>lines[index].start?position-lines[index].start:0;
                return std::size_t(std::ptrdiff_t(begin)+delta)+newStart[index]+std::min(column,newLength[index]);
            };
            plan.cursors[i]=map(cursors[i]);if(anchors[i])plan.anchors[i]=map(*anchors[i]);
        }
        delta+=std::ptrdiff_t(change.inserted.size())-std::ptrdiff_t(change.removed.size());
        plan.changes.push_back(std::move(change));
    }
    return plan;
}
}
