#include "EditorConfig.hpp"
#include "core/text/ChunkedFile.hpp"
#include <editorconfig/editorconfig.h>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <algorithm>
#include <memory>

namespace EditorConfig {
Settings resolve(const QString& path) {
    Settings result;
    if(path.isEmpty()) return result;
    const auto handle=editorconfig_handle_init();
    if(!handle){result.warning=QStringLiteral("Cannot allocate EditorConfig parser");return result;}
    const auto cleanup=std::unique_ptr<void,decltype(&editorconfig_handle_destroy)>(handle,editorconfig_handle_destroy);
    const auto encoded=QFile::encodeName(QFileInfo(path).absoluteFilePath());
    const auto status=editorconfig_parse(encoded.constData(),handle);
    if(status!=0) {
        result.warning=QStringLiteral("EditorConfig: %1 (%2)")
            .arg(QString::fromUtf8(editorconfig_get_error_msg(status)),
                 QString::fromUtf8(editorconfig_handle_get_err_file(handle) ? editorconfig_handle_get_err_file(handle) : encoded.constData()));
        return result;
    }
    QMap<QString,QString> values;
    for(int i=0;i<editorconfig_handle_get_name_value_count(handle);++i) {
        const char* name=nullptr;const char* value=nullptr;
        editorconfig_handle_get_name_value(handle,i,&name,&value);
        if(name && value && QString::fromUtf8(value).compare("unset",Qt::CaseInsensitive)!=0)
            values.insert(QString::fromUtf8(name).toLower(),QString::fromUtf8(value).toLower());
    }
    const auto boolean=[&](const char* name)->std::optional<bool> {
        if(values.value(name)=="true")return true;
        if(values.value(name)=="false")return false;
        return {};
    };
    const auto number=[&](const char* name)->std::optional<int> {
        bool ok=false;const int value=values.value(name).toInt(&ok);
        return ok && value>0 && value<=256 ? std::optional<int>(value) : std::nullopt;
    };
    if(values.value("indent_style")=="tab")result.tabs=true;
    if(values.value("indent_style")=="space")result.tabs=false;
    result.indentSize=number("indent_size");result.tabWidth=number("tab_width");
    result.indentSizeIsTab=values.value("indent_size")=="tab";
    const auto eol=values.value("end_of_line");
    if(eol=="lf")result.eol="\n";
    if(eol=="crlf")result.eol="\r\n";
    if(eol=="cr")result.warning=QStringLiteral("EditorConfig: end_of_line=cr is not supported; keeping existing line endings");
    const auto charset=values.value("charset");
    if(charset=="utf-8")result.bom=false;
    else if(charset=="utf-8-bom")result.bom=true;
    else if(charset=="latin1" || charset=="utf-16le" || charset=="utf-16be")
        result.warning=QStringLiteral("EditorConfig: charset=%1 is not supported; keeping document bytes").arg(charset);
    result.trimWhitespace=boolean("trim_trailing_whitespace");
    result.finalNewline=boolean("insert_final_newline");
    return result;
}

HistoryResult normalize(PieceTreeBuffer& buffer,EditHistory& history,const std::vector<std::size_t>& before,
                        const Settings& settings,const std::function<void(int)>& checkpoint) {
    auto after=before;
    if(!settings.normalizes())return {after.front(),after};
    const auto source=buffer.snapshot();
    const auto check=[&](std::size_t offset) {if(checkpoint)checkpoint(int(offset*90/std::max<std::size_t>(1,source.size())));};
    std::shared_ptr<ChunkedFile> payload;
    std::vector<EditHistory::StoredChange> changes;
    std::ptrdiff_t delta=0;
    const auto change=[&](std::size_t begin,std::size_t end,std::string_view inserted) {
        if(end-begin==inserted.size() && source.slice(begin,end-begin)==inserted)return;
        if(!payload)payload=std::make_shared<ChunkedFile>();
        for(auto at=begin;at<end;at+=64*1024){check(at);payload->append(source.slice(at,std::min<std::size_t>(64*1024,end-at)));}
        payload->append(inserted);
        changes.push_back({begin,end-begin,inserted.size()});
        for(std::size_t i=0;i<before.size();++i) {
            if(before[i]<begin)continue;
            if(before[i]>=end)after[i]=std::size_t(std::ptrdiff_t(after[i])+std::ptrdiff_t(inserted.size())-std::ptrdiff_t(end-begin));
            else after[i]=std::size_t(std::ptrdiff_t(begin)+delta)+std::min(before[i]-begin,inserted.size());
        }
        const auto offset=std::size_t(std::ptrdiff_t(begin)+delta);
        buffer.erase(offset,end-begin);buffer.insert(offset,inserted);
        delta+=std::ptrdiff_t(inserted.size())-std::ptrdiff_t(end-begin);
    };
    const bool hasBom=source.size()>=3 && source.slice(0,3)=="\xef\xbb\xbf";
    const std::size_t body=hasBom?3:0;
    if(settings.bom && *settings.bom!=hasBom)change(0,body,*settings.bom?"\xef\xbb\xbf":"");
    const bool trim=settings.trimWhitespace.value_or(false);
    auto end=source.size();
    const auto trimEnd=[&] {while(trim && end>body && (source.byteAt(end-1)==' ' || source.byteAt(end-1)=='\t')){--end;if(end%65536==0)check(end);}};
    trimEnd();
    if(settings.finalNewline==false)while(end>body && source.byteAt(end-1)=='\n') {
        --end;if(end>body && source.byteAt(end-1)=='\r')--end;trimEnd();check(end);
    }
    std::string newline="\n";
    if(source.lineCount()>1){const auto first=source.lineEnd(0);if(first && source.byteAt(first-1)=='\r')newline="\r\n";}
    if(settings.eol)newline=*settings.eol;
    std::optional<std::size_t> whitespace;
    std::size_t position=0;
    source.forEachChunk([&](std::string_view chunk) {
        check(position);
        for(char ch:chunk) {
            const auto at=position++;
            if(at<body)continue;
            if(at>=end)return false;
            if(ch==' ' || ch=='\t'){if(!whitespace)whitespace=at;continue;}
            if(ch=='\r' && at+1<end && source.byteAt(at+1)=='\n')continue;
            if(ch=='\n') {
                const auto eolStart=at>body && source.byteAt(at-1)=='\r'?at-1:at;
                const auto start=trim && whitespace?*whitespace:eolStart;
                const auto original=source.slice(eolStart,at+1-eolStart);
                change(start,at+1,settings.eol?*settings.eol:original);
            }
            whitespace.reset();
        }
        return true;
    });
    if(end<source.size())change(end,source.size(),{});
    if(settings.finalNewline==true && end>body && source.byteAt(end-1)!='\n')change(source.size(),source.size(),newline);
    if(!changes.empty())history.recordDiskBatch(changes,payload,before,after,0);
    check(source.size());
    return {after.front(),after};
}
}
