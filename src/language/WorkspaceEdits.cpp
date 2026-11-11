#include "app/I18n.hpp"
#include "WorkspaceEdits.hpp"
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QSet>
#include <algorithm>
#include <cmath>
namespace {
bool integer(const QJsonValue& value, int* result) {
    if (!value.isDouble()) return false;
    const double number=value.toDouble();
    if (number<0 || number>2147483647 || std::floor(number)!=number) return false;
    *result=static_cast<int>(number); return true;
}
struct TextCoordinates {
    QByteArray bytes;
    QVector<int> starts;
    bool valid{false};
    explicit TextCoordinates(const QByteArray& input):bytes(input) {
        const int bom=bytes.startsWith("\xef\xbb\xbf")?3:0;
        const auto content=bytes.mid(bom);
        valid=!bytes.contains('\0') && QString::fromUtf8(content).toUtf8()==content;
        starts.append(bom);
        for (int i=bom;i<bytes.size();++i) if (bytes[i]=='\n') starts.append(i+1);
    }
    std::optional<int> offset(const QJsonValue& value) const {
        if (!valid || !value.isObject()) return {};
        const auto point=value.toObject(); int line,column;
        if (!integer(point.value("line"),&line) || !integer(point.value("character"),&column) || line>=starts.size()) return {};
        int end=line+1<starts.size()?starts[line+1]-1:bytes.size();
        if (end>starts[line] && bytes[end-1]=='\r') --end;
        const auto text=QString::fromUtf8(bytes.constData()+starts[line],end-starts[line]);
        if (column>text.size() || (column<text.size() && text[column].isLowSurrogate())) return {};
        return starts[line]+text.left(column).toUtf8().size();
    }
};
}
QString WorkspaceEdits::pathKey(const QString& path) {
    const QFileInfo info(path); const auto canonical=info.canonicalFilePath();
    return canonical.isEmpty()?QDir::cleanPath(info.absoluteFilePath()):canonical;
}
bool WorkspaceEdits::decode(const QJsonObject& workspace,QVector<WorkspaceDocumentEdit>* output,QString* error) {
    output->clear(); error->clear();
    auto add=[&](const QString& uri,const QJsonValue& version,const QJsonValue& edits) {
        const QUrl url(uri,QUrl::StrictMode);
        if (!url.isValid() || !url.isLocalFile() || (!url.host().isEmpty() && url.host()!="localhost") || url.hasQuery() || url.hasFragment() || !QDir::isAbsolutePath(url.toLocalFile())) {
            *error=I18n::text("Rename contains an unsupported document URI: ")+uri; return false;
        }
        if (!edits.isArray()) { *error=I18n::text("Rename edits must be an array."); return false; }
        WorkspaceDocumentEdit document; document.path=pathKey(url.toLocalFile()); document.edits=edits.toArray();
        if (!version.isUndefined() && !version.isNull()) {
            int number; if (!integer(version,&number)) { *error=I18n::text("Invalid document version."); return false; }
            document.version=number;
        }
        for (const auto& existing:*output) if (existing.path==document.path) { *error=I18n::text("Duplicate document in rename result."); return false; }
        output->append(std::move(document)); return true;
    };
    if (workspace.contains("documentChanges")) {
        if (!workspace.value("documentChanges").isArray()) { *error=I18n::text("Invalid documentChanges."); return false; }
        for (const auto& value:workspace.value("documentChanges").toArray()) {
            if (!value.isObject()) { *error=I18n::text("Invalid document edit."); return false; }
            const auto object=value.toObject();
            if (object.contains("kind") || !object.value("textDocument").isObject()) {
                *error=I18n::text("This rename includes file creation, deletion or file renaming, which is not supported."); return false;
            }
            const auto document=object.value("textDocument").toObject();
            if (!add(document.value("uri").toString(),document.value("version"),object.value("edits"))) return false;
        }
    } else if (workspace.contains("changes")) {
        if (!workspace.value("changes").isObject()) { *error=I18n::text("Invalid changes map."); return false; }
        const auto changes=workspace.value("changes").toObject();
        for (auto it=changes.begin();it!=changes.end();++it) if (!add(it.key(),QJsonValue::Null,it.value())) return false;
    } else if (!workspace.isEmpty()) { *error=I18n::text("Server returned no text changes."); return false; }
    std::sort(output->begin(),output->end(),[](const auto& a,const auto& b){return a.path<b.path;});
    return true;
}
bool WorkspaceEdits::convert(const QByteArray& before,const QJsonArray& edits,std::vector<EditChange>* output,QString* error) {
    output->clear(); error->clear(); TextCoordinates coordinates(before);
    if (!coordinates.valid) { *error=I18n::text("Rename requires valid UTF-8 text without binary data."); return false; }
    for (const auto& value:edits) {
        if (!value.isObject()) { *error=I18n::text("Invalid text edit."); return false; }
        const auto edit=value.toObject();
        if (edit.contains("annotationId")) { *error=I18n::text("Annotated edits require confirmation not supported by this client."); return false; }
        if (!edit.value("range").isObject() || !edit.value("newText").isString()) { *error=I18n::text("Invalid text edit range or replacement."); return false; }
        const auto range=edit.value("range").toObject();
        const auto a=coordinates.offset(range.value("start")),b=coordinates.offset(range.value("end"));
        if (!a || !b || *b<*a) { *error=I18n::text("Invalid UTF-16 range in rename result."); return false; }
        output->push_back({static_cast<std::size_t>(*a),before.mid(*a,*b-*a).toStdString(),edit.value("newText").toString().toUtf8().toStdString()});
    }
    std::sort(output->begin(),output->end(),[](const auto& a,const auto& b){return a.offset<b.offset;});
    for (std::size_t i=1;i<output->size();++i) {
        const auto& a=(*output)[i-1]; const auto& b=(*output)[i];
        if (b.offset<a.offset+a.removed.size() || b.offset==a.offset) { *error=I18n::text("Rename contains overlapping edits."); return false; }
    }
    return true;
}
bool WorkspaceEdits::convertFormatting(const QByteArray& before,const QJsonArray& edits,std::vector<EditChange>* output,QString* error) {
    if(!convert(before,edits,output,error)) return false;
    // Keep the original line-ending convention even when a server returns LF.
    const auto newline=before.indexOf('\n');
    const bool crlf=newline>0 && before[newline-1]=='\r';
    for(auto& change:*output) {
        auto text=QByteArray::fromStdString(change.inserted);
        text.replace("\r\n","\n");
        if(crlf) text.replace("\n","\r\n");
        change.inserted=text.toStdString();
    }
    output->erase(std::remove_if(output->begin(),output->end(),[](const auto& change){return change.removed==change.inserted;}),output->end());
    return true;
}
bool WorkspaceEdits::rangeText(const QByteArray& before,const QJsonObject& range,QString* text,QString* error) {
    TextCoordinates coordinates(before); const auto a=coordinates.offset(range.value("start")),b=coordinates.offset(range.value("end"));
    if (!a || !b || *b<*a) { *error=I18n::text("Invalid symbol range."); return false; }
    *text=QString::fromUtf8(before.mid(*a,*b-*a)); return true;
}
