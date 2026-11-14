#define PCRE2_CODE_UNIT_WIDTH 16
#include <pcre2.h>
#include "SearchScanner.hpp"
#include "project/WorkspaceFiles.hpp"
#include <QFile>
#include <QSet>
#include <QTemporaryFile>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QDir>
#include <limits>
#include <stdexcept>
namespace {
struct Subject {
    QString text;
    QTemporaryFile file;
    uchar* mapped{};
    qsizetype units{};
    ~Subject(){if(mapped)file.unmap(mapped);}
    void append(const QString& part) {
        if(!file.isOpen() && text.size()+part.size()>64*1024) {
            if(!file.open())throw std::runtime_error("Cannot spill long search line to disk");
            if(file.write(reinterpret_cast<const char*>(text.utf16()),text.size()*2)!=text.size()*2)throw std::runtime_error("Cannot write long search line");
            text.clear();text.squeeze();
        }
        if(file.isOpen()) {if(file.write(reinterpret_cast<const char*>(part.utf16()),part.size()*2)!=part.size()*2)throw std::runtime_error("Cannot write long search line");}
        else text+=part;
        units+=part.size();
    }
    QStringView view() {
        if(!file.isOpen())return QStringView(text);
        if(!file.flush())throw std::runtime_error("Cannot flush long search line");
        mapped=file.map(0,file.size());if(!mapped)throw std::runtime_error("Cannot map long search line");
        return QStringView(reinterpret_cast<const QChar*>(mapped),units);
    }
};
qint64 utf8Bytes(QStringView text) {
    qint64 total=0;
    while(!text.isEmpty()) {
        auto size=std::min<qsizetype>(64*1024,text.size());
        if(size<text.size() && text[size-1].isHighSurrogate())--size;
        total+=text.first(size).toString().toUtf8().size();text=text.mid(size);
    }
    return total;
}
bool acceptsPattern(const QString& path,const QStringList& patterns) {
    for(const auto& pattern:patterns)if(QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern,QRegularExpression::UnanchoredWildcardConversion),QRegularExpression::CaseInsensitiveOption).match(path).hasMatch())return true;
    return false;
}
QStringList patterns(QString text) {text.replace(';',',');QStringList result;for(const auto& item:text.split(',',Qt::SkipEmptyParts))result<<item.trimmed();return result;}
struct MatchControl { const std::atomic_bool* cancel{};QElapsedTimer timer;int checks{};bool timedOut{},bounded{true}; };
int callout(pcre2_callout_block*,void* opaque) {
    auto& control=*static_cast<MatchControl*>(opaque);
    if(control.cancel->load())return PCRE2_ERROR_CALLOUT;
    if(control.bounded && (++control.checks&1023)==0 && control.timer.elapsed()>2000){control.timedOut=true;return PCRE2_ERROR_CALLOUT;}
    return 0;
}
}
ProjectSearchResult scanProject(const ProjectSearchOptions& options,const std::shared_ptr<std::atomic_bool>& cancelled,const std::function<void(const ProjectSearchResult&)>& progress) {
    ProjectSearchResult result;
    if(options.query.isEmpty())return result;
    QString pattern=options.regularExpression?options.query:QRegularExpression::escape(options.query);
    if(options.wholeWord)pattern="(?<![\\p{L}\\p{N}_])(?:"+pattern+")(?![\\p{L}\\p{N}_])";
    int error{};PCRE2_SIZE errorOffset{};
    using Code=std::unique_ptr<pcre2_code,decltype(&pcre2_code_free)>;
    Code code(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.utf16()),pattern.size(),PCRE2_UTF|PCRE2_AUTO_CALLOUT|PCRE2_NO_START_OPTIMIZE|(options.matchCase?0:PCRE2_CASELESS),&error,&errorOffset,nullptr),pcre2_code_free);
    if(!code){PCRE2_UCHAR message[256];pcre2_get_error_message(error,message,256);result.error=QString::fromUtf16(reinterpret_cast<const char16_t*>(message));return result;}
    std::unique_ptr<pcre2_match_data,decltype(&pcre2_match_data_free)> data(pcre2_match_data_create_from_pattern(code.get(),nullptr),pcre2_match_data_free);
    std::unique_ptr<pcre2_match_context,decltype(&pcre2_match_context_free)> context(pcre2_match_context_create(nullptr),pcre2_match_context_free);
    MatchControl control{cancelled.get(),{},0,false,options.regularExpression};pcre2_set_callout(context.get(),callout,&control);
    pcre2_set_match_limit(context.get(),options.regularExpression?10000000:std::numeric_limits<uint32_t>::max());pcre2_set_depth_limit(context.get(),1000);pcre2_set_heap_limit(context.get(),8192);
    const auto includes=patterns(options.filesToInclude),excludes=patterns(options.filesToExclude);
    QSet<QString> visited;QElapsedTimer delivery;delivery.start();
    for(const auto& inputRoot:options.roots) {
        const auto root=QDir(inputRoot).absolutePath();WorkspaceFiles workspace(root,{options.showHidden,options.includeIgnored,{}});
        workspace.walk([&](const QFileInfo& info) {
            const auto path=info.absoluteFilePath();const auto relative=QDir(root).relativeFilePath(path);
            if(acceptsPattern(relative,excludes) || (!includes.isEmpty() && !acceptsPattern(relative,includes)) || (options.roots.size()>1 && visited.contains(path)))return;
            if(options.roots.size()>1)visited.insert(path);
            QFile file(path);if(!file.open(QIODevice::ReadOnly) || file.peek(8192).contains('\0'))return;
            ++result.filesSearched;const auto firstMatch=result.matches.size();
            QCryptographicHash hash(QCryptographicHash::Sha256);int lineNumber=0;qint64 offset=0;bool valid=true,limited=false;
            while(!file.atEnd() && !cancelled->load() && valid) {
                Subject subject;QByteArray carry;bool endLine=false; qint64 lineBytes=0;
                while(!file.atEnd() && !endLine && !cancelled->load()) {
                    const QByteArray block=file.readLine(64*1024);
                    if(block.isEmpty()){valid=file.error()==QFileDevice::NoError;break;}
                    hash.addData(block);lineBytes+=block.size();endLine=block.endsWith('\n');
                    QByteArray bytes=carry+block;carry.clear();
                    // Retain an incomplete UTF-8 code point at a block boundary.
                    int lead=bytes.size()-1;while(lead>=0 && (uchar(bytes[lead])&0xc0)==0x80)--lead;
                    if(lead>=0){const uchar ch=uchar(bytes[lead]);const int width=ch<0x80?1:ch>=0xf0?4:ch>=0xe0?3:ch>=0xc0?2:1;if(bytes.size()-lead<width){carry=bytes.mid(lead);bytes.truncate(lead);}}
                    const auto part=QString::fromUtf8(bytes);if(bytes.contains('\0') || part.toUtf8()!=bytes){valid=false;break;}subject.append(part);
                }
                if(!carry.isEmpty())valid=false;
                if(!valid || cancelled->load())break;
                ++lineNumber;const auto line=subject.view();PCRE2_SIZE from=0;qsizetype lastColumn=0;qint64 lastByte=0;
                while(from<=PCRE2_SIZE(line.size()) && !cancelled->load()) {
                    control.timer.restart();control.checks=0;control.timedOut=false;
                    const int matched=pcre2_match(code.get(),reinterpret_cast<PCRE2_SPTR>(line.utf16()),line.size(),from,0,data.get(),context.get());
                    if(matched==PCRE2_ERROR_NOMATCH)break;
                    if(matched<0){if(!cancelled->load()){valid=false;limited=true;}break;}
                    const auto* range=pcre2_get_ovector_pointer(data.get());const qsizetype start=range[0],end=range[1];
                    from=end;
                    if(end==start){if(from>=PCRE2_SIZE(line.size()))break;from+=line[from].isHighSurrogate()?2:1;continue;}
                    lastByte+=utf8Bytes(line.mid(lastColumn,start-lastColumn));
                    ProjectSearchMatch item;item.path=path;item.relativePath=relative;item.line=lineNumber;item.column=int(start+1);item.length=int(end-start);
                    const auto previewStart=std::max<qsizetype>(0,start-40);item.preview=line.mid(previewStart,180).toString().trimmed();
                    item.byteOffset=offset+lastByte;item.byteLength=utf8Bytes(line.mid(start,end-start));item.matchedText=line.mid(start,end-start).toString();
                    if(options.regularExpression)for(uint32_t c=0;c<pcre2_get_ovector_count(data.get());++c) item.captures<< (range[c*2]==PCRE2_UNSET?QString{}:line.mid(range[c*2],range[c*2+1]-range[c*2]).toString());
                    lastColumn=end;lastByte+=item.byteLength;result.matches.push_back(item);
                }
                offset+=lineBytes;
            }
            if(!valid || file.error()!=QFileDevice::NoError || cancelled->load()) {
                result.matches.truncate(firstMatch);
                if(limited){++result.skippedFiles;if(result.warnings.size()<100)result.warnings<<path+": regular expression exceeded its execution budget";}
            } else if(result.matches.size()>firstMatch){result.fileHashes.insert(path,hash.result());result.matches.sealFile(firstMatch);}
            if(delivery.elapsed()>=200 && !cancelled->load()){progress(result);delivery.restart();}
        },cancelled);
    }
    return result;
}
