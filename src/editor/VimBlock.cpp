#include "VimBlock.hpp"
#include <QTextBoundaryFinder>
#include <QString>
#include <QVector>
#include <algorithm>

namespace VimBlock {
int width(std::string_view bytes,int column,int tabWidth) {
    if(bytes=="\t")return tabWidth-column%tabWidth;
    if(bytes.size()==1)return 1;
    const auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size())).toUcs4();
    for(char32_t ch:text)if(ch>=0x1100 && (ch<=0x115f || (ch>=0x2329 && ch<=0x232a) || (ch>=0x2e80 && ch<=0xa4cf) || (ch>=0xac00 && ch<=0xd7a3) || (ch>=0xf900 && ch<=0xfaff) || (ch>=0xfe10 && ch<=0xfe6f) || (ch>=0xff00 && ch<=0xff60) || (ch>=0xffe0 && ch<=0xffe6) || (ch>=0x1f000 && ch<=0x1faff) || ch>=0x20000))return 2;
    return 1;
}
namespace {
template<class Visitor> void cells(const PieceTreeBuffer& buffer,std::size_t row,int tabWidth,Visitor visit,const std::function<void()>& checkpoint={},int initialColumn=0) {
    auto from=VimMotions::lineStart(buffer,row);const auto end=VimMotions::lineEnd(buffer,row);int column=initialColumn;
    while(from<end) {
        if(checkpoint)checkpoint();
        const auto limit=std::min(end,buffer.byteAtCodePoint(buffer.codePointsBefore(std::min(end,from+32768))));
        const auto bytes=buffer.slice(from,limit-from);const auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));
        std::vector<std::size_t> offsets(text.size()+1);std::size_t at=0;
        for(qsizetype i=0;i<text.size();++i){offsets[i]=at;const auto ch=text[i];if(ch.isHighSurrogate() && i+1<text.size()){offsets[++i]=at;at+=4;}else at+=ch.unicode()<128?1:ch.unicode()<2048?2:3;}offsets.back()=at;
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,text);int first=0,last=0;std::size_t consumed=0;
        while((last=finder.toNextBoundary())>=0) {
            const auto a=offsets[std::size_t(first)],b=offsets[std::size_t(last)];
            if(limit<end && b+4096>=bytes.size() && consumed)break;
            const auto value=std::string_view(bytes).substr(a,b-a);const int size=width(value,column,tabWidth);
            if(!visit(from+a,from+b,column,size,value))return;
            column+=size;consumed=b;first=last;
        }
        if(!consumed)return;from+=consumed;
    }
    visit(end,end,column,0,std::string_view{});
}
}
void forEachCell(const PieceTreeBuffer& b,std::size_t row,int tabWidth,const std::function<bool(std::size_t,std::size_t,int,int,std::string_view)>& visitor,const std::function<void()>& checkpoint,int initialColumn){cells(b,row,tabWidth,visitor,checkpoint,initialColumn);}
Slice slice(const PieceTreeBuffer& b,std::size_t row,int left,int right,int tabWidth,const std::function<void()>& checkpoint,const std::function<void(std::string_view)>& sink) {
    Slice result;const auto end=VimMotions::lineEnd(b,row);result.begin=result.end=end;
    bool started=false;
    cells(b,row,tabWidth,[&](std::size_t a,std::size_t last,int col,int size,std::string_view value) {
        result.column=col+size;
        if(!size){if(!started)result.gap=std::max(0,left-col);return false;}
        if(col+size<=left)return true;
        if(right==left) {
            result.begin=result.end=a;
            if(col<left && value=="\t"){result.end=last;result.before=std::string(left-col,' ');result.after=std::string(col+size-left,' ');}
            return false;
        }
        if(col>=right)return false;
        if(!started){result.begin=a;started=true;if(col<left && value=="\t")result.before=std::string(left-col,' ');}
        result.end=last;
        if(value=="\t"){const std::string spaces=col>=left && col+size<=right?"\t":std::string(std::max(0,std::min(right,col+size)-std::max(left,col)),' ');if(sink)sink(spaces);else result.text+=spaces;if(col+size>right)result.after=std::string(col+size-right,' ');}
        else if(sink)sink(value);else result.text+=value;
        return col+size<right;
    },checkpoint);
    return result;
}
int column(const PieceTreeBuffer& b,std::size_t row,std::size_t position,int tabWidth) {
    const auto first=VimMotions::lineStart(b,row);position=std::min(position,VimMotions::lineEnd(b,row));
    if(b.codePointsBefore(position)-b.codePointsBefore(first)==position-first) {
        int col=0;for(auto p=first;p<position;p+=64*1024){const auto bytes=b.slice(p,std::min<std::size_t>(64*1024,position-p));std::size_t from=0;
            for(auto at=bytes.find('\t');at!=std::string::npos;at=bytes.find('\t',from)){col+=int(at-from);col+=tabWidth-col%tabWidth;from=at+1;}col+=int(bytes.size()-from);}
        return col;
    }
    int result=0;cells(b,row,tabWidth,[&](std::size_t a,std::size_t,int col,int size,std::string_view){result=col;if(a>=position)return false;result=col+size;return true;});return result;
}
std::size_t advance(const PieceTreeBuffer& b,std::size_t p,int count,const std::function<void()>& checkpoint) {
    const bool forward=count>=0;auto remaining=std::size_t(forward?count:-count);
    while(remaining) {
        if(checkpoint)checkpoint();const auto row=b.lineIndexAt(p),first=VimMotions::lineStart(b,row),last=VimMotions::lineEnd(b,row);
        VimController::Action motion;motion.command=forward?VimController::Command::Right:VimController::Command::Left;motion.count=int(remaining);
        const auto target=VimMotions::resolve(b,p,motion,checkpoint).target;
        if(forward?target<last:target>first)return target;
        std::size_t used=0;cells(b,row,4,[&](std::size_t begin,std::size_t end,int,int size,std::string_view){if(!size)return false;if(forward?begin>=p:end<=p)++used;return true;},checkpoint);
        remaining-=std::min(remaining,used);p=target;if(!remaining)break;
        if(forward){if(row+1==b.lineCount())break;p=b.lineStart(row+1);}else {if(!row)break;p=VimMotions::lineEnd(b,row-1);}--remaining;
    }
    return p;
}
}
