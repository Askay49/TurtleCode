#include "VimEx.hpp"
#include "VimSearch.hpp"
#include <QDir>
#include <QFileInfo>
#include <limits>
#include <utility>

namespace VimEx {
namespace {
struct Reader {
    const QString& text; const Context& context;
    qsizetype at{}; QString error;
    void spaces(){while(at<text.size() && text[at].isSpace())++at;}
    QChar peek() const{return at<text.size()?text[at]:QChar{};}
    std::optional<qint64> number(){
        const auto begin=at;while(peek()>='0' && peek()<='9')++at;
        if(begin==at)return {};
        bool ok=false;const auto value=text.mid(begin,at-begin).toLongLong(&ok);
        if(!ok){error="E16: Invalid range";return {};}
        return value;
    }
    std::optional<qint64> address(qint64 current){
        spaces();qint64 value=current;bool found=true;
        const auto c=peek();
        if(c=='.'){++at;}
        else if(c=='$'){++at;value=context.last;}
        else if(c=='\''){
            ++at;const auto name=peek();if(name.isNull()){error="E20: Mark not set";return {};}
            ++at;const auto mark=context.mark?context.mark(name):std::nullopt;
            if(!mark){error="E20: Mark not set";return {};}value=*mark;
        } else if(c=='/' || c=='?'){
            ++at;QString pattern;
            while(at<text.size()){
                const auto ch=text[at++];
                if(ch==c)break;
                if(ch=='\\' && at<text.size()){pattern+=ch;pattern+=text[at++];}else pattern+=ch;
            }
            if(!context.search){error="Search addresses are not available yet";return {};}
            const auto result=context.search(pattern,c=='/',current);
            if(!result){error="E486: Pattern not found";return {};}value=*result;
        } else if(c>='0' && c<='9'){
            const auto n=number();if(!n)return {};value=*n;
        } else if(c!='+' && c!='-')found=false;
        if(!found)return {};
        for(;;){
            spaces();const auto sign=peek();const bool negative=sign=='-';
            if(sign=='+' || sign=='-')++at;
            else if(!(sign>='0' && sign<='9'))break;
            auto offset=number();if(!error.isEmpty())return {};
            const auto delta=offset.value_or(1);
            if((!negative && value>std::numeric_limits<qint64>::max()-delta) || (negative && value< std::numeric_limits<qint64>::min()+delta)){
                error="E16: Invalid range";return {};
            }
            value+=negative?-delta:delta;
        }
        return value;
    }
};
}
Registry::Registry(){
    // Order follows Vim's preferred abbreviations (not ambiguous-prefix errors).
    add({"close",3,false,true});
    add({"copy",2,true,false,false,Arguments::Address});
    add({"t",1,true,false,false,Arguments::Address});
    add({"delete",1,true,false,false,Arguments::RegisterCount});
    add({"edit",1,false,true,false,Arguments::File});
    add({"enew",3,false,true});
    add({"file",1,false,true,false,Arguments::File});
    add({"global",1,true,true,false,Arguments::Global,DefaultRange::Whole,false,false});
    add({"vglobal",1,true,false,false,Arguments::Global,DefaultRange::Whole,false,false});
    add({"join",1,true,true,false,Arguments::RegisterCount});
    add({"list",1,true});
    add({"number",2,true});
    add({"print",1,true});
    add({"put",2,true,true,true,Arguments::Register});
    add({"quit",1,false,true});
    add({"qall",2,false,true});
    add({"redo",3});
    add({"substitute",1,true,false,false,Arguments::Substitute});
    add({"smagic",2,true,false,false,Arguments::Substitute});
    add({"snomagic",3,true,false,false,Arguments::Substitute});
    add({"set",2,false,false,false,Arguments::Raw});
    add({"nohlsearch",3});
    add({"normal",4,true,true,false,Arguments::Raw,DefaultRange::Current,false,false});
    add({"move",1,true,false,false,Arguments::Address});
    add({"mark",2,true,false,false,Arguments::Register});
    add({"k",1,true,false,false,Arguments::Register});
    add({"read",1,true,false,true,Arguments::File});
    add({"registers",3,false,false,false,Arguments::Raw});
    add({"display",2,false,false,false,Arguments::Raw});
    add({"sort",3,true,true,false,Arguments::Raw,DefaultRange::Whole});
    add({"saveas",3,false,true,false,Arguments::File});
    add({"buffer",1,false,true,false,Arguments::Raw});
    add({"bnext",2,false,true});add({"bprevious",2,false,true});add({"bNext",2,false,true});
    add({"bfirst",2,false,true});add({"blast",2,false,true});
    add({"bdelete",2,false,true,false,Arguments::Raw});add({"bwipeout",2,false,true,false,Arguments::Raw});
    add({"buffers",7});add({"ls",2});add({"files",5});
    add({"@",1,true,false,false,Arguments::Register});
    add({"&",1,true,false,false,Arguments::Substitute});
    add({"~",1,true,false,false,Arguments::Substitute});
    add({"undo",1});
    add({"update",2,true,true,false,Arguments::File,DefaultRange::Whole});
    add({"write",1,true,true,false,Arguments::File,DefaultRange::Whole});
    add({"wq",2,true,true,false,Arguments::File,DefaultRange::Whole});
    add({"xit",1,true,true,false,Arguments::File,DefaultRange::Whole});
    add({"yank",1,true,false,false,Arguments::RegisterCount});
    add({"=",1,true,false,false,Arguments::None,DefaultRange::Whole});
    add({">",1,true,false,false,Arguments::RegisterCount});
    add({"<",1,true,false,false,Arguments::RegisterCount});
}
void Registry::add(Definition value){
    for(auto& existing:definitions_)if(existing.name==value.name){existing=std::move(value);return;}
    definitions_.append(std::move(value));
}
const Definition* Registry::find(const QString& name) const {
    for(const auto& definition:definitions_)if(name.size()>=definition.minimum && definition.name.startsWith(name))return &definition;
    return nullptr;
}
Parsed Registry::parse(const QString& line,const Context& context) const {
    Parsed result;Reader reader{line,context};reader.spaces();
    while(reader.peek()==':'){++reader.at;reader.spaces();}
    if(reader.peek().isNull() || reader.peek()=='"'){result.empty=true;return result;}
    for(;;){
        const auto begin=reader.at;while(reader.peek().isLetter())++reader.at;const auto word=line.mid(begin,reader.at-begin);
        if(word=="silent" || word=="sil"){result.command.silent=true;if(reader.peek()=='!'){result.command.ignoreErrors=true;++reader.at;}}
        else if(word=="keepjumps")result.command.keepJumps=true;
        else if(word=="keeppatterns")result.command.keepPatterns=true;
        else {reader.at=begin;break;}
        reader.spaces();
        while(reader.peek()==':'){++reader.at;reader.spaces();}
    }
    qint64 first=context.current,last=context.current,current=context.current;
    bool range=false;
    if(reader.peek()=='%' || reader.peek()=='*'){
        const auto symbol=reader.peek();++reader.at;range=true;
        if(symbol=='%'){first=1;last=context.last;}
        else {
            const auto a=context.mark?context.mark('<'):std::nullopt,b=context.mark?context.mark('>'):std::nullopt;
            if(!a || !b)reader.error="E20: Mark not set";else {first=*a;last=*b;}
        }
    } else {
        auto address=reader.address(current);
        if(address){range=true;first=last=*address;}
        while(reader.error.isEmpty() && (reader.peek()==',' || reader.peek()==';')){
            const auto separator=reader.peek();++reader.at;range=true;
            if(separator==';')current=last;
            address=reader.address(current);first=last;last=address.value_or(current);
        }
    }
    if(!reader.error.isEmpty()){result.error=reader.error;return result;}
    reader.spaces();const auto nameStart=reader.at;
    if(reader.peek()=='k')++reader.at;
    else while(reader.peek().isLetter() && reader.peek().unicode()<128)++reader.at;
    if(reader.at==nameStart && QStringLiteral("=><&~@").contains(reader.peek()) && !reader.peek().isNull())++reader.at;
    const auto name=line.mid(nameStart,reader.at-nameStart);
    auto& invocation=result.command;invocation.explicitRange=range;
    if(name.isEmpty()){
        invocation.definition={"",0,true,false,true};
        if(!reader.peek().isNull() && reader.peek()!='|' && reader.peek()!='"'){result.error="E492: Not an editor command: "+line.mid(nameStart);return result;}
        if(reader.peek()=='|'){++reader.at;result.rest=line.mid(reader.at);invocation.definition.name="print";}
    }else{
        const auto* definition=find(name);
        if(!definition){result.error="E492: Not an editor command: "+name;const auto bar=line.indexOf('|',reader.at);if(bar>=0)result.rest=line.mid(bar+1);return result;}
        invocation.definition=*definition;
        if(reader.peek()=='!'){invocation.bang=true;++reader.at;}
        if(invocation.bang && !definition->bang){result.error="E477: No ! allowed";return result;}
        if(range && !definition->range){result.error="E481: No range allowed";return result;}
        if(!range && definition->defaultRange==DefaultRange::Whole){first=1;last=context.last;}
        reader.spaces();QString argument;
        if(definition->arguments==Arguments::Substitute){
            const auto source=line.mid(reader.at);const auto parsed=VimSearch::parseSubstitute(source);
            if(!parsed.error.isEmpty()){result.error=parsed.error;return result;}
            result.rest=parsed.rest;argument=source;reader.at=line.size();
        }
        if(definition->arguments==Arguments::Global){argument=line.mid(reader.at);reader.at=line.size();}
        QChar sortDelimiter;bool sortPattern=false,sortSeen=false;
        while(reader.at<line.size()){
            const auto ch=line[reader.at++];
            if(ch=='\\' && reader.at<line.size()){
                // Preserve quoting for filename expansion and command-specific parsers.
                argument+=ch;argument+=line[reader.at++];continue;
            }
            if(definition->name=="sort"){
                if(sortPattern){argument+=ch;if(ch==sortDelimiter)sortPattern=false;continue;}
                if(!sortSeen && !ch.isLetter() && !ch.isSpace() && ch!='|' && ch!='"'){sortDelimiter=ch;sortPattern=sortSeen=true;argument+=ch;continue;}
            }
            if(ch=='|' && definition->bars){result.rest=line.mid(reader.at);break;}
            if(ch=='"' && definition->comments)break;
            argument+=ch;
        }
        invocation.arguments=(definition->arguments==Arguments::Substitute || definition->arguments==Arguments::Global || definition->name=="normal")?argument:argument.trimmed();
        if(definition->arguments==Arguments::None && !invocation.arguments.isEmpty()){result.error="E488: Trailing characters: "+invocation.arguments;return result;}
        if(definition->arguments==Arguments::Address){
            Reader destination{invocation.arguments,context};const auto address=destination.address(context.current);destination.spaces();
            if(!destination.error.isEmpty())result.error=destination.error;
            else if(!address || destination.at<invocation.arguments.size())result.error="E14: Invalid address";
            else if(*address<0 || *address>context.last)result.error="E16: Invalid range";
            else invocation.destination=*address;
        }
        if(definition->arguments==Arguments::Register || definition->arguments==Arguments::RegisterCount){
            Reader args{invocation.arguments,context};args.spaces();
            if(!args.peek().isNull() && (definition->arguments==Arguments::Register || !(args.peek()>='0' && args.peek()<='9'))){
                invocation.registerName=args.peek();++args.at;args.spaces();
                if(!QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\"-_*+.%#/: ").contains(invocation.registerName) && !(name=="@" && invocation.registerName=='@'))result.error="E488: Trailing characters: "+invocation.arguments;
            }
            if(definition->arguments==Arguments::RegisterCount){const auto count=args.number();if(count){if(*count<1 || *count>10000000)result.error="E939: Positive count required";else invocation.count=int(*count);}}
            args.spaces();if(args.at<invocation.arguments.size() || !args.error.isEmpty())result.error="E488: Trailing characters: "+invocation.arguments;
        }
    }
    invocation.first=first;invocation.last=last;
    if(first>last)result.error="E493: Backwards range given";
    else if(first<0 || last>context.last)result.error="E16: Invalid range";
    if(!invocation.definition.zero){invocation.first=std::max<qint64>(1,first);invocation.last=std::max<qint64>(1,last);}
    if(invocation.count){invocation.first=invocation.last;invocation.last=std::min(context.last,invocation.last+invocation.count-1);}
    return result;
}
Global parseGlobal(const QString& text){
    Global result;
    if(text.isEmpty() || text[0].isLetterOrNumber() || QStringLiteral("\\\"|!").contains(text[0])){result.error="E148: Invalid global command";return result;}
    const auto delimiter=text[0];qsizetype at=1;
    while(at<text.size()){const auto c=text[at++];if(c==delimiter)break;result.pattern+=c;if(c=='\\' && at<text.size())result.pattern+=text[at++];}
    result.commands=text.mid(at);if(result.commands.trimmed().isEmpty())result.commands="print";
    return result;
}
struct LineMarks::Node {qint64 row{},lazy{};unsigned priority{};std::shared_ptr<Node> left,right;};
LineMarks::LineMarks()=default;LineMarks::~LineMarks()=default;
LineMarks::LineMarks(const LineMarks& other):root_(other.root_),random_(other.random_){}
LineMarks& LineMarks::operator=(const LineMarks& other){if(this!=&other){root_=other.root_;random_=other.random_;}return *this;}
void LineMarks::shift(std::shared_ptr<Node>& node,qint64 delta){if(node && delta){if(!node.unique())node=std::make_shared<Node>(*node);node->row+=delta;node->lazy+=delta;}}
void LineMarks::push(Node& node){shift(node.left,node.lazy);shift(node.right,node.lazy);node.lazy=0;}
std::pair<std::shared_ptr<LineMarks::Node>,std::shared_ptr<LineMarks::Node>> LineMarks::split(std::shared_ptr<Node> node,qint64 row){
    if(!node)return {};if(!node.unique())node=std::make_shared<Node>(*node);push(*node);
    if(node->row<row){auto parts=split(std::move(node->right),row);node->right=std::move(parts.first);return {std::move(node),std::move(parts.second)};}
    auto parts=split(std::move(node->left),row);node->left=std::move(parts.second);return {std::move(parts.first),std::move(node)};
}
std::shared_ptr<LineMarks::Node> LineMarks::merge(std::shared_ptr<Node> a,std::shared_ptr<Node> b){
    if(!a)return b;if(!b)return a;
    if(a->priority>b->priority){if(!a.unique())a=std::make_shared<Node>(*a);push(*a);a->right=merge(std::move(a->right),std::move(b));return a;}
    if(!b.unique())b=std::make_shared<Node>(*b);push(*b);b->left=merge(std::move(a),std::move(b->left));return b;
}
void LineMarks::add(qint64 row){random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;auto node=std::make_shared<Node>();node->row=row;node->priority=random_;auto parts=split(std::move(root_),row);root_=merge(merge(std::move(parts.first),std::move(node)),std::move(parts.second));}
bool LineMarks::empty() const{return !root_;}
std::optional<qint64> LineMarks::takeFirst(){
    if(!root_)return {};auto* node=root_.get();qint64 lazy=0;
    while(node->left){lazy+=node->lazy;node=node->left.get();}
    const auto row=node->row+lazy;auto a=split(std::move(root_),row),b=split(std::move(a.second),row+1);root_=merge(std::move(a.first),std::move(b.second));return row;
}
void LineMarks::edit(qint64 row,qint64 column,qint64 removed,qint64 inserted){
    if(removed){const auto first=row+(column?1:0);auto a=split(std::move(root_),first),b=split(std::move(a.second),first+removed);shift(b.second,-removed);root_=merge(std::move(a.first),std::move(b.second));}
    if(inserted){auto a=split(std::move(root_),row+(column?1:0));shift(a.second,inserted);root_=merge(std::move(a.first),std::move(a.second));}
}
void LineMarks::move(qint64 first,qint64 last,qint64 destination){
    if(destination>=first && destination<=last+1)return;
    const auto count=last-first+1;auto a=split(std::move(root_),first),b=split(std::move(a.second),last+1);
    shift(b.second,-count);auto rest=merge(std::move(a.first),std::move(b.second));
    const auto target=destination>last?destination-count:destination;auto c=split(std::move(rest),target);shift(c.second,count);shift(b.first,target-first);root_=merge(merge(std::move(c.first),std::move(b.first)),std::move(c.second));
}
QStringList Registry::complete(const QString& prefix) const {
    QStringList result;for(const auto& value:definitions_)if(value.name.startsWith(prefix))result<<value.name;result.sort();return result;
}
QString fileArgument(const QString& argument,const QString& currentFile,QString* error,const QString& alternateFile){
    if(argument.startsWith('!') || argument.startsWith(">>") || argument.startsWith("++")){*error="This write form is not available yet";return {};}
    QString path;
    for(qsizetype i=0;i<argument.size();++i){
        const auto c=argument[i];
        if(c=='\\' && i+1<argument.size()){path+=argument[++i];continue;}
        if(c.isSpace()){*error="E172: Only one file name allowed";return {};}
        if(c=='%' || c=='#'){
            auto expanded=c=='%'?currentFile:alternateFile;
            if(expanded.isEmpty()){*error=c=='%'?"E499: Empty file name for '%'":"E194: No alternate file name";return {};}
            while(i+1<argument.size() && argument[i+1]==':'){
                if(i+2>=argument.size()){*error="Invalid file name modifier";return {};}
                const auto modifier=argument[i+2];i+=2;const QFileInfo file(expanded);
                if(modifier=='p')expanded=file.absoluteFilePath();
                else if(modifier=='h')expanded=file.path();
                else if(modifier=='t')expanded=file.fileName();
                else if(modifier=='r'){const auto suffix=file.suffix();if(!suffix.isEmpty())expanded.chop(suffix.size()+1);}
                else if(modifier=='e')expanded=file.suffix();
                else {*error="This file name modifier is not available yet";return {};}
            }
            path+=expanded;
        }else if(c=='$' || c=='*' || c=='?' || c=='`' || (c=='~' && i!=0)){
            *error="File expansion is not available for this expression";return {};
        }else path+=c;
    }
    if(path.startsWith("~/"))path=QDir::home().filePath(path.mid(2));
    return path.isEmpty()?path:QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
}
