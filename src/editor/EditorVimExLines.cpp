#include "EditorWidget.hpp"
#include "VimNormal.hpp"
#include "app/I18n.hpp"
#include <QFile>
#include <QRegularExpression>
#include <algorithm>
#include <numeric>
#include <limits>

namespace {
using Step=std::function<void(int)>;
void appendBuffer(ChunkedFile& file,const PieceTreeBuffer& buffer,std::size_t begin,std::size_t end,const Step& step){for(auto at=begin;at<end;at+=64*1024){step(30);file.append(buffer.slice(at,std::min<std::size_t>(64*1024,end-at)));}}
void appendFile(ChunkedFile& file,const ChunkedFile& source,std::size_t begin,std::size_t length,const Step& step){for(std::size_t at=0;at<length;at+=64*1024){step(40);file.append(source.read(begin+at,std::min<std::size_t>(64*1024,length-at)));}}
void change(PieceTreeBuffer& buffer,EditHistory& history,std::size_t at,std::size_t removed,const std::shared_ptr<ChunkedFile>& input,std::size_t before,std::size_t after,const Step& step,const std::shared_ptr<ChunkedFile>& shared={}){
    auto payload=shared?shared:std::make_shared<ChunkedFile>();const auto base=payload->size();appendBuffer(*payload,buffer,at,at+removed,step);if(input)appendFile(*payload,*input,0,input->size(),step);
    buffer.erase(at,removed);if(input)for(std::size_t p=0;p<input->size();p+=64*1024){step(60);buffer.insert(at+p,input->read(p,std::min<std::size_t>(64*1024,input->size()-p)));}
    history.recordDiskBatch({{at,removed,input?input->size():0}},payload,{before},{after},0,base);
}
std::pair<std::size_t,std::size_t> insertLines(PieceTreeBuffer& buffer,EditHistory& history,const std::shared_ptr<ChunkedFile>& source,std::size_t destination,bool finalNewline,std::string_view eol,std::size_t before,const Step& step,const std::shared_ptr<ChunkedFile>& shared={}){
    const auto rows=VimMotions::lastLine(buffer,finalNewline)+1;const auto at=destination<rows?VimMotions::lineStart(buffer,destination):buffer.size();
    auto input=VimState::convertNewlines(source,eol,[&]{step(35);});if(!input->size() || input->read(input->size()-1,1)!="\n")input->append(eol);
    auto text=std::make_shared<ChunkedFile>();
    if(destination==rows && !finalNewline){text->append(eol);const auto trim=eol.size();appendFile(*text,*input,0,input->size()-trim,step);}else appendFile(*text,*input,0,input->size(),step);
    change(buffer,history,at,0,text,before,at,step,shared);return {at,text->size()};
}
}
void EditorWidget::editVimExLines(const VimEx::Invocation& command,bool normal){
    const auto name=command.definition.name;auto first=std::size_t(command.first-1),last=std::size_t(command.last-1);const auto original=cursorPosition_;
    const bool joinSpaces=VimState::Session::instance().joinSpaces;
    const auto previousPattern=VimState::Session::instance().searchPattern;const bool ignoreCase=VimState::Session::instance().searchOptions.ignoreCase;
    const bool finalNewline=vimFinalNewline_;const auto eol=SmartEditing::eol(buffer_);const auto rows=VimMotions::lastLine(buffer_,finalNewline)+1;
    if(name=="join" && !command.explicitRange && !command.count)last=std::min(rows-1,first+1);
    if(name=="join" && !command.registerName.isNull()){finishVimEx("E488: Trailing characters");return;}
    const auto destination=std::size_t(command.destination);
    if(name=="move" && destination>first && destination<=last){finishVimEx("E134: Cannot move a range of lines into itself");return;}
    if(name=="move" && (destination==first || destination==last+1)){moveCursor(VimMotions::firstText(buffer_,last),false);scheduleVimEx();return;}
    std::vector<std::shared_ptr<VimEx::LineMarks>> trackers;std::vector<VimEx::LineMarks> moved;
    QVector<std::pair<VimState::Anchor,QPoint>> movedAnchors;
    if(name=="move" || name=="join"){
        for(auto& frame:vimExFrames_)if(frame.marks){trackers.push_back(frame.marks);moved.push_back(*frame.marks);if(name=="move")moved.back().move(qint64(first),qint64(last),qint64(destination));else moved.back().edit(qint64(first+1),0,qint64(last-first),0);}
        if(name=="move")for(const auto& anchor:VimState::Session::instance().anchors(this)){const auto row=buffer_.lineIndexAt(anchor->offset);if(row>=first && row<=last)movedAnchors.append({anchor,QPoint(int(row-first),int(buffer_.utf16Before(anchor->offset)-buffer_.utf16Before(buffer_.lineStart(row))))});}
    }
    const auto joinAnchors=VimState::Session::instance().anchors(this);std::vector<std::size_t> joinPositions;for(const auto& anchor:joinAnchors)joinPositions.push_back(anchor->offset);auto joinedPositions=std::make_shared<std::vector<std::size_t>>();
    auto insertedRow=std::make_shared<std::size_t>(0);
    runBulkOperation(I18n::text("Editing lines…"),[command,name,first,last,destination,original,finalNewline,eol,insertedRow,previousPattern,ignoreCase,normal,joinSpaces,joinPositions,joinedPositions](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        const auto begin=VimMotions::lineStart(buffer,first),end=last+1<buffer.lineCount()?buffer.lineStart(last+1):buffer.size();
        if(name=="copy" || name=="t" || name=="move"){
            auto source=std::make_shared<ChunkedFile>();appendBuffer(*source,buffer,begin,end,step);auto payload=std::make_shared<ChunkedFile>();auto target=destination;
            if(name=="move"){
                auto eraseBegin=begin;if(last==VimMotions::lastLine(buffer,finalNewline) && first>0 && !finalNewline)eraseBegin=VimMotions::lineEnd(buffer,first-1);
                change(buffer,history,eraseBegin,end-eraseBegin,{},original,eraseBegin,step,payload);if(target>last)target-=last-first+1;
            }
            const auto added=insertLines(buffer,history,source,target,finalNewline,eol,original,step,payload);*insertedRow=target;
            const auto row=std::min(VimMotions::lastLine(buffer,finalNewline),target+last-first);const auto cursor=VimMotions::firstText(buffer,row);(void)added;return HistoryResult{cursor,{cursor}};
        }
        if(name=="join"){
            if(first==last)return HistoryResult{original,{original}};
            const auto result=VimNormal::join(buffer,first,last,command.bang,joinSpaces,[&]{step(40);});
            for(const auto position:joinPositions)joinedPositions->push_back(result.project(position));
            const auto cursor=normal?result.cursor:begin;
            change(buffer,history,begin,VimMotions::lineEnd(buffer,last)-begin,result.text,original,cursor,step);
            const auto after=normal?cursor:VimMotions::firstText(buffer,first);return HistoryResult{after,{after}};
        }
        auto flags=command.arguments.trimmed();QString pattern;qsizetype slash=0;while(slash<flags.size() && (flags[slash].isLetter() || flags[slash].isSpace()))++slash;if(slash==flags.size())slash=-1;
        if(slash>=0){auto input=flags.mid(slash);auto parsed=VimEx::parseGlobal(input);if(!parsed.error.isEmpty())throw std::runtime_error(parsed.error.toStdString());pattern=parsed.pattern.isEmpty()?previousPattern:parsed.pattern;if(pattern.isEmpty())throw std::runtime_error("E35: No previous regular expression");flags=flags.left(slash).trimmed();if(parsed.commands!="print")flags+=parsed.commands.trimmed();}
        for(const auto c:flags)if(!QStringLiteral("iurnfxob ").contains(c))throw std::runtime_error("E488: Unsupported sort flag");
        if(last-first>=1000000)throw std::runtime_error("Vim: sort exceeds 1000000 lines");
        VimSearch::Subject subject(buffer,finalNewline,[&]{step(10);});VimSearch::Options options;options.ignoreCase=ignoreCase;std::optional<VimSearch::Pattern> expression;if(!pattern.isEmpty()){expression.emplace(pattern,options);if(!expression->error().isEmpty())throw std::runtime_error(expression->error().toStdString());}
        struct Line {std::size_t row,begin,end,keyBegin,keyEnd;long double number{};bool numeric{false};};std::vector<Line> lines;lines.reserve(last-first+1);
        const bool numeric=flags.contains('n') || flags.contains('f') || flags.contains('x') || flags.contains('o') || flags.contains('b');
        int numericFlags=0;for(const auto flag:QStringLiteral("nfxob"))numericFlags+=flags.contains(flag);if(numericFlags>1)throw std::runtime_error("E474: Invalid argument");
        const int base=flags.contains('x')?16:flags.contains('o')?8:flags.contains('b')?2:10;
        QRegularExpression numberExpression(base==16?"-?(?:0x)?[0-9a-fA-F]+":base==8?"-?[0-7]+":base==2?"-?[01]+":flags.contains('f')?"[-+]?(?:[0-9]+\\.?[0-9]*|\\.[0-9]+)(?:[eE][-+]?[0-9]+)?":"-?[0-9]+");
        for(auto row=first;row<=last;++row){step(30);const auto a=VimMotions::lineStart(buffer,row),b=VimMotions::lineEnd(buffer,row);Line line{row,a,b,a,b};
            if(expression){const auto match=expression->find(subject,a,[&]{step(40);});if(match && buffer.lineIndexAt(match->begin)==row){line.keyBegin=flags.contains('r')?match->begin:match->end;line.keyEnd=flags.contains('r')?match->end:b;}else line.keyBegin=line.keyEnd=b;}
            if(numeric){
                const std::string candidates=base==16?"-0123456789abcdefABCDEF":base==8?"-01234567":base==2?"-01":flags.contains('f')?"-+.0123456789":"-0123456789";
                bool found=false;
                for(auto block=line.keyBegin;block<line.keyEnd && !found;block+=4096){step(35);const auto chunk=subject.slice(block,std::min<std::size_t>(4096,line.keyEnd-block));
                    for(std::size_t offset=chunk.find_first_of(candidates);offset!=std::string_view::npos;offset=chunk.find_first_of(candidates,offset+1)){
                        step(35);const auto bytes=subject.slice(block+offset,std::min<std::size_t>(256,line.keyEnd-block-offset));
                        const auto match=numberExpression.match(QString::fromUtf8(bytes.data(),qsizetype(bytes.size())),0,QRegularExpression::NormalMatch,QRegularExpression::AnchorAtOffsetMatchOption);
                        if(!match.hasMatch())continue;
                        bool ok=false;line.number=flags.contains('f')?match.captured().toDouble(&ok):static_cast<long double>(match.captured().toLongLong(&ok,base));
                        if(!ok && !flags.contains('f')){line.number=match.captured().startsWith('-')?std::numeric_limits<qint64>::min():std::numeric_limits<qint64>::max();ok=true;}line.numeric=ok;found=true;break;
                    }
                }
            }
            lines.push_back(line);
        }
        const auto compareText=[&](std::size_t a,std::size_t aEnd,std::size_t b,std::size_t bEnd){
            while(a<aEnd && b<bEnd){step(50);auto size=std::min<std::size_t>({4096,aEnd-a,bEnd-b});const auto left=subject.slice(a,size),right=subject.slice(b,size);
                int result=0;
                if(flags.contains('i')){for(std::size_t i=0;i<size;++i){const auto fold=[](unsigned char c){return c>='A' && c<='Z'?c+32:int(c);};const auto x=fold(static_cast<unsigned char>(left[i])),y=fold(static_cast<unsigned char>(right[i]));if(x!=y){result=x<y?-1:1;break;}}}
                else result=left.compare(right);
                if(result)return result;
                a+=size;b+=size;
            }return a<aEnd?1:b<bEnd?-1:0;
        };
        std::stable_sort(lines.begin(),lines.end(),[&](const Line& a,const Line& b){int comparison=0;if(numeric)comparison=a.numeric!=b.numeric?(a.numeric?1:-1):a.number<b.number?-1:a.number>b.number?1:0;else comparison=compareText(a.keyBegin,a.keyEnd,b.keyBegin,b.keyEnd);return command.bang?comparison>0:comparison<0;});
        auto output=std::make_shared<ChunkedFile>();const Line* previous=nullptr;std::size_t written=0;
        for(const auto& line:lines){if(flags.contains('u') && previous && compareText(previous->begin,previous->end,line.begin,line.end)==0)continue;if(written++)output->append(eol);appendBuffer(*output,buffer,line.begin,line.end,step);previous=&line;}
        if(end>begin && buffer.byteAt(end-1)=='\n')output->append(eol);
        change(buffer,history,begin,end-begin,output,original,begin,step);const auto cursor=VimMotions::firstText(buffer,first);return HistoryResult{cursor,{cursor}};
    },true,{},[this,trackers,moved,movedAnchors,insertedRow,joinAnchors,joinedPositions](bool ok){
        if(!ok)return;
        for(std::size_t i=0;i<trackers.size();++i)*trackers[i]=moved[i];
        for(std::size_t i=0;i<joinedPositions->size();++i)joinAnchors[qsizetype(i)]->offset=(*joinedPositions)[i];
        for(const auto& [anchor,position]:movedAnchors){const auto row=std::min(buffer_.lineCount()-1,*insertedRow+std::size_t(position.x()));anchor->offset=std::min(lineContentEnd(row),buffer_.byteAtUtf16(buffer_.utf16Before(buffer_.lineStart(row))+std::size_t(position.y())));}
        scheduleVimEx();
    });
}
void EditorWidget::readVimExFile(const VimEx::Invocation& command){
    QString error;auto path=VimEx::fileArgument(command.arguments,filePath_,&error,property("vimAlternatePath").toString());if(!error.isEmpty()){finishVimEx(error);return;}if(path.isEmpty())path=filePath_;if(path.isEmpty()){finishVimEx("E32: No file name");return;}
    const auto destination=std::size_t(command.last),original=cursorPosition_;const bool finalNewline=vimFinalNewline_;const auto eol=SmartEditing::eol(buffer_);
    runBulkOperation(I18n::text("Reading file…"),[path,destination,original,finalNewline,eol](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        PieceTreeBuffer input;std::string error;if(!input.loadFile(QFile::encodeName(path).toStdString(),&error,{},[&](std::size_t,std::size_t,std::string_view){step(10);}))throw std::runtime_error(error);
        if(!input.size())return HistoryResult{original,{original}};
        auto source=std::make_shared<ChunkedFile>();appendBuffer(*source,input,VimMotions::lineStart(input,0),input.size(),step);
        const auto [at,length]=insertLines(buffer,history,source,destination,finalNewline,eol,original,step);
        const auto lastByte=at+length?at+length-1:0;const auto cursor=VimMotions::firstText(buffer,buffer.lineIndexAt(std::min(buffer.size(),lastByte)));return HistoryResult{cursor,{cursor}};
    },true,{},[this](bool ok){if(ok)scheduleVimEx();});
}
