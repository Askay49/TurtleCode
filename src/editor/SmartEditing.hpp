#pragma once
#include "CodeContext.hpp"
#include "LineOperations.hpp"
#include <functional>
#include <stdexcept>

namespace SmartEditing {
using Plan=LineOperations::Plan;
inline std::string eol(const PieceTreeBuffer& buffer) {
    if(buffer.lineCount()>1){const auto end=buffer.lineEnd(0);if(end && buffer.byteAt(end-1)=='\r')return "\r\n";}
    return "\n";
}
inline std::size_t map(std::size_t position,const std::vector<EditChange>& changes,bool right=true) {
    std::ptrdiff_t delta=0;
    for(const auto& change:changes) {
        if(position<change.offset || (position==change.offset && !right))break;
        const auto end=change.offset+change.removed.size();
        if(position<=end)return static_cast<std::size_t>(std::ptrdiff_t(change.offset)+delta+
            (position==end?change.inserted.size():std::min(position-change.offset,change.inserted.size())));
        delta+=std::ptrdiff_t(change.inserted.size())-std::ptrdiff_t(change.removed.size());
    }
    return static_cast<std::size_t>(std::ptrdiff_t(position)+delta);
}
inline void finish(Plan& plan,const std::vector<std::size_t>& cursors,const std::vector<std::optional<std::size_t>>& anchors,bool selectIndent=false) {
    std::stable_sort(plan.changes.begin(),plan.changes.end(),[](const auto& a,const auto& b){return a.offset<b.offset;});
    std::vector<EditChange> merged;
    for(auto& change:plan.changes) {
        if(change.removed==change.inserted)continue;
        if(!merged.empty()) {
            auto& last=merged.back();
            if(last.offset==change.offset && last.removed.empty() && change.removed.empty()){last.inserted+=change.inserted;continue;}
            if(change.offset<last.offset+last.removed.size() || change.offset==last.offset)throw std::runtime_error("Overlapping selections cannot be edited together.");
        }
        merged.push_back(std::move(change));
    }
    plan.changes=std::move(merged);plan.cursors.clear();plan.anchors.clear();
    for(std::size_t i=0;i<cursors.size();++i) {
        const auto anchor=anchors[i].value_or(cursors[i]);
        const auto low=std::min(anchor,cursors[i]);
        plan.cursors.push_back(map(cursors[i],plan.changes,!(selectIndent && cursors[i]==low && anchor!=cursors[i])));
        plan.anchors.push_back(anchors[i]?std::optional<std::size_t>(map(anchor,plan.changes,!(selectIndent && anchor==low && anchor!=cursors[i]))):std::nullopt);
    }
}
inline std::size_t indentationEnd(const PieceTreeBuffer& buffer,std::size_t row) {
    auto p=LineOperations::contentStart(buffer,row);const auto end=buffer.lineEnd(row);CodeContext::Reader reader(buffer);
    while(p<end && (reader.at(p)==' ' || reader.at(p)=='\t'))++p;
    return p;
}
inline Plan indent(const PieceTreeBuffer& buffer,const std::vector<std::size_t>& cursors,
                   const std::vector<std::optional<std::size_t>>& anchors,bool unindent,int width,const std::function<void()>& checkpoint={}) {
    Plan plan;
    for(auto block:LineOperations::ranges(buffer,cursors,anchors))for(auto row=block.first;row<=block.last;++row) {
        if(checkpoint)checkpoint();
        const auto start=LineOperations::contentStart(buffer,row);
        if(!unindent)plan.changes.push_back({start,{},std::string(width,' ')});
        else {
            std::size_t count=0;
            if(start<buffer.size() && buffer.byteAt(start)=='\t')count=1;
            else while(count<static_cast<std::size_t>(width) && start+count<buffer.lineEnd(row) && buffer.byteAt(start+count)==' ')++count;
            if(count)plan.changes.push_back({start,buffer.slice(start,count),{}});
        }
    }
    finish(plan,cursors,anchors,true);return plan;
}
inline Plan lineComments(const PieceTreeBuffer& buffer,const std::vector<std::size_t>& cursors,
                         const std::vector<std::optional<std::size_t>>& anchors,const CodeContext::Rules& rules,const std::function<void()>& checkpoint={}) {
    Plan plan;if(rules.line.empty())return plan;
    struct Row {std::size_t start;bool blank,comment;};std::vector<Row> rows;bool remove=true,any=false;
    for(auto block:LineOperations::ranges(buffer,cursors,anchors))for(auto row=block.first;row<=block.last;++row) {
        if(checkpoint)checkpoint();
        const auto start=indentationEnd(buffer,row);auto end=buffer.lineEnd(row);
        if(end>start && buffer.byteAt(end-1)=='\r')--end;
        const bool blank=start==end;
        const bool comment=!blank && buffer.slice(start,std::min(rules.line.size(),end-start))==rules.line;
        rows.push_back({start,blank,comment});if(!blank){any=true;remove=remove && comment;}
    }
    remove=remove && any;
    for(const auto row:rows) {
        if(checkpoint)checkpoint();
        if(row.blank && rows.size()>1)continue;
        if(remove) {
            auto length=rules.line.size();if(row.start+length<buffer.size() && buffer.byteAt(row.start+length)==' ')++length;
            plan.changes.push_back({row.start,buffer.slice(row.start,length),{}});
        } else plan.changes.push_back({row.start,{},rules.line+" "});
    }
    finish(plan,cursors,anchors);return plan;
}
inline Plan blockComments(const PieceTreeBuffer& buffer,const std::vector<std::size_t>& cursors,
                          const std::vector<std::optional<std::size_t>>& anchors,const CodeContext::Rules& rules,const std::function<void()>& checkpoint={}) {
    if(rules.open.empty())return lineComments(buffer,cursors,anchors,rules,checkpoint);
    Plan plan;std::vector<std::pair<std::size_t,std::size_t>> targets;
    for(std::size_t i=0;i<cursors.size();++i) {
        if(checkpoint)checkpoint();
        auto a=std::min(cursors[i],anchors[i].value_or(cursors[i])),b=std::max(cursors[i],anchors[i].value_or(cursors[i]));
        if(a==b){const auto row=buffer.lineIndexAt(a);a=indentationEnd(buffer,row);b=buffer.lineEnd(row);if(b>a && buffer.byteAt(b-1)=='\r')--b;}
        bool duplicate=false;
        for(const auto& target:targets) {
            if(target.first==a && target.second==b){duplicate=true;break;}
            if(a<target.second && b>target.first)throw std::runtime_error("Overlapping selections cannot be edited together.");
        }
        if(duplicate)continue;
        targets.push_back({a,b});
        const auto open=rules.open.size(),close=rules.close.size();
        if(b-a>=open+close && buffer.slice(a,open)==rules.open && buffer.slice(b-close,close)==rules.close) {
            plan.changes.push_back({a,rules.open,{}});plan.changes.push_back({b-close,rules.close,{}});
        } else if(a>=open && close<=buffer.size()-b && buffer.slice(a-open,open)==rules.open && buffer.slice(b,close)==rules.close) {
            plan.changes.push_back({a-open,rules.open,{}});plan.changes.push_back({b,rules.close,{}});
        } else {
            // C-style/HTML comments cannot safely contain a closing delimiter.
            CodeContext::Reader reader(buffer);
            for(auto p=a;p<b;++p) {
                if(checkpoint && p%4096==0)checkpoint();
                if(reader.matches(p,rules.close) || reader.matches(p,rules.open))throw std::runtime_error("The selection contains comment delimiters. Use line comments instead.");
            }
            if(a==b)plan.changes.push_back({a,{},rules.open+rules.close});
            else {plan.changes.push_back({a,{},rules.open});plan.changes.push_back({b,{},rules.close});}
        }
    }
    finish(plan,cursors,anchors);
    // Leave the inner selection active, allowing the same command to unwrap it.
    for(std::size_t i=0;i<cursors.size();++i)if(anchors[i] && *anchors[i]!=cursors[i]) {
        const auto high=std::max(cursors[i],*anchors[i]);
        if(cursors[i]==high)plan.cursors[i]=map(high,plan.changes,false);
        else plan.anchors[i]=map(high,plan.changes,false);
    }
    return plan;
}
}
