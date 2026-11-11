#pragma once
#include "core/text/PieceTreeBuffer.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A bounded lexical fallback for editing, independent of installed grammars.
// Checkpoints allow typing near the end of a large file without rescanning it.
namespace CodeContext {
struct Rules {
    std::string line{"//"},open{"/*"},close{"*/"};
    bool rawCpp{true},triple{false},backtick{false};
    bool operator==(const Rules&) const = default;
};
enum class Mode {Code,LineComment,BlockComment,Single,Double,Backtick,TripleSingle,TripleDouble,Raw};
struct State {Mode mode{Mode::Code};bool escaped{false};std::string rawEnd;};
class Reader {
    const PieceTreeBuffer& buffer;
    std::size_t begin{static_cast<std::size_t>(-1)};
    std::string bytes;
public:
    explicit Reader(const PieceTreeBuffer& source):buffer(source){}
    char at(std::size_t pos) {
        if(pos>=buffer.size())return '\0';
        if(begin==static_cast<std::size_t>(-1) || pos<begin || pos>=begin+bytes.size()) {
            begin=pos;bytes=buffer.slice(pos,std::min<std::size_t>(4096,buffer.size()-pos));
        }
        return bytes[pos-begin];
    }
    bool matches(std::size_t pos,std::string_view token) {
        if(token.empty() || pos>buffer.size() || token.size()>buffer.size()-pos)return false;
        for(std::size_t i=0;i<token.size();++i)if(at(pos+i)!=token[i])return false;
        return true;
    }
};
inline bool word(char c) {return static_cast<unsigned char>(c)>=128 || (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_';}
inline std::size_t advance(Reader& r,std::size_t p,State& s,const Rules& rules) {
    const char c=r.at(p);
    switch(s.mode) {
    case Mode::LineComment:
        if(c=='\n' && !s.escaped)s.mode=Mode::Code;
        s.escaped=rules.rawCpp && (c=='\\' || (c=='\r' && s.escaped));return p+1;
    case Mode::BlockComment:
        if(r.matches(p,rules.close)){s.mode=Mode::Code;return p+rules.close.size();}return p+1;
    case Mode::Raw:
        if(r.matches(p,s.rawEnd)){const auto n=s.rawEnd.size();s={};return p+n;}return p+1;
    case Mode::Single:case Mode::Double:case Mode::Backtick:case Mode::TripleSingle:case Mode::TripleDouble: {
        if(s.escaped){s.escaped=false;return p+(c=='\r' && r.at(p+1)=='\n'?2:1);}
        if(c=='\\'){s.escaped=true;return p+1;}
        const bool triple=s.mode==Mode::TripleSingle || s.mode==Mode::TripleDouble;
        const char quote=s.mode==Mode::Single || s.mode==Mode::TripleSingle?'\'':s.mode==Mode::Backtick?'`':'"';
        const std::string end(triple?3:1,quote);
        if(r.matches(p,end)){s={};return p+end.size();}
        if(c=='\n' && !triple && s.mode!=Mode::Backtick)s={};
        return p+1;
    }
    case Mode::Code:break;
    }
    if(r.matches(p,rules.open)){s.mode=Mode::BlockComment;return p+rules.open.size();}
    if(r.matches(p,rules.line)){s.mode=Mode::LineComment;return p+rules.line.size();}
    if(rules.rawCpp && r.matches(p,"R\"")) {
        std::string delimiter;
        for(std::size_t i=p+2;i<p+19;++i) {
            const char d=r.at(i);
            if(d=='('){s.mode=Mode::Raw;s.rawEnd=")"+delimiter+"\"";return i+1;}
            if(d=='\0' || d==' ' || d=='\\' || d==')' || d=='\n' || d=='\r' || d=='\t')break;
            delimiter+=d;
        }
    }
    if(c=='\'' && rules.rawCpp && p && word(r.at(p-1)) && word(r.at(p+1)))return p+1; // digit separator
    if(c=='\'' || c=='"' || (c=='`' && rules.backtick)) {
        const bool triple=rules.triple && r.matches(p,std::string(3,c));
        s.mode=triple?(c=='\''?Mode::TripleSingle:Mode::TripleDouble):c=='\''?Mode::Single:c=='"'?Mode::Double:Mode::Backtick;
        return p+(triple?3:1);
    }
    return p+1;
}
class Cache {
    std::map<std::size_t,State> checkpoints{{0,{}}};
    Rules previous;
public:
    void clear(){checkpoints={{0,{}}};}
    void invalidate(std::size_t offset) {
        // A token opener may have looked ahead across the edit boundary.
        const auto safe=offset>32?offset-32:0;
        checkpoints.erase(checkpoints.upper_bound(safe),checkpoints.end());
        if(!safe)clear();
    }
    std::optional<State> at(const PieceTreeBuffer& buffer,std::size_t target,const Rules& rules,std::size_t budget=128*1024) {
        if(rules!=previous){clear();previous=rules;}
        target=std::min(target,buffer.size());
        auto it=checkpoints.upper_bound(target);--it;
        auto pos=it->first;State state=it->second;Reader reader(buffer);
        auto nextCheckpoint=pos+4096;const auto limit=pos+budget;
        while(pos<target && pos<limit) {
            const auto next=advance(reader,pos,state,rules);
            // At a position inside a multi-byte delimiter, return its entered context.
            if(next>target)return state;
            pos=next;
            if(pos>=nextCheckpoint){checkpoints[pos]=state;nextCheckpoint=pos+4096;}
        }
        if(pos==target)checkpoints[pos]=state;
        while(checkpoints.size()>4096)checkpoints.erase(std::next(checkpoints.begin()));
        if(pos!=target)return {};
        return state;
    }
};
inline char closeFor(char c) {switch(c){case '(':return ')';case '[':return ']';case '{':return '}';case '\'':return '\'';case '"':return '"';default:return 0;}}
inline bool bracket(char c){return c=='(' || c=='[' || c=='{' || c==')' || c==']' || c=='}';}
struct Match {std::optional<std::size_t> first,second;bool unmatched{false};bool pending{false};};
inline Match match(const PieceTreeBuffer& buffer,std::size_t cursor,Cache& cache,const Rules& rules) {
    Reader reader(buffer);std::optional<std::size_t> candidate;
    if(cursor<buffer.size() && bracket(reader.at(cursor)))candidate=cursor;
    else if(cursor && bracket(reader.at(cursor-1)))candidate=cursor-1;
    if(!candidate)return {};
    const auto context=cache.at(buffer,*candidate,rules);
    if(!context)return {{},{},false,true};
    if(context->mode!=Mode::Code)return {};
    const char token=reader.at(*candidate);const bool forward=closeFor(token)!=0;
    const auto begin=forward?*candidate:(*candidate>128*1024?*candidate-128*1024:0);
    const auto end=forward?std::min(buffer.size(),*candidate+128*1024):*candidate+1;
    auto state=cache.at(buffer,begin,rules);
    if(!state)return {{},{},false,true};
    std::vector<std::pair<char,std::size_t>> stack;
    for(auto pos=begin;pos<end;) {
        const auto c=reader.at(pos);
        if(state->mode==Mode::Code && bracket(c)) {
            if(closeFor(c))stack.push_back({c,pos});
            else {
                if(stack.empty()) {
                    if(pos==*candidate)return {candidate,{},begin==0,false};
                } else if(closeFor(stack.back().first)!=c) {
                    if(forward || pos==*candidate)return {candidate,{},true,false};
                    stack.clear();
                } else {
                    const auto opening=stack.back().second;stack.pop_back();
                    if((forward && opening==*candidate) || (!forward && pos==*candidate))return {candidate,forward?pos:opening,false,false};
                }
            }
        }
        pos=advance(reader,pos,*state,rules);
    }
    return {candidate,{},forward && end==buffer.size(),false};
}
}
