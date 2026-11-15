#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include "VimSearch.hpp"
#include <QTemporaryFile>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <algorithm>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <exception>
#include <utility>

namespace VimSearch {
namespace {
struct Translation {QString regex,error;int caseOverride{},captures{};QStringList starts,ends,virtuals;};
Translation translate(const QString& text,bool magic){
    Translation result;auto& out=result.regex;int mode=magic?2:1;int atom=-1,branch=0;
    bool ignoreComposing=false;
    for(qsizetype i=0;i+1<text.size();++i)if(text[i]=='\\'){if(text[++i]=='Z')ignoreComposing=true;}
    struct Frame {int atom,branch;};QVector<Frame> frames;
    const auto appendLiteral=[&](const QString& base,const QString& marks,bool followingComposing=false){
        atom=out.size();QString value=base.isEmpty()?QStringLiteral("[^\\r\\n\\p{M}]"):QRegularExpression::escape(base);
        if(!marks.isEmpty() && (!ignoreComposing || base.isEmpty())){
            for(qsizetype i=0;i<marks.size();++i){QString mark(marks[i]);if(marks[i].isHighSurrogate() && i+1<marks.size())mark+=marks[++i];value+="(?=\\p{M}*"+QRegularExpression::escape(mark)+")";}
            value+="\\p{M}*";
        }else if(ignoreComposing)value+="\\p{M}*";
        else if(!followingComposing)value+="(?!\\p{M})";
        out+="(?:"+value+")";
    };
    const auto literal=[&](QChar c){appendLiteral(QString(c),{});};
    const auto takeMarks=[&](qsizetype& i){QString marks;while(i+1<text.size()){
        const auto c=text[i+1];bool combining=c.isMark();int size=1;
        if(c.isHighSurrogate() && i+2<text.size() && text[i+2].isLowSurrogate()){combining=QChar::isMark(QChar::surrogateToUcs4(c,text[i+2]));size=2;}
        if(!combining)break;marks+=text.mid(i+1,size);i+=size;
    }return marks;};
    const auto newlineAtom=[&]{const auto name=QStringLiteral("vn%1").arg(result.virtuals.size());result.virtuals<<name;return QStringLiteral("(?:\\r?\\n|(?<%1>\\z))").arg(name);};
    const auto open=[&](bool capture){const int at=int(out.size());frames.append({at,branch});out+=capture?QStringLiteral("(?<v%1>").arg(++result.captures):QStringLiteral("(?:");branch=int(out.size());atom=-1;};
    const auto close=[&]{if(frames.isEmpty()){result.error="E54: Unmatched group";return;}const auto frame=frames.takeLast();out+=')';atom=frame.atom;branch=frame.branch;};
    for(qsizetype i=0;i<text.size() && result.error.isEmpty();++i){
        auto c=text[i];bool escaped=false;
        if(c=='\\'){
            if(++i==text.size()){literal('\\');break;}escaped=true;c=text[i];
            if(c=='v' || c=='m' || c=='M' || c=='V'){mode=c=='v'?3:c=='m'?2:c=='M'?1:0;continue;}
            if(c=='Z')continue;
            if(c=='c' || c=='C'){result.caseOverride=c=='c'?1:-1;continue;}
            if(c=='z' && i+1<text.size() && (text[i+1]=='s' || text[i+1]=='e')){
                const bool start=text[++i]=='s';auto& names=start?result.starts:result.ends;
                const auto name=QStringLiteral("%1%2").arg(start?"zs":"ze").arg(names.size());names<<name;
                out+=QStringLiteral("(?<%1>)").arg(name);continue;
            }
            if(c=='%' && i+1<text.size()){
                c=text[++i];
                if(c=='('){open(false);continue;}
                if(c=='C'){atom=out.size();out+="\\p{M}*";continue;}
                if(c=='#' && i+2<text.size() && text[i+1]=='=' && QStringLiteral("012").contains(text[i+2])){i+=2;continue;}
                if(c=='d' || c=='x' || c=='u' || c=='U'){
                    const auto begin=i+1;const int maximum=c=='d'?10:c=='x'?2:c=='u'?4:8;const int base=c=='d'?10:16;
                    while(i+1<text.size() && i+1-begin<maximum && (base==10?text[i+1].isDigit():QStringLiteral("0123456789abcdefABCDEF").contains(text[i+1])))++i;
                    bool ok=false;const auto value=text.mid(begin,i-begin+1).toUInt(&ok,base);
                    if(!ok || value>0x10ffff){result.error="E678: Invalid character code";break;}
                    atom=out.size();out+=QStringLiteral("\\x{%1}").arg(value,0,16);continue;
                }
                if(c=='^' || c=='$'){atom=out.size();out+=c=='^'?"\\A":"\\z";continue;}
                result.error="Unsupported Vim pattern atom: \\%"+QString(c);break;
            }
            if(c=='@'){
                if(atom<0){result.error="E866: Misplaced lookaround";break;}
                QString kind;
                if(text.mid(i+1,2)=="<="){kind="?<=";i+=2;}
                else if(text.mid(i+1,2)=="<!"){kind="?<!";i+=2;}
                else if(i+1<text.size() && text[i+1]=='='){kind="?=";++i;}
                else if(i+1<text.size() && text[i+1]=='!'){kind="?!";++i;}
                else if(i+1<text.size() && text[i+1]=='>'){kind="?>";++i;}
                else {result.error="E869: Invalid lookaround";break;}
                const auto part=out.mid(atom);out.truncate(atom);out+='('+kind+part+')';continue;
            }
            bool newline=false;
            if(c=='_' && i+1<text.size()){newline=true;c=text[++i];}
            QString token;
            if(c=='n')token=newlineAtom();
            else if(c=='t')token="\\t";
            else if(c=='r')token="\\r(?!\\n)";
            else if(c=='e')token="\\x1b";
            else if(c=='b')token="\\x08";
            else if(c>='1' && c<='9')token=QStringLiteral("\\k<v%1>").arg(c.digitValue());
            else if(c=='<' && mode!=3)token="(?<![\\p{L}\\p{N}_])(?=[\\p{L}\\p{N}_])";
            else if(c=='>' && mode!=3)token="(?<=[\\p{L}\\p{N}_])(?![\\p{L}\\p{N}_])";
            else {
                const auto upper=c.toUpper();QString chars;
                if(upper=='D')chars="0-9";else if(upper=='X')chars="0-9A-Fa-f";else if(upper=='O')chars="0-7";
                else if(upper=='W')chars="A-Za-z0-9_";else if(upper=='H')chars="A-Za-z_";
                else if(upper=='A')chars="A-Za-z";else if(upper=='L')chars="a-z";else if(upper=='U')chars="A-Z";
                else if(upper=='K' || upper=='I')chars="\\p{L}\\p{N}_";
                else if(upper=='S')chars=" \\t";
                if(upper=='K' || upper=='I'){if(c.isUpper())chars="\\p{L}_";token="["+chars+"]";}
                else if(!chars.isEmpty())token=c.isUpper()?"[^\\r\\n"+chars+"]":"["+chars+"]";
                if(!token.isEmpty())token="(?:(?!\\p{M})"+token+"\\p{M}*)";
            }
            if(newline && token.isEmpty() && c=='.')token="(?:(?!\\r?\\n)\\X)";
            if(newline && (c=='^' || c=='$'))token=QString(c);
            if(!token.isEmpty()){
                atom=out.size();out+=newline?"(?:"+token+"|"+newlineAtom()+")":token;continue;
            }
            if(newline){result.error="Unsupported Vim multiline atom";break;}
            if(QStringLiteral("fFpP").contains(c)){result.error="Unsupported Vim character class: \\"+QString(c);break;}
            if(c=='Z' || c=='z'){result.error="Unsupported Vim pattern atom: \\"+QString(c);break;}
        }
        const bool special=mode==3?!escaped:(c=='(' || c==')' || c=='|' || c=='+' || c=='?' || c=='=' || c=='{' || c=='&')?escaped:
            (c=='.' || c=='[' || c=='*')?(mode==2?!escaped:mode==1?escaped:false):(c=='^' || c=='$')?mode!=0 && !escaped:false;
        if(c=='~' && (mode>=2?!escaped:mode==1 && escaped)){result.error="Previous-replacement pattern atom ~ is not supported";break;}
        if(special && c=='('){open(true);continue;}
        if(special && (c=='<' || c=='>')){atom=out.size();out+=c=='<'?"(?<![\\p{L}\\p{N}_])(?=[\\p{L}\\p{N}_])":"(?<=[\\p{L}\\p{N}_])(?![\\p{L}\\p{N}_])";continue;}
        if(special && c==')'){close();continue;}
        if(special && c=='|'){out+='|';branch=out.size();atom=-1;continue;}
        if(special && c=='&'){const auto part=out.mid(branch);out.truncate(branch);out+="(?="+part+")";atom=-1;continue;}
        if(special && (c=='*' || c=='+' || c=='?' || c=='=' || c=='{')){
            if(atom<0){if(c=='*'){literal(c);continue;}result.error="E866: Misplaced repetition";break;}
            if((c=='*' || c=='+') && out.mid(atom)==QStringLiteral("(?:(?!\\r?\\n)\\X)")){out.truncate(atom);out+="[^\\r\\n]";}
            QString after;const auto part=out.mid(atom),guard=QStringLiteral("(?!\\p{M}))");
            if(part.startsWith("(?:") && part.endsWith(guard)){
                out.truncate(atom);out+="(?:"+part.mid(3,part.size()-3-guard.size());after=")(?!\\p{M})";
            }
            if(c=='{'){
                const auto begin=i+1;const auto end=text.indexOf('}',begin);
                if(end<0){result.error="E554: Missing }";break;}
                auto number=text.mid(begin,end-begin);bool lazy=number.startsWith('-');if(lazy)number.remove(0,1);
                if(number.isEmpty()){if(!lazy){result.error="E554: Invalid repetition";break;}out+="*?";}
                else if(QRegularExpression("^[0-9]*(,[0-9]*)?$").match(number).hasMatch()){if(number.startsWith(','))number.prepend('0');out+='{'+number+'}'+(lazy?"?":"");}
                else {result.error="E554: Invalid repetition";break;}i=end;
            }else out+=c=='='?'?':c;
            out+=after;
            continue;
        }
        if(special && c=='['){
            atom=out.size();QString collection;bool closed=false;auto j=i+1;
            if(j<text.size() && text[j]=='^')collection+=text[j++];
            if(j<text.size() && text[j]==']')collection+=text[j++];
            for(;j<text.size();++j){
                const auto value=text[j];
                if(value=='[' && j+1<text.size() && text[j+1]==':'){
                    const auto end=text.indexOf(":]",j+2);if(end<0)break;collection+=text.mid(j,end-j+2);j=end+1;continue;
                }
                if(value==']'){closed=true;break;}
                if(value=='\\' && j+1<text.size()){collection+=value;collection+=text[++j];}else collection+=value;
            }
            if(!closed){literal('[');continue;}
            const std::pair<const char*,const char*> ascii[]={{"[:alpha:]","A-Za-z"},{"[:alnum:]","A-Za-z0-9"},{"[:lower:]","a-z"},{"[:upper:]","A-Z"},{"[:digit:]","0-9"},{"[:xdigit:]","A-Fa-f0-9"}};
            for(const auto& [name,value]:ascii)collection.replace(QString::fromLatin1(name),QString::fromLatin1(value));
            out+="(?:(?![\\r\\n\\p{M}])["+collection+"]\\p{M}*)";i=j;continue;
        }
        if(special && (c=='.' || c=='^' || c=='$')){
            if(c=='^' && atom>=0){literal(c);continue;}
            if(c=='$' && i+1<text.size() && text[i+1]!='\\' && text[i+1]!='|' && text[i+1]!=')'){literal(c);continue;}
            if(c=='.'){const auto marks=takeMarks(i);if(!marks.isEmpty())appendLiteral({},marks);else {atom=out.size();out+="(?:(?!\\r?\\n)\\X)";}continue;}
            atom=out.size();out+=c;continue;
        }
        QString base(c);
        if(c.isHighSurrogate() && i+1<text.size() && text[i+1].isLowSurrogate())base+=text[++i];
        auto marks=takeMarks(i);
        if(c.isMark()){marks.prepend(base);base.clear();}
        appendLiteral(base,marks,text.mid(i+1,3)=="\\%C");
    }
    if(!frames.isEmpty() && result.error.isEmpty())result.error="E54: Unmatched group";
    if(result.captures>9)result.error="E872: Too many capture groups";
    return result;
}
struct Control {
    Checkpoint checkpoint;QElapsedTimer timer;int checks{};std::exception_ptr exception;
    explicit Control(Checkpoint callback):checkpoint(std::move(callback)){timer.start();}
};
int callout(pcre2_callout_block*,void* opaque){
    auto& control=*static_cast<Control*>(opaque);
    if(++control.checks%1024==0){
        try{if(control.checkpoint)control.checkpoint();if(control.timer.elapsed()>2000)throw std::runtime_error("Vim pattern exceeded the matching time limit");}
        catch(...){control.exception=std::current_exception();return PCRE2_ERROR_CALLOUT;}
    }
    return 0;
}
}
struct Subject::Data {QTemporaryFile file;uchar* mapped{};std::size_t length{},base{},end{};~Data(){if(mapped)file.unmap(mapped);}};
Subject::Subject(const PieceTreeBuffer& buffer,bool finalNewline,const Checkpoint& check):data_(std::make_unique<Data>()){
    auto& d=*data_;if(!d.file.open())throw std::runtime_error("Cannot create Vim search subject");
    buffer.forEachChunk([&](std::string_view chunk){for(std::size_t at=0;at<chunk.size();at+=64*1024){if(check)check();const auto size=std::min<std::size_t>(64*1024,chunk.size()-at);if(d.file.write(chunk.data()+at,qint64(size))!=qint64(size))throw std::runtime_error("Cannot write Vim search subject");}return true;});
    if(!d.file.flush())throw std::runtime_error("Cannot flush Vim search subject");
    d.length=buffer.size();d.end=d.length;
    if(d.length){d.mapped=d.file.map(0,qint64(d.length));if(!d.mapped)throw std::runtime_error("Cannot map Vim search subject");}
    if(d.length>=3 && std::memcmp(d.mapped,"\xef\xbb\xbf",3)==0)d.base=3;
    if(finalNewline && d.end>d.base && d.mapped[d.end-1]=='\n'){--d.end;if(d.end>d.base && d.mapped[d.end-1]=='\r')--d.end;}
}
Subject::~Subject()=default;
std::size_t Subject::size() const{return data_->end;}
std::size_t Subject::begin() const{return data_->base;}
std::string_view Subject::slice(std::size_t at,std::size_t length) const {if(at>data_->length || length>data_->length-at)throw std::runtime_error("Invalid Vim match span");return length?std::string_view(reinterpret_cast<const char*>(data_->mapped)+at,length):std::string_view{};}
std::size_t Subject::next(std::size_t at) const {if(at>=size())return size()+1;++at;while(at<size() && (data_->mapped[at]&0xc0)==0x80)++at;return at;}
std::size_t Subject::previous(std::size_t at) const {at=std::min(at,size());if(at<=begin())return begin();--at;while(at>begin() && (data_->mapped[at]&0xc0)==0x80)--at;return at;}
struct Pattern::Data {
    pcre2_code* code{};QString error;std::array<int,10> captures{};QVector<int> starts,ends,virtuals;
    ~Data(){if(code)pcre2_code_free(code);}
};
Pattern::Pattern(const QString& text,Options options):data_(std::make_unique<Data>()){
    auto translated=translate(text,options.magic);data_->error=translated.error;if(!data_->error.isEmpty())return;
    const auto pattern=translated.regex.toUtf8();int error;PCRE2_SIZE offset;
    auto* context=pcre2_compile_context_create(nullptr);pcre2_set_newline(context,PCRE2_NEWLINE_ANYCRLF);
    bool upper=false;for(qsizetype i=0;i<text.size();++i){if(text[i]=='\\' && i+1<text.size()){++i;continue;}if(text[i].isUpper())upper=true;}
    const auto force=translated.caseOverride?translated.caseOverride:options.caseOverride;
    const bool ignore=force?force>0:options.ignoreCase && !(options.smartCase && upper);
    data_->code=pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.constData()),pattern.size(),PCRE2_UTF|PCRE2_UCP|PCRE2_MULTILINE|PCRE2_AUTO_CALLOUT|PCRE2_MATCH_INVALID_UTF|(ignore?PCRE2_CASELESS:0),&error,&offset,context);
    pcre2_compile_context_free(context);
    if(!data_->code){PCRE2_UCHAR message[256];pcre2_get_error_message(error,message,sizeof message);data_->error="Vim pattern: "+QString::fromUtf8(reinterpret_cast<const char*>(message));return;}
    for(int i=1;i<=translated.captures;++i){const auto name=QStringLiteral("v%1").arg(i).toLatin1();data_->captures[i]=pcre2_substring_number_from_name(data_->code,reinterpret_cast<PCRE2_SPTR>(name.constData()));}
    for(const auto& name:translated.starts)data_->starts.append(pcre2_substring_number_from_name(data_->code,reinterpret_cast<PCRE2_SPTR>(name.toLatin1().constData())));
    for(const auto& name:translated.ends)data_->ends.append(pcre2_substring_number_from_name(data_->code,reinterpret_cast<PCRE2_SPTR>(name.toLatin1().constData())));
    for(const auto& name:translated.virtuals)data_->virtuals.append(pcre2_substring_number_from_name(data_->code,reinterpret_cast<PCRE2_SPTR>(name.toLatin1().constData())));
}
Pattern::~Pattern()=default;Pattern::Pattern(Pattern&&) noexcept=default;Pattern& Pattern::operator=(Pattern&&) noexcept=default;
QString Pattern::error() const{return data_->error;}
std::optional<Match> Pattern::find(const Subject& subject,std::size_t from,const Checkpoint& check) const {
    if(!data_->error.isEmpty())throw std::runtime_error(data_->error.toStdString());
    if(from>subject.size())return {};
    if(check)check();Control control(check);
    auto* context=pcre2_match_context_create(nullptr);pcre2_set_match_limit(context,5000000);pcre2_set_depth_limit(context,1000);pcre2_set_heap_limit(context,16384);pcre2_set_callout(context,callout,&control);
    auto* match=pcre2_match_data_create_from_pattern(data_->code,nullptr);
    static const unsigned char empty=0;const auto* bytes=subject.data_->mapped?subject.data_->mapped+subject.begin():&empty;
    const int result=pcre2_match(data_->code,bytes,subject.size()-subject.begin(),std::max(from,subject.begin())-subject.begin(),0,match,context);
    Match value;
    if(result>=0){const auto* positions=pcre2_get_ovector_pointer(match);const auto base=subject.begin();value.rawBegin=value.begin=positions[0]+base;value.rawEnd=value.end=positions[1]+base;
        for(const auto index:data_->starts)if(index>0 && positions[index*2]!=PCRE2_UNSET)value.begin=positions[index*2]+base;
        for(const auto index:data_->ends)if(index>0 && positions[index*2]!=PCRE2_UNSET)value.end=positions[index*2]+base;
        for(const auto index:data_->virtuals)if(index>0 && positions[index*2]!=PCRE2_UNSET)value.virtualNewline=true;
        value.end=std::max(value.begin,value.end);value.groups[0]={value.begin,value.end,true};
        for(int i=1;i<10;++i){const auto index=data_->captures[i];if(index>0 && positions[index*2]!=PCRE2_UNSET)value.groups[i]={positions[index*2]+base,positions[index*2+1]+base,true};}
    }
    pcre2_match_data_free(match);pcre2_match_context_free(context);
    if(control.exception)std::rethrow_exception(control.exception);
    if(result<0 && result!=PCRE2_ERROR_NOMATCH)throw std::runtime_error("Vim pattern exceeded matching limits or could not be evaluated");
    return result>=0?std::optional<Match>(value):std::nullopt;
}
Navigation navigate(const Subject& subject,const Pattern& pattern,std::size_t cursor,bool forward,int count,const Checkpoint& check,bool endOfMatch,int characterOffset){
    std::deque<Match> before;std::size_t total=0,rank=0;int after=0;
    for(auto at=subject.begin();at<=subject.size();){
        const auto match=pattern.find(subject,at,check);if(!match)break;
        auto position=endOfMatch && match->end>match->begin?subject.previous(match->end):match->begin;
        for(int n=0;n<std::abs(characterOffset);++n)position=characterOffset>0?std::min(subject.size(),subject.next(position)):subject.previous(position);
        if(forward && position>cursor && ++after==count)return {match,false};
        if(position<cursor){++rank;before.push_back(*match);if(before.size()>std::size_t(count))before.pop_front();}
        else if(!forward && before.size()==std::size_t(count))return {before.front(),false};
        ++total;at=std::max(subject.next(match->rawBegin),match->end);
    }
    if(!forward && before.size()==std::size_t(count))return {before.front(),false};
    if(!total)return {};
    // Wrapped counts can span several complete trips, without retaining all matches.
    const auto target=forward?(total-std::size_t(after)+std::size_t(count)-1)%total:(rank+total-(std::size_t(count)%total))%total;
    std::size_t index=0;
    for(auto at=subject.begin();at<=subject.size();){const auto match=pattern.find(subject,at,check);if(!match)break;if(index++==target)return {match,true};at=std::max(subject.next(match->rawBegin),match->end);}
    return {};
}
QString literal(const QString& text,bool wholeWord){QString result="\\V";auto value=text;value.replace("\\","\\\\");result+=value;return wholeWord?"\\<"+result+"\\m\\>":result;}
Substitute parseSubstitute(const QString& text){
    Substitute result;if(text.isEmpty() || text[0].isLetterOrNumber() || text[0].isSpace() || text[0]=='&'){
        result.repeat=true;const auto bar=text.indexOf('|'),comment=text.indexOf('"');const auto end=bar<0?comment:comment<0?bar:std::min(bar,comment);
        result.flags=(end<0?text:text.left(end)).trimmed();if(bar>=0 && bar==end)result.rest=text.mid(bar+1);return result;
    }
    const auto delimiter=text[0];if(delimiter=='\\' || delimiter=='"' || delimiter=='|'){result.error="E146: Invalid substitute delimiter";return result;}
    qsizetype at=1;const auto part=[&](QString& output){
        while(at<text.size()){
            const auto c=text[at++];if(c==delimiter)return true;
            if(c=='\\' && at<text.size()){
                const auto next=text[at++];output+=c;output+=next;
            }else output+=c;
        }
        return false;
    };
    if(!part(result.pattern)){result.error="E476: Invalid substitute command";return result;}
    part(result.replacement);
    const auto flagsStart=at;
    while(at<text.size() && text[at]!='|' && text[at]!='"')++at;
    result.flags=text.mid(flagsStart,at-flagsStart).trimmed();
    if(at<text.size() && text[at]=='|')result.rest=text.mid(at+1);
    return result;
}
QString expandReplacement(const QString& text,const QString& previous,QString* error){
    QString result;
    if(text.startsWith("\\=")){*error="Expression replacements require Vimscript and are not supported";return {};}
    for(qsizetype i=0;i<text.size();++i){if(text[i]=='\\' && i+1<text.size()){result+=text[i];result+=text[++i];}else if(text[i]=='~')result+=previous;else result+=text[i];}
    return result;
}
static void renderReplacement(const QString& program,const Subject& subject,const Match& match,std::string_view eol,const std::function<void(std::string_view)>& output,const Checkpoint& check){
    int mode=0,next=0;
    const auto append=[&](std::string_view bytes){
        for(std::size_t at=0;at<bytes.size();){if(check)check();auto size=std::min<std::size_t>(64*1024,bytes.size()-at);
            while(at+size<bytes.size() && size && (static_cast<unsigned char>(bytes[at+size])&0xc0)==0x80)--size;
            if(!mode && !next)output(bytes.substr(at,size));
            else {auto text=QString::fromUtf8(bytes.data()+at,qsizetype(size));QString converted;
                for(qsizetype i=0;i<text.size();){auto size=1;if(text[i].isHighSurrogate() && i+1<text.size() && text[i+1].isLowSurrogate())size=2;auto part=text.mid(i,size);const auto change=next?std::exchange(next,0):mode;converted+=change>0?part.toUpper():change<0?part.toLower():part;i+=size;}const auto result=converted.toUtf8();output({result.data(),std::size_t(result.size())});}
            at+=size;
        }
    };
    for(qsizetype i=0;i<program.size();++i){auto c=program[i];
        if(c=='&'){append(subject.slice(match.begin,match.end-match.begin));continue;}
        if(c=='\\' && i+1<program.size()){
            c=program[++i];
            if(c=='u' || c=='l'){next=c=='u'?1:-1;continue;}
            if(c=='U' || c=='L'){mode=c=='U'?1:-1;continue;}
            if(c=='E' || c=='e'){mode=next=0;continue;}
            if(c>='0' && c<='9'){const auto span=match.groups[c.digitValue()];if(span.set)append(subject.slice(span.begin,span.end-span.begin));continue;}
            if(c=='r'){append(eol);continue;}if(c=='n'){append(std::string_view("\0",1));continue;}if(c=='t'){append("\t");continue;}if(c=='b'){append("\b");continue;}
        }
        QString part(c);if(c.isHighSurrogate() && i+1<program.size() && program[i+1].isLowSurrogate())part+=program[++i];const auto bytes=part.toUtf8();append({bytes.data(),std::size_t(bytes.size())});
    }
}
void replacement(const QString& program,const Subject& subject,const Match& match,std::string_view eol,ChunkedFile& output,const Checkpoint& check){renderReplacement(program,subject,match,eol,[&](std::string_view bytes){output.append(bytes);},check);}
Edit Plan::at(std::size_t index) const {const auto bytes=records->read(index*sizeof(Edit),sizeof(Edit));if(bytes.size()!=sizeof(Edit))throw std::runtime_error("Invalid substitution plan");Edit result;std::memcpy(&result,bytes.data(),sizeof result);return result;}
std::shared_ptr<Plan> plan(const PieceTreeBuffer& buffer,const Subject& subject,const Pattern& pattern,const QString& program,std::size_t first,std::size_t last,bool global,bool countOnly,std::string_view eol,const Checkpoint& check,const std::function<bool(std::size_t)>& selected){
    auto result=std::make_shared<Plan>();std::optional<std::size_t> changedRow,nonemptyEnd;
    std::string pendingText,pendingRecords;std::size_t textSize=0;
    const auto write=[&](std::string_view bytes){textSize+=bytes.size();while(!bytes.empty()){const auto size=std::min<std::size_t>(64*1024-pendingText.size(),bytes.size());pendingText.append(bytes.substr(0,size));bytes.remove_prefix(size);if(pendingText.size()==64*1024){result->text->append(pendingText);pendingText.clear();}}};
    auto at=std::max(buffer.lineStart(first),subject.begin());
    const auto afterEmpty=[&](std::size_t from){auto next=subject.next(from);const auto row=buffer.lineIndexAt(from);const auto end=buffer.lineEnd(row);if(next==end && from<end)next=row+1<buffer.lineCount()?buffer.lineStart(row+1):subject.size()+1;return next;};
    while(at<=subject.size()){
        if(check)check();const auto found=pattern.find(subject,at,check);if(!found)break;const auto& match=*found;
        const auto row=buffer.lineIndexAt(match.begin);if(row>last)break;
        if(selected && !selected(row)){if(row+1>=buffer.lineCount())break;at=buffer.lineStart(row+1);continue;}
        if(nonemptyEnd && match.begin==match.end && match.begin==*nonemptyEnd && !match.virtualNewline){at=afterEmpty(match.rawBegin);continue;}
        const auto offset=textSize;if(!countOnly)renderReplacement(program,subject,match,eol,write,check);
        const Edit edit{match.begin,match.end,offset,textSize-offset};
        if(!countOnly){pendingRecords.append(reinterpret_cast<const char*>(&edit),sizeof edit);if(pendingRecords.size()>=64*1024){result->records->append(pendingRecords);pendingRecords.clear();}}
        ++result->count;if(!changedRow || *changedRow!=row){++result->lines;changedRow=row;}
        nonemptyEnd=match.end>match.begin?std::optional<std::size_t>(match.end):std::nullopt;
        if(!global){const auto endRow=buffer.lineIndexAt(match.end>match.begin?match.end-1:match.end);if(endRow+1>=buffer.lineCount())break;at=buffer.lineStart(endRow+1);}
        else at=match.end>match.begin?match.end:afterEmpty(match.rawBegin);
    }
    if(!pendingText.empty())result->text->append(pendingText);if(!pendingRecords.empty())result->records->append(pendingRecords);
    result->text->dropCache();result->records->dropCache();return result;
}
}
