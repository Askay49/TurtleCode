#include "VimMotions.hpp"
#include "UnicodeNavigation.hpp"
#include "VimBlock.hpp"
#include <algorithm>
#include <QChar>
#include <optional>
#include <cctype>
#include <QTextBoundaryFinder>

namespace VimMotions {
using C=VimController::Command;
std::size_t lastLine(const PieceTreeBuffer& b,bool finalNewline) {return b.lineCount()-1-(finalNewline && b.size() && b.byteAt(b.size()-1)=='\n'?1:0);}
std::size_t lineStart(const PieceTreeBuffer& b,std::size_t line) {
    const auto p=b.lineStart(line);return !p && b.size()>=3 && b.slice(0,3)=="\xef\xbb\xbf"?3:p;
}
std::size_t lineEnd(const PieceTreeBuffer& b,std::size_t line) {
    auto p=b.lineEnd(line);if(p>b.lineStart(line) && p<b.size() && b.byteAt(p-1)=='\r')--p;return p;
}
std::size_t firstText(const PieceTreeBuffer& b,std::size_t line) {
    auto p=lineStart(b,line);const auto end=lineEnd(b,line);
    while(p<end) {
        const auto bytes=b.slice(p,std::min<std::size_t>(16384,end-p));
        for(char ch:bytes){if(ch!=' ' && ch!='\t')return p;++p;}
    }
    return p;
}
namespace {
// Sliding byte cache: word/object scans never materialize a full line/document.
class Scan {
    const PieceTreeBuffer& b;const std::function<void()>& checkpoint;
    std::string cache;std::size_t base{},ticks{};
public:
    Scan(const PieceTreeBuffer& buffer,const std::function<void()>& check):b(buffer),checkpoint(check){}
    char at(std::size_t p) {
        if((++ticks%4096)==0 && checkpoint)checkpoint();
        if(p>=b.size())return 0;
        if(cache.empty() || p<base || p>=base+cache.size()){base=p>4096?p-4096:0;cache=b.slice(base,std::min<std::size_t>(16384,b.size()-base));}
        return cache[p-base];
    }
    std::size_t next(std::size_t p) {if(p>=b.size())return b.size();++p;while(p<b.size() && (static_cast<unsigned char>(at(p))&0xc0)==0x80)++p;return p;}
    std::size_t prev(std::size_t p) {if(!p)return 0;--p;while(p && (static_cast<unsigned char>(at(p))&0xc0)==0x80)--p;return p;}
    int kind(std::size_t p,bool big=false) {
        if(p>=b.size())return 0;const auto first=static_cast<unsigned char>(at(p));char32_t ch=first;
        if(first>=0xc0){ch=first & (first<0xe0?31:first<0xf0?15:7);for(auto n=p+1;n<next(p);++n)ch=(ch<<6)|(static_cast<unsigned char>(at(n))&63);}
        if(QChar::isSpace(ch))return 0;if(big)return 1;
        const auto category=QChar::category(ch);
        return QChar::isLetterOrNumber(ch) || ch=='_' || category==QChar::Mark_NonSpacing || category==QChar::Mark_SpacingCombining || category==QChar::Mark_Enclosing?1:2;
    }
    bool escaped(std::size_t p) {int n=0;while(p && at(p-1)=='\\'){--p;++n;}return n%2;}
    bool skipQuote(std::size_t& p,bool forward) {
        const char quote=at(p);if((quote!='"' && quote!='\'' && quote!='`') || escaped(p))return false;
        const auto row=b.lineIndexAt(p),first=lineStart(b,row),last=lineEnd(b,row);auto q=p;
        while(forward?q<last:q>first){q=forward?next(q):prev(q);if(at(q)==quote && !escaped(q)){p=q;return true;}}
        return false;
    }
};
std::size_t grapheme(const PieceTreeBuffer& b,std::size_t p,bool forward) {
    const auto row=b.lineIndexAt(p),start=lineStart(b,row),end=lineEnd(b,row);
    if(forward && p>=end)return row+1<b.lineCount()?b.lineStart(row+1):b.size();
    if(!forward && p<=start)return row?lineEnd(b,row-1):start;
    const auto from=b.byteAtCodePoint(b.codePointsBefore(p>8192?std::max(start,p-8192):start));
    const auto last=std::min(end,b.byteAtCodePoint(b.codePointsBefore(std::min(end,p+8192))));
    const auto bytes=b.slice(from,last-from);
    return from+(forward?EditorUnicode::nextGrapheme(bytes,p-from):EditorUnicode::previousGrapheme(bytes,p-from));
}
bool blank(const PieceTreeBuffer& b,std::size_t row) {return b.lineStart(row)==lineEnd(b,row);}
std::size_t horizontal(const PieceTreeBuffer& b,std::size_t p,std::size_t count,bool forward) {
    const auto row=b.lineIndexAt(p),start=lineStart(b,row),end=lineEnd(b,row);
    while(count && (forward?p<end:p>start)) {
        const auto from=b.byteAtCodePoint(b.codePointsBefore(p>16384?std::max(start,p-16384):start));
        const auto last=std::min(end,b.byteAtCodePoint(b.codePointsBefore(std::min(end,p+16384))));
        const auto bytes=b.slice(from,last-from);const auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));
        std::vector<std::size_t> offsets(text.size()+1);std::size_t byte=0;
        for(qsizetype i=0;i<text.size();++i){offsets[i]=byte;const auto ch=text[i];if(ch.isHighSurrogate() && i+1<text.size()){offsets[++i]=byte;byte+=4;}else byte+=ch.unicode()<128?1:ch.unicode()<2048?2:3;}offsets.back()=byte;
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,text);
        finder.setPosition(int(std::lower_bound(offsets.begin(),offsets.end(),p-from)-offsets.begin()));
        const auto old=p;
        while(count){const int q=forward?finder.toNextBoundary():finder.toPreviousBoundary();if(q<0)break;p=from+offsets[std::size_t(q)];--count;
            // Leave overlap for the next bounded context window.
            if((forward && p+4096>=last && last<end) || (!forward && p<from+4096 && from>start))break;}
        if(p==old)break;
    }
    return p;
}
}
Range resolve(const PieceTreeBuffer& b,std::size_t cursor,const VimController::Action& a,const std::function<void()>& checkpoint,bool finalNewline) {
    const auto lastRow=lastLine(b,finalNewline);
    Scan s(b,checkpoint);Range r;r.target=cursor;const auto row=b.lineIndexAt(cursor),start=lineStart(b,row),end=lineEnd(b,row);
    const auto isBlank=[&](std::size_t line){s.at(b.lineStart(line));return blank(b,line);};
    auto p=cursor;bool inclusive=false;const bool op=a.operation!=C::Consume;const auto count=std::size_t(a.count);
    auto lines=[&](std::size_t first,std::size_t last) {
        r.begin=lineStart(b,first);r.end=last+1<b.lineCount()?b.lineStart(last+1):b.size();r.linewise=true;r.valid=true;r.target=r.begin;
    };
    auto wordEnd=[&](std::size_t at,bool big,bool advance) {
        auto q=advance?s.next(at):at;
        while(q<b.size() && !s.kind(q,big))q=s.next(q);
        if(q>=b.size())return b.size();const auto kind=s.kind(q,big);
        while(s.next(q)<b.size() && s.kind(s.next(q),big)==kind)q=s.next(q);return q;
    };
    switch(a.command) {
    case C::Line:lines(row,std::min(lastRow,row+count-1));return r;
    case C::Left:case C::Right:
        p=horizontal(b,p,count,a.command==C::Right);break;
    case C::Start:p=start;break;
    case C::FirstText:p=firstText(b,row);break;
    case C::End:p=lineEnd(b,std::min(lastRow,row+count-1));inclusive=p>lineStart(b,b.lineIndexAt(p));if(inclusive)p=grapheme(b,p,false);break;
    case C::Up:case C::Down:{const auto to=a.command==C::Up?(row>count?row-count:0):std::min(lastRow,row+count);if(op){lines(std::min(row,to),std::max(row,to));return r;}p=std::min(lineEnd(b,to),b.lineStart(to)+cursor-b.lineStart(row));break;}
    case C::FirstLine:case C::LastLine:{const auto to=a.command==C::LastLine && !a.explicitCount?lastRow:std::min(lastRow,count-1);if(op){lines(std::min(row,to),std::max(row,to));return r;}p=firstText(b,to);break;}
    case C::Word:case C::BigWord:
        if(a.operation==C::Change && s.kind(p)) {for(std::size_t n=0;n<count;++n)p=wordEnd(p,a.command==C::BigWord,n>0);inclusive=true;break;}
        for(std::size_t n=0;n<count && p<b.size();++n) {
            const auto kind=s.kind(p,a.command==C::BigWord);
            if(kind)while(p<b.size() && s.kind(p,a.command==C::BigWord)==kind)p=s.next(p);
            const auto oldLine=b.lineIndexAt(p);
            while(p<b.size() && !s.kind(p,a.command==C::BigWord)) {
                // Empty lines are stops for w/W.
                if(p==b.lineStart(b.lineIndexAt(p)) && b.lineIndexAt(p)>oldLine && isBlank(b.lineIndexAt(p)))break;
                p=s.next(p);
            }
        }
        if(op && count==1 && s.kind(cursor) && b.lineIndexAt(p)>row)p=end;
        break;
    case C::WordBack:case C::BigWordBack:
        for(std::size_t n=0;n<count && p>lineStart(b,0);++n){p=s.prev(p);while(p>lineStart(b,0) && !s.kind(p))p=s.prev(p);const auto kind=s.kind(p,a.command==C::BigWordBack);while(p>lineStart(b,0) && s.kind(s.prev(p),a.command==C::BigWordBack)==kind)p=s.prev(p);}break;
    case C::WordEnd:case C::BigWordEnd:
        for(std::size_t n=0;n<count;++n)p=wordEnd(p,a.command==C::BigWordEnd,true);inclusive=true;break;
    case C::WordEndBack:case C::BigWordEndBack:
        for(std::size_t n=0;n<count && p>lineStart(b,0);++n){
            const bool big=a.command==C::BigWordEndBack;auto q=s.prev(p);
            if(s.kind(p,big) && s.kind(q,big)==s.kind(p,big))while(q>lineStart(b,0) && s.kind(q,big)==s.kind(p,big))q=s.prev(q);
            while(q>lineStart(b,0) && !s.kind(q,big)){const auto line=b.lineIndexAt(q);if(q==lineStart(b,line) && isBlank(line))break;q=s.prev(q);}p=q;
        }if(p==cursor)return r;inclusive=true;break;
    case C::LineNext:case C::LineBack:case C::LineOffset:{
        const auto amount=a.command==C::LineOffset?count-1:count;
        const auto to=a.command==C::LineBack?(row>amount?row-amount:0):std::min(lastRow,row+amount);
        if(op){lines(std::min(row,to),std::max(row,to));return r;}p=firstText(b,to);break;
    }
    case C::Column:p=VimBlock::slice(b,row,int(count-1),int(count-1),a.tabWidth,checkpoint,[](std::string_view){}).begin;break;
    case C::LastText:{const auto to=std::min(lastRow,row+count-1);p=lineEnd(b,to);while(p>lineStart(b,to) && (s.at(s.prev(p))==' ' || s.at(s.prev(p))=='\t'))p=s.prev(p);if(p>lineStart(b,to))p=grapheme(b,p,false);inclusive=true;break;}
    case C::SentenceNext:case C::SentenceBack:{
        const auto first=lineStart(b,0);const bool forward=a.command==C::SentenceNext;
        const auto starts=[&](std::size_t at){
            const auto line=b.lineIndexAt(at),begin=lineStart(b,line);
            if(at==first)return true;
            if(at==begin && ((isBlank(line) && (!line || !isBlank(line-1))) || (!isBlank(line) && line && isBlank(line-1)) || s.at(at)=='\f'))return true;
            if(!s.kind(at))return false;
            auto q=s.prev(at);if(s.kind(q))return false;
            while(q>first && !s.kind(q)){if(s.at(q)=='\n' && q && b.lineIndexAt(q)>0 && isBlank(b.lineIndexAt(q)))break;q=s.prev(q);}
            while(q>first && (s.at(q)==')' || s.at(q)==']' || s.at(q)=='"' || s.at(q)=='\''))q=s.prev(q);
            return s.at(q)=='.' || s.at(q)=='!' || s.at(q)=='?';
        };
        for(std::size_t n=0;n<count;++n){
            if(forward){if(p>=b.size())break;p=s.next(p);while(p<b.size() && !starts(p))p=s.next(p);}
            else {if(p<=first)break;p=s.prev(p);while(p>first && !starts(p))p=s.prev(p);}
        }break;
    }
    case C::SectionNext:case C::SectionBack:{
        const bool forward=a.command==C::SectionNext,closing=(forward && a.character=='[') || (!forward && a.character==']');auto line=row;
        const auto boundary=[&](std::size_t at){
            const auto offset=lineStart(b,at);const auto c=s.at(offset);if(c==(closing?'}':'{') || c=='\f')return true;
            if(c=='.'){const auto macro=b.slice(offset+1,std::min<std::size_t>(2,lineEnd(b,at)>offset?lineEnd(b,at)-offset-1:0));return macro=="SH" || macro=="NH" || macro=="H " || macro=="H" || macro=="HU" || macro=="nh" || macro=="sh";}return false;
        };
        for(std::size_t n=0;n<count;++n){
            bool found=false;while(forward?line<lastRow:line>0){line=forward?line+1:line-1;if(checkpoint)checkpoint();
                if(forward && !closing && op && s.at(lineStart(b,line))=='}'){line=std::min(lastRow,line+1);found=true;break;}
                if(boundary(line)){found=true;break;}}
            if(!found){p=forward?lineEnd(b,lastRow):lineStart(b,0);break;}p=lineStart(b,line);
        }break;
    }
    case C::UnclosedNext:case C::UnclosedBack:{
        const bool forward=a.command==C::UnclosedNext;const char selected=a.character.toLatin1(),other=selected=='('?')':selected=='{'?'}':selected==')'?'(':'{';
        for(std::size_t n=0;n<count;++n){int depth=0;bool found=false;while(forward?p<b.size():p>lineStart(b,0)){p=forward?s.next(p):s.prev(p);if(s.skipQuote(p,forward))continue;if(s.at(p)==other)++depth;else if(s.at(p)==selected){if(!depth){found=true;break;}--depth;}}if(!found)return r;}break;
    }
    case C::ParagraphBack:case C::ParagraphNext:{
        auto line=row;const bool forward=a.command==C::ParagraphNext;
        const auto canStep=[&]{return forward?line<lastRow:line>0;};
        const auto step=[&]{line=forward?line+1:line-1;};
        for(std::size_t n=0;n<count;++n){
            while(isBlank(line) && canStep())step();
            if(canStep())step();
            while(!isBlank(line) && canStep())step();
            if(checkpoint)checkpoint();
        }
        p=forward && line==lastRow && !isBlank(line)?b.size():lineStart(b,line);break;
    }
    case C::Find:case C::FindBack:case C::Till:case C::TillBack:{const bool forward=a.command==C::Find || a.command==C::Till;const auto bytes=QString(a.character).toUtf8().toStdString();if(bytes.empty())return r;for(std::size_t n=0;n<count;++n){bool found=false;while(forward?p<end:p>start){p=forward?s.next(p):s.prev(p);if(p<end && s.at(p)==bytes[0] && b.slice(p,bytes.size())==bytes){if(a.repeatFind && n==0 && ((a.command==C::Till && grapheme(b,p,false)==cursor) || (a.command==C::TillBack && grapheme(b,p,true)==cursor)))continue;found=true;break;}}if(!found)return r;}if(a.command==C::Till)p=grapheme(b,p,false);if(a.command==C::TillBack)p=grapheme(b,p,true);inclusive=forward;break;}
    case C::Match:{
        if(a.explicitCount){if(count>100)return r;const auto to=(count*(lastRow+1)+99)/100-1;if(op){lines(std::min(row,to),std::max(row,to));return r;}p=firstText(b,to);break;}
        std::size_t q=p;const std::string opens="([{",closes=")]}",both="()[]{}";
        while(q<end && both.find(s.at(q))==std::string::npos)q=s.next(q);if(q>=end)return r;
        const char ch=s.at(q);const auto index=opens.find(ch);const bool forward=index!=std::string::npos;const char opposite=forward?closes[index]:opens[closes.find(ch)];int depth=1;
        while(forward?q<b.size():q>lineStart(b,0)){q=forward?s.next(q):s.prev(q);if(s.skipQuote(q,forward))continue;if(s.at(q)==ch)++depth;else if(s.at(q)==opposite && --depth==0){p=q;break;}}
        if(depth)return r;inclusive=true;break;}
    case C::Object:{r.object=true;const char object=a.character.toLatin1();
        if(object=='s') {
            const auto bounds=[&](std::size_t at) {
                VimController::Action motion;motion.command=C::SentenceBack;
                auto begin=resolve(b,std::min(b.size(),s.next(at)),motion,checkpoint,finalNewline).target;
                motion.command=C::SentenceNext;
                auto end=resolve(b,begin,motion,checkpoint,finalNewline).target;
                if(end<=begin)end=b.size();
                while(end>begin && !s.kind(s.prev(end)))end=s.prev(end);
                return std::pair{begin,end};
            };
            const auto whitespace=[&](std::size_t q) {
                const auto initialRow=b.lineIndexAt(q);const bool blank=isBlank(initialRow);
                while(q<b.size() && !s.kind(q)){
                    q=s.next(q);
                    if(!blank && q<b.size() && q==lineStart(b,b.lineIndexAt(q)) && isBlank(b.lineIndexAt(q)))break;
                }
                return q;
            };
            auto [begin,last]=bounds(p);
            const bool white=!s.kind(p);
            if(white) {
                const bool blank=isBlank(b.lineIndexAt(p));
                begin=p;while(begin>lineStart(b,b.lineIndexAt(p)) && !s.kind(s.prev(begin)))begin=s.prev(begin);
                last=whitespace(p);
                if(a.around && !blank && last<b.size() && s.kind(last))last=bounds(last).second;
            } else if(a.around) {
                const auto tail=whitespace(last);
                if(tail>last)last=tail;else while(begin>lineStart(b,b.lineIndexAt(begin)) && !s.kind(s.prev(begin)))begin=s.prev(begin);
            }
            for(std::size_t n=1;n<count && last<b.size();++n) {
                if(!s.kind(last)) {
                    const bool blank=isBlank(b.lineIndexAt(last));last=whitespace(last);
                    if(a.around && !blank && last<b.size() && s.kind(last))last=bounds(last).second;
                }else last=bounds(last).second;
                if(a.around && last<b.size() && !s.kind(last) && !isBlank(b.lineIndexAt(last)))last=whitespace(last);
            }
            // A change keeps the last line break, as other characterwise c motions do.
            if(a.operation==C::Change && last>begin && s.at(last-1)=='\n'){--last;if(last>begin && s.at(last-1)=='\r')--last;}
            r.begin=begin;r.end=last;r.target=begin;r.valid=last>begin;return r;
        }
        if(object=='t') {
            struct Tag {std::string name;std::size_t begin,end;};
            struct Pair {std::size_t begin,inside,end,after;};
            std::vector<Tag> stack;std::vector<Pair> pairs;
            for(std::size_t q=lineStart(b,0);q<b.size();) {
                if(s.at(q)!='<'){q=s.next(q);continue;}
                const auto first=q++;bool closing=s.at(q)=='/';if(closing)++q;
                if(s.at(q)=='!' || s.at(q)=='?'){while(q<b.size() && s.at(q)!='>')++q;if(q<b.size())++q;continue;}
                std::string name;while(q<b.size() && (std::isalnum(static_cast<unsigned char>(s.at(q))) || s.at(q)=='_' || s.at(q)=='-' || s.at(q)==':')){name+=char(std::tolower(static_cast<unsigned char>(s.at(q++))));if(name.size()>4096)break;}
                if(name.empty())continue;
                // Vim's tag text object terminates a tag at the first '>'.
                bool self=false;while(q<b.size()){if(s.at(q++)=='>'){self=q>1 && s.at(q-2)=='/';break;}}
                if(closing){
                    for(auto i=stack.size();i>0;--i)if(stack[i-1].name==name){const auto open=stack[i-1];if(open.begin<=p && p<q)pairs.push_back({open.begin,open.end,first,q});stack.resize(i-1);break;}
                }else if(!self){if(stack.size()>=100000)return r;stack.push_back({name,first,q});}
            }
            std::sort(pairs.begin(),pairs.end(),[](const auto& x,const auto& y){return x.after-x.begin<y.after-y.begin;});
            if(std::size_t(a.count)>pairs.size())return r;
            const auto pair=pairs[std::size_t(a.count)-1];r.begin=a.around?pair.begin:pair.inside;r.end=a.around?pair.after:pair.end;if(!a.around && pair.inside==pair.end && a.operation==C::Consume){r.begin=pair.begin;r.end=pair.inside;}r.target=r.begin;r.valid=true;return r;
        }
        if(object=='w' || object=='W') {
            if(p>=b.size())return r;
            const bool big=object=='W';auto begin=p;const auto kind=s.kind(p,big);
            while(begin>start && s.kind(s.prev(begin),big)==kind)begin=s.prev(begin);
            auto last=p;
            if(a.around && !kind)while(last<b.size() && !s.kind(last,big))last=s.next(last);
            for(std::size_t n=0;n<count;++n) {
                const auto k=s.kind(last,big);
                while(last<b.size() && s.kind(last,big)==k)last=s.next(last);
                if(a.around && n+1<count)while(last<b.size() && !s.kind(last,big))last=s.next(last);
            }
            if(a.around && kind) {
                auto tail=last;while(tail<lineEnd(b,b.lineIndexAt(last)) && !s.kind(tail))tail=s.next(tail);
                if(tail>last)last=tail;else while(begin>start && !s.kind(s.prev(begin)))begin=s.prev(begin);
            }
            r.begin=begin;r.end=last;r.target=begin;r.valid=true;return r;
        }
        if(object=='p') {
            auto first=row,last=row;const bool empty=isBlank(row);
            while(first && isBlank(first-1)==empty)--first;
            while(last<lastRow && isBlank(last+1)==empty)++last;
            for(std::size_t n=1;n<count;++n){while(last<lastRow && isBlank(last+1))++last;while(last<lastRow && !isBlank(last+1))++last;}
            if(a.around){const auto old=last;while(last<lastRow && isBlank(last+1))++last;if(empty)while(last<lastRow && !isBlank(last+1))++last;else if(old==last)while(first && isBlank(first-1))--first;}
            lines(first,last);r.object=true;return r;
        }
        if(object=='"' || object=='\'' || object=='`') {
            std::optional<std::size_t> open;
            for(auto q=start;q<end;q=s.next(q))if(s.at(q)==object && !s.escaped(q)) {
                if(!open)open=q;else {if(q>=cursor){r.begin=*open+(a.around?0:1);r.end=q+(a.around?1:0);r.target=r.begin;r.valid=true;return r;}open.reset();}
            }return r;
        }
        const std::string opens="([{<",closes=")]}>";const char alias=object=='b'?'(':object=='B'?'{':object;auto index=opens.find(alias);if(index==std::string::npos)index=closes.find(alias);if(index==std::string::npos)return r;
        const char open=opens[index],close=closes[index];auto search=p;std::size_t begin=0,last=0;
        for(std::size_t n=0;n<count;++n) {
            int depth=0;bool found=false;auto q=search;
            while(true){if(s.skipQuote(q,false)){if(q<=lineStart(b,0))break;q=s.prev(q);continue;}const auto ch=s.at(q);if(ch==open){if(!depth){begin=q;found=true;break;}--depth;}else if(ch==close && q!=p)++depth;if(q<=lineStart(b,0))break;q=s.prev(q);}
            if(!found)return r;depth=1;last=s.next(begin);
            while(last<b.size()){if(s.skipQuote(last,true)){last=s.next(last);continue;}if(s.at(last)==open)++depth;else if(s.at(last)==close && --depth==0)break;last=s.next(last);}if(depth)return r;
            if(n+1<count){if(!begin)return r;search=s.prev(begin);}
        }
        r.begin=begin+(a.around?0:1);r.end=last+(a.around?1:0);
        if(a.operation==C::Change && !a.around && b.lineIndexAt(begin)<b.lineIndexAt(last) && r.begin==lineEnd(b,b.lineIndexAt(begin)) && firstText(b,b.lineIndexAt(last))==last) {
            r.begin=b.lineStart(b.lineIndexAt(begin)+1);r.end=b.lineStart(b.lineIndexAt(last));r.linewise=true;
        }
        r.target=r.begin;r.valid=true;return r;
    }
    default:return r;
    }
    if((a.command==C::WordEndBack || a.command==C::BigWordEndBack) && p<b.size())p=grapheme(b,grapheme(b,p,true),false);
    r.target=p;r.valid=true;
    if(op) {
        r.begin=std::min(cursor,p);r.end=std::max(cursor,p);
        if(inclusive)r.end=grapheme(b,r.end,true);
        else if(p>cursor && b.lineIndexAt(p)>row && p==lineStart(b,b.lineIndexAt(p))) {
            if(cursor<=firstText(b,row)){lines(row,b.lineIndexAt(p)-1);return r;}
            r.end=lineEnd(b,b.lineIndexAt(p)-1);
        }
    }
    return r;
}
Range resolveSelection(const PieceTreeBuffer& buffer,std::size_t cursor,const VimController::Action& action,
                       std::optional<std::pair<std::size_t,std::size_t>> selection,const std::function<void()>& checkpoint,bool finalNewline) {
    auto range=resolve(buffer,cursor,action,checkpoint,finalNewline);
    if(action.command!=C::Object || !selection || !range.valid)return range;
    const auto [first,last]=*selection;
    if(QStringLiteral("wWs").contains(action.character) || (range.begin>=first && range.end<=last)) {
        auto extended=action;const auto object=action.character;
        if(QStringLiteral("wWsp").contains(object)) {
            auto focus=last;
            if(cursor<last-1 && cursor<=first){focus=grapheme(buffer,first,false);if(action.around){Scan scan(buffer,checkpoint);while(focus && !scan.kind(focus))focus=scan.prev(focus);}}
            range=resolve(buffer,focus,extended,checkpoint,finalNewline);
        }
        else {if(object=='t' && !action.around)extended.around=true;else ++extended.count;range=resolve(buffer,first,extended,checkpoint,finalNewline);}
    }
    if(range.valid && action.character=='t' && range.begin>=first && range.end<=last) {
        auto extended=action;++extended.count;range=resolve(buffer,first,extended,checkpoint,finalNewline);
    }
    if(range.valid && QStringLiteral("wWsp").contains(action.character)){range.begin=std::min(range.begin,first);range.end=std::max(range.end,last);}
    return range;
}

}
