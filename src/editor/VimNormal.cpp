#include "VimNormal.hpp"
#include "VimBlock.hpp"
#include "CodeContext.hpp"
#include <QChar>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <limits>

namespace VimNormal {
namespace {
void check(const Check& step){if(step)step();}
void copy(ChunkedFile& output,const PieceTreeBuffer& source,std::size_t begin,std::size_t end,const Check& step){for(auto at=begin;at<end;at+=65536){check(step);output.append(source.slice(at,std::min<std::size_t>(65536,end-at)));}}
struct Builder {
    Plan plan;std::string records,text;std::size_t size{};
    void add(std::size_t begin,std::size_t end,std::string_view input){
        VimSearch::Edit edit{begin,end,size,input.size()};records.append(reinterpret_cast<const char*>(&edit),sizeof edit);text+=input;size+=input.size();++plan.edits->count;
        if(records.size()>=65536){plan.edits->records->append(records);records.clear();}if(text.size()>=65536){plan.edits->text->append(text);text.clear();}
    }
    void addFile(std::size_t begin,std::size_t end,const ChunkedFile& input,const Check& step){
        flush();VimSearch::Edit edit{begin,end,size,input.size()};records.append(reinterpret_cast<const char*>(&edit),sizeof edit);++plan.edits->count;
        for(std::size_t p=0;p<input.size();p+=65536){check(step);plan.edits->text->append(input.read(p,std::min<std::size_t>(65536,input.size()-p)));}size+=input.size();
    }
    void flush(){if(!records.empty()){plan.edits->records->append(records);records.clear();}if(!text.empty()){plan.edits->text->append(text);text.clear();}}
    Plan finish(std::size_t cursor){flush();plan.cursor=cursor;plan.edits->records->dropCache();plan.edits->text->dropCache();return std::move(plan);}
};
std::size_t map(const Plan& plan,std::size_t position){std::ptrdiff_t delta=0;for(std::size_t n=0;n<plan.edits->count;++n){const auto edit=plan.edits->at(n);if(position<edit.begin)break;if(position<edit.end)return std::size_t(std::ptrdiff_t(edit.begin)+delta)+std::min<std::size_t>(position-edit.begin,edit.length);delta+=std::ptrdiff_t(edit.length)-std::ptrdiff_t(edit.end-edit.begin);}return std::size_t(std::ptrdiff_t(position)+delta);}
std::pair<char32_t,std::size_t> character(CodeContext::Reader& reader,std::size_t at,std::size_t end){
    const auto c=static_cast<unsigned char>(reader.at(at));unsigned size=c<0x80?1:c>=0xc2 && c<=0xdf?2:c>=0xe0 && c<=0xef?3:c>=0xf0 && c<=0xf4?4:1;char32_t value=size==1?c:c&(size==2?31:size==3?15:7);
    if(at+size>end)return {0xfffd,1};for(unsigned n=1;n<size;++n){const auto next=static_cast<unsigned char>(reader.at(at+n));if((next&0xc0)!=0x80)return {0xfffd,1};value=(value<<6)|(next&63);}return {value,size};
}
std::string indentation(int columns,const Options& options){columns=std::max(0,columns);return options.tabs?std::string(std::size_t(columns/options.tabWidth),'\t')+std::string(std::size_t(columns%options.tabWidth),' '):std::string(std::size_t(columns),' ');}
int displayWidth(std::string_view bytes,int column,int tabWidth){
    const int initial=column;bool ascii=true;for(const auto c:bytes)if(static_cast<unsigned char>(c)>=128){ascii=false;break;}
    if(ascii){for(const auto c:bytes)column+=c=='\t'?tabWidth-column%tabWidth:1;return column-initial;}
    const auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,text);int begin=0;
    while(const auto end=finder.toNextBoundary()){if(end<0)break;const auto cluster=text.mid(begin,end-begin).toUtf8();column+=VimBlock::width({cluster.data(),std::size_t(cluster.size())},column,tabWidth);begin=end;}return column-initial;
}
bool blank(char c){return c==' ' || c=='\t' || c=='\r' || c=='\n';}
int digit(char c){return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:-1;}
}
std::size_t Join::project(std::size_t position) const{
    if(position<begin)return position;if(position>=end)return position-(end-begin)+text->size();
    for(const auto& part:parts)if(position<=part.end)return part.destination+(position>part.text?position-part.text:0);return begin+text->size();
}
Join join(const PieceTreeBuffer& buffer,std::size_t first,std::size_t last,bool raw,bool spaces,const Check& step){
    Join result;const auto begin=VimMotions::lineStart(buffer,first);result.begin=begin;result.end=VimMotions::lineEnd(buffer,last);result.parts.push_back({begin,begin,VimMotions::lineEnd(buffer,first),begin});copy(*result.text,buffer,begin,VimMotions::lineEnd(buffer,first),step);
    for(auto row=first+1;row<=last;++row){check(step);auto start=raw?VimMotions::lineStart(buffer,row):VimMotions::firstText(buffer,row);const auto end=VimMotions::lineEnd(buffer,row);result.cursor=begin+result.text->size();
        if(!raw && start<end && result.text->size()){
            const auto previous=result.text->read(result.text->size()-1,1)[0],next=buffer.byteAt(start);
            if(previous!=' ' && previous!='\t' && next!=')'){result.text->append(spaces && (previous=='.' || previous=='?' || previous=='!')?"  ":" ");}
        }
        result.parts.push_back({VimMotions::lineStart(buffer,row),start,end,begin+result.text->size()});copy(*result.text,buffer,start,end,step);
    }return result;
}
Plan changeCase(const PieceTreeBuffer& buffer,const std::vector<VimMotions::Range>& ranges,VimController::Command operation,std::size_t cursor,const Check& step){
    Builder builder;CodeContext::Reader reader(buffer);std::string pending;std::size_t runBegin=0,runEnd=0;
    const auto flush=[&]{if(!pending.empty()){builder.add(runBegin,runEnd,pending);pending.clear();}};
    for(const auto& range:ranges)for(auto at=range.begin;at<range.end;){
        if(at%4096<4)check(step);const auto [value,size]=character(reader,at,range.end);
        const auto upper=QChar::toUpper(value);const auto lower=value==0x130?char32_t('i'):QChar::toLower(value);
        const char32_t changed=operation==VimController::Command::UpperCase?upper:operation==VimController::Command::LowerCase?lower:QChar::isLower(value)?upper:lower;
        // Vim expands sharp s only for explicit uppercasing; ~ keeps it intact.
        const auto converted=operation==VimController::Command::UpperCase && value==0xdf?QByteArray("SS"):QString::fromUcs4(&changed,1).toUtf8();const std::string_view bytes(converted.data(),std::size_t(converted.size()));
        if(buffer.slice(at,size)==bytes){
            if(value=='\n' || value=='\r' || (!pending.empty() && runEnd!=at))flush();
            else if(!pending.empty()){pending+=bytes;runEnd=at+size;if(pending.size()>=65536)flush();}
        }
        else if(bytes.size()==size){if(!pending.empty() && runEnd!=at)flush();if(pending.empty())runBegin=at;pending+=bytes;runEnd=at+size;if(pending.size()>=65536)flush();}
        else {flush();builder.add(at,at+size,bytes);}at+=size;
    }flush();
    auto plan=builder.finish(cursor);plan.cursor=map(plan,cursor);return plan;
}
Plan number(const PieceTreeBuffer& buffer,const std::vector<VimMotions::Range>& ranges,int count,bool decrement,bool progressive,bool visual,const Options& options,const Check& step){
    Builder builder;CodeContext::Reader reader(buffer);std::size_t target=ranges.empty()?0:ranges.front().begin;std::uint64_t sequence=0;
    const auto formats=options.numberFormats.split(',');
    for(const auto& range:ranges){check(step);auto p=visual?range.begin:VimMotions::lineStart(buffer,buffer.lineIndexAt(range.begin));const auto lineEnd=visual?std::min(range.end,VimMotions::lineEnd(buffer,buffer.lineIndexAt(range.begin))):VimMotions::lineEnd(buffer,buffer.lineIndexAt(range.begin));
        while(p<lineEnd){check(step);const auto ch=reader.at(p);
            if(formats.contains("alpha") && ((ch>='a' && ch<='z') || (ch>='A' && ch<='Z')) && p>=range.begin && p<range.end){
                const auto amount=std::uint64_t(count)*(progressive?++sequence:1);const auto low=ch>='a'?'a':'A',high=ch>='a'?'z':'Z';const auto changed=decrement?int(std::max<std::int64_t>(low,std::int64_t(ch)-std::int64_t(amount))):int(std::min<std::uint64_t>(high,std::uint64_t(ch)+amount));const char result=char(changed);builder.add(p,p+1,{&result,1});target=p;break;
            }
            if(ch<'0' || ch>'9'){++p;continue;}
            auto begin=p,digitsBegin=p;int base=10;std::string prefix;bool upper=false;
            if(ch=='0' && p+2<lineEnd && formats.contains("hex") && (reader.at(p+1)=='x' || reader.at(p+1)=='X') && digit(reader.at(p+2))>=0){base=16;prefix=buffer.slice(p,2);digitsBegin=p+2;upper=prefix[1]=='X';}
            else if(ch=='0' && p+2<lineEnd && formats.contains("bin") && (reader.at(p+1)=='b' || reader.at(p+1)=='B') && (reader.at(p+2)=='0' || reader.at(p+2)=='1')){base=2;prefix=buffer.slice(p,2);digitsBegin=p+2;}
            auto end=digitsBegin;while(end<lineEnd && digit(reader.at(end))>=0 && digit(reader.at(end))<base){if((end&4095)==0)check(step);++end;}
            if(end<=range.begin || begin>=range.end){p=std::max(p+1,end);continue;}
            if(base==10 && ch=='0' && formats.contains("octal")){base=8;for(auto at=digitsBegin;at<end;++at)if(reader.at(at)=='8' || reader.at(at)=='9'){base=10;break;}}
            bool negative=false;if(base==10 && begin>VimMotions::lineStart(buffer,buffer.lineIndexAt(begin)) && reader.at(begin-1)=='-' && !formats.contains("unsigned") && (!visual || begin-1>=range.begin)){negative=true;--begin;}
            std::uint64_t value=0;bool overflow=false;
            for(auto at=digitsBegin;at<end;++at){if((at&4095)==0)check(step);const auto d=unsigned(digit(reader.at(at)));if(reader.at(at)>='a' && reader.at(at)<='f')upper=false;else if(reader.at(at)>='A' && reader.at(at)<='F')upper=true;if(value>(std::numeric_limits<std::uint64_t>::max()-d)/unsigned(base)){overflow=true;value=std::numeric_limits<std::uint64_t>::max();}else if(!overflow)value=value*unsigned(base)+d;}
            const auto amount=std::uint64_t(count)*(progressive?++sequence:1);
            const auto old=value;const bool subtract=decrement!=negative;
            if(!overflow)value=subtract?value-amount:value+amount;
            if(base==10){if(subtract && value>old){value=std::uint64_t(0)-value;negative=!negative;}else if(!subtract && value<old){value=~value;negative=!negative;}}
            if(formats.contains("unsigned") && negative){value=subtract?0:std::numeric_limits<std::uint64_t>::max();negative=false;}
            if(!value)negative=false;auto text=QString::number(qulonglong(value),base).toLatin1().toStdString();if(upper)for(auto& c:text)if(c>='a' && c<='f')c=char(c-'a'+'A');
            const bool padded=(!prefix.empty() || reader.at(digitsBegin)=='0') && (base!=10 || !formats.contains("octal"));if(padded && end-digitsBegin>text.size())text.insert(0,end-digitsBegin-text.size(),'0');
            if(negative)prefix.insert(0,"-");text=prefix+text;builder.add(begin,end,text);target=begin+text.size()-1;break;
        }
    }
    auto plan=builder.finish(target);if(visual)plan.cursor=map(plan,ranges.empty()?0:ranges.front().begin);else if(plan.edits->count){const auto edit=plan.edits->at(plan.edits->count-1);plan.cursor=map(plan,std::size_t(edit.begin))+std::size_t(edit.length)-1;}return plan;
}
Plan indent(const PieceTreeBuffer& buffer,std::size_t first,std::size_t last,const Options& options,const Check& step){
    Builder builder;CodeContext::Reader reader(buffer);CodeContext::State state;CodeContext::Rules rules;
    struct Frame {char open;int indent,column;bool content{false},switchBlock{false},caseActive{false};};std::vector<Frame> stack;
    int previousIndent=0,restoreIndent=0,commentIndent=0;bool control=false,restore=false;std::string previous;
    for(std::size_t row=0;row<=last;++row){check(step);const auto start=VimMotions::lineStart(buffer,row),text=VimMotions::firstText(buffer,row),end=VimMotions::lineEnd(buffer,row);
        const auto prefix=buffer.slice(text,std::min<std::size_t>(256,end-text));const bool empty=text==end;int columns=previousIndent;
        if(!stack.empty()){const auto& frame=stack.back();columns=frame.open=='{'?frame.indent+options.shiftWidth:frame.content?std::max(frame.column+1,frame.indent+options.shiftWidth*2):frame.indent+options.shiftWidth*2;}
        if(state.mode==CodeContext::Mode::BlockComment)columns=commentIndent+(reader.at(text)=='*'?1:0);
        else if(reader.at(text)=='#')columns=0;
        else if(reader.at(text)=='}' || reader.at(text)==')' || reader.at(text)==']'){
            const auto expected=reader.at(text)=='}'?'{':reader.at(text)==')'?'(':'[';
            for(auto it=stack.rbegin();it!=stack.rend();++it)if(it->open==expected){columns=it->indent;break;}
        }else if(prefix.starts_with("case ") || prefix.starts_with("default:")){for(auto it=stack.rbegin();it!=stack.rend();++it)if(it->switchBlock){it->caseActive=true;columns=it->indent+options.shiftWidth;break;}}
        else if(!stack.empty() && stack.back().open=='{' && stack.back().caseActive)columns+=options.shiftWidth;
        if(control && reader.at(text)!='{' && !prefix.starts_with("else"))columns=previousIndent+options.shiftWidth;
        else if(restore && stack.empty())columns=previousIndent;
        if(prefix.starts_with("else") && restore)columns=restoreIndent;
        const auto colon=prefix.find(':');
        if(colon!=std::string::npos && colon && prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_",0)==colon && (colon+1==prefix.size() || prefix[colon+1]!=':') && !prefix.starts_with("case") && !prefix.starts_with("default")){
            if(prefix.starts_with("public:") || prefix.starts_with("private:") || prefix.starts_with("protected:")){for(auto it=stack.rbegin();it!=stack.rend();++it)if(it->open=='{'){columns=it->indent+options.shiftWidth;it->caseActive=true;break;}}else columns=std::max(0,columns-options.shiftWidth);
        }
        if(empty)columns=0;
        const auto inserted=indentation(columns,options);
        if(row>=first && buffer.slice(start,text-start)!=inserted)builder.add(start,text,inserted);
        // Context outside the operated range uses its actual indentation.
        const int actual=row>=first?columns:VimBlock::column(buffer,row,text,options.tabWidth);
        int column=actual;
        for(auto p=text;p<end;){if((p&4095)==0)check(step);const char c=reader.at(p);const auto before=state.mode;
            if(before==CodeContext::Mode::Code){
                if(reader.matches(p,"/*"))commentIndent=column;
                if(c=='{' || c=='(' || c=='['){stack.push_back({c,actual,column,false,prefix.starts_with("switch") || (prefix.starts_with("{") && previous.starts_with("switch"))});}
                else if(c=='}' || c==')' || c==']'){const auto open=c=='}'?'{':c==')'?'(':'[';if(!stack.empty() && stack.back().open==open)stack.pop_back();}
                else if(!blank(c) && !stack.empty() && stack.back().open!='{')stack.back().content=true;
            }
            const auto next=CodeContext::advance(reader,p,state,rules);for(auto q=p;q<next;++q)column+=reader.at(q)=='\t'?options.tabWidth-column%options.tabWidth:1;p=next;
        }
        if(row+1<buffer.lineCount())CodeContext::advance(reader,buffer.lineEnd(row),state,rules);
        if(!empty){
            const auto tail=buffer.slice(end>128?std::max(text,end-128):text,end-(end>128?std::max(text,end-128):text));const bool statement=tail.ends_with(";");
            restore=control && statement;if(restore)restoreIndent=previousIndent;
            const bool word=prefix.starts_with("if (") || prefix.starts_with("if(") || prefix.starts_with("for (") || prefix.starts_with("for(") || prefix.starts_with("while (") || prefix.starts_with("while(") || prefix=="else" || prefix=="do";
            control=word && !tail.ends_with(";") && !tail.ends_with("{");previousIndent=prefix.starts_with("*/")?commentIndent:actual;previous=prefix;
        }
    }
    auto plan=builder.finish(VimMotions::lineStart(buffer,first));plan.cursor=map(plan,plan.cursor);return plan;
}
Plan format(const PieceTreeBuffer& buffer,std::size_t first,std::size_t last,const Options& options,std::string_view eol,const Check& step){
    auto output=std::make_shared<ChunkedFile>();CodeContext::Reader reader(buffer);bool paragraph=false;int column=0;std::string leader,indentPrefix;std::size_t lastOutputLine=0;
    const auto newline=[&]{output->append(eol);lastOutputLine=output->size();column=0;};
    const auto emitPrefix=[&]{output->append(indentPrefix+leader);column=displayWidth(indentPrefix+leader,0,options.tabWidth);};
    for(auto row=first;row<=last;++row){check(step);auto at=VimMotions::firstText(buffer,row);const auto start=VimMotions::lineStart(buffer,row),end=VimMotions::lineEnd(buffer,row);
        const auto indentationSize=at-start;if(indentationSize>1024*1024)throw std::runtime_error("Vim: formatting indentation exceeds 1 MiB");
        const auto rowIndent=buffer.slice(start,indentationSize);std::string rowLeader;
        for(const auto token:{"//","#","%",">","*"})if(buffer.slice(at,std::min<std::size_t>(std::char_traits<char>::length(token),end-at))==token){rowLeader=token;at+=rowLeader.size();while(at<end && (reader.at(at)==' ' || reader.at(at)=='\t')){rowLeader+=reader.at(at++);}break;}
        if(at==end){if(paragraph)newline();if(row<last)newline();paragraph=false;continue;}
        if(paragraph && rowLeader!=leader){newline();paragraph=false;}
        if(!paragraph){if(row>first && output->size() && output->read(output->size()-1,1)!="\n")newline();indentPrefix=rowIndent;leader=rowLeader;emitPrefix();paragraph=true;}
        bool fromLine=true;std::string gap;
        while(at<end){check(step);if(reader.at(at)==' ' || reader.at(at)=='\t'){gap+=reader.at(at++);continue;}
            const auto begin=at;while(at<end && reader.at(at)!=' ' && reader.at(at)!='\t'){if((at&4095)==0)check(step);++at;}
            const auto length=at-begin;int width=0;for(auto p=begin;p<at;){const auto bytes=buffer.slice(p,std::min<std::size_t>(65536,at-p));width+=displayWidth(bytes,column+width,options.tabWidth);p+=bytes.size();check(step);}
            if(fromLine && output->size()>lastOutputLine+indentPrefix.size()+leader.size() && gap.empty()){const auto previous=output->read(output->size()-1,1)[0];gap=options.joinSpaces && (previous=='.' || previous=='?' || previous=='!')?"  ":" ";}
            if(column>displayWidth(indentPrefix+leader,0,options.tabWidth) && column+displayWidth(gap,column,options.tabWidth)+width>options.textWidth){newline();emitPrefix();gap.clear();}
            if(output->size()>lastOutputLine+indentPrefix.size()+leader.size()){output->append(gap);column+=displayWidth(gap,column,options.tabWidth);}
            copy(*output,buffer,begin,begin+length,step);column+=width;gap.clear();fromLine=false;
        }
    }
    const auto begin=VimMotions::lineStart(buffer,first),end=VimMotions::lineEnd(buffer,last);Builder builder;
    // Avoid no-op history entries while retaining bounded comparisons.
    bool same=output->size()==end-begin;for(std::size_t p=0;same && p<output->size();p+=65536){check(step);const auto n=std::min<std::size_t>(65536,output->size()-p);same=output->read(p,n)==buffer.slice(begin+p,n);}
    if(!same)builder.addFile(begin,end,*output,step);return builder.finish(begin+lastOutputLine+indentPrefix.size());
}
std::size_t project(const Plan& plan,std::size_t position){return map(plan,position);}
HistoryResult apply(const Plan& plan,PieceTreeBuffer& buffer,EditHistory& history,std::size_t original,const Check& step){
    const auto edits=plan.edits;auto payload=std::make_shared<ChunkedFile>();
    for(auto upper=edits->count;upper;){const auto lower=upper>512?upper-512:0;const auto base=payload->size();std::vector<EditHistory::StoredChange> changes;
        std::string pending;const auto append=[&](std::string_view text){pending+=text;if(pending.size()>=65536){payload->append(pending);pending.clear();}};
        for(auto n=lower;n<upper;++n){check(step);const auto edit=edits->at(n);for(auto at=edit.begin;at<edit.end;at+=65536){check(step);append(buffer.slice(at,std::min<std::size_t>(65536,edit.end-at)));}for(std::size_t p=0;p<edit.length;p+=65536){check(step);append(edits->text->read(edit.offset+p,std::min<std::size_t>(65536,edit.length-p)));}changes.push_back({std::size_t(edit.begin),std::size_t(edit.end-edit.begin),std::size_t(edit.length)});}
        if(!pending.empty())payload->append(pending);
        for(auto n=upper;n>lower;){check(step);const auto edit=edits->at(--n);buffer.erase(edit.begin,edit.end-edit.begin);for(std::size_t p=0;p<edit.length;p+=65536){check(step);buffer.insert(edit.begin+p,edits->text->read(edit.offset+p,std::min<std::size_t>(65536,edit.length-p)));}}
        history.recordDiskBatch(changes,payload,{original},{plan.cursor},0,base);upper=lower;
    }
    payload->dropCache();return {plan.cursor,{plan.cursor}};
}
}
