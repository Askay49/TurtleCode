#include "language/WorkspaceEdits.hpp"
#include <QCoreApplication>
#include <QUrl>
#include <cassert>
static QJsonObject point(int line,int character) { return {{"line",line},{"character",character}}; }
static QJsonObject edit(int line,int a,int b,const QString& text) {
    return {{"range",QJsonObject{{"start",point(line,a)},{"end",point(line,b)}}},{"newText",text}};
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv); QString error; std::vector<EditChange> changes;
    const QByteArray before=QString::fromUtf8("// 😀\r\nint café = café;\r\n").toUtf8();
    assert(WorkspaceEdits::convert(before,QJsonArray{edit(1,11,15,"renamed"),edit(1,4,8,"renamed")},&changes,&error));
    assert(changes.size()==2 && changes[0].removed=="café");
    std::string after=before.toStdString(); for(auto it=changes.rbegin();it!=changes.rend();++it) after.replace(it->offset,it->removed.size(),it->inserted);
    assert(after=="// 😀\r\nint renamed = renamed;\r\n");
    assert(!WorkspaceEdits::convert(before,QJsonArray{edit(0,4,5,"x")},&changes,&error)); // split surrogate
    assert(!WorkspaceEdits::convert(before,QJsonArray{edit(99,0,1,"x")},&changes,&error));
    assert(!WorkspaceEdits::convert(before,QJsonArray{edit(1,0,5,"x"),edit(1,4,8,"y")},&changes,&error));
    assert(!WorkspaceEdits::convert(QByteArray("bad\xff"),QJsonArray{},&changes,&error));
    const QByteArray bom=QByteArray("\xef\xbb\xbf")+"int name;\n";
    assert(WorkspaceEdits::convert(bom,QJsonArray{edit(0,4,8,"value")},&changes,&error));
    assert(changes[0].offset==7 && changes[0].removed=="name");
    {
        const QByteArray original=QByteArray("\xef\xbb\xbf")+QString::fromUtf8("// 😀\r\nint  value=1;\r\n").toUtf8();
        QJsonArray edits{QJsonObject{{"range",QJsonObject{{"start",point(0,0)},{"end",point(2,0)}}},{"newText",QString::fromUtf8("// 😀\nint value = 1;\n")}}};
        assert(WorkspaceEdits::convertFormatting(original,edits,&changes,&error));
        assert(changes.size()==1 && changes[0].offset==3);
        auto result=original.toStdString();
        for(auto it=changes.rbegin();it!=changes.rend();++it) result.replace(it->offset,it->removed.size(),it->inserted);
        assert(result==std::string("\xef\xbb\xbf")+"// 😀\r\nint value = 1;\r\n");
        assert(WorkspaceEdits::convertFormatting("int value;\n",QJsonArray{edit(0,0,10,"int value;")},&changes,&error) && changes.empty());
        assert(!WorkspaceEdits::convertFormatting(original,QJsonArray{edit(0,4,5,"x")},&changes,&error));
    }
    QVector<WorkspaceDocumentEdit> documents;
    const auto uri=QUrl::fromLocalFile("/tmp/rename-target.cpp").toString();
    assert(WorkspaceEdits::decode(QJsonObject{{"changes",QJsonObject{{uri,QJsonArray{edit(0,0,1,"x")}}}}},&documents,&error));
    assert(documents.size()==1 && !documents[0].version);
    assert(WorkspaceEdits::decode(QJsonObject{{"documentChanges",QJsonArray{QJsonObject{{"textDocument",QJsonObject{{"uri",uri},{"version",7}}},{"edits",QJsonArray{edit(0,0,1,"x")}}}}}},&documents,&error));
    assert(documents.size()==1 && documents[0].version==7);
    assert(!WorkspaceEdits::decode(QJsonObject{{"changes",QJsonObject{{"https://example.com/source.cpp",QJsonArray{}}}}},&documents,&error));
    assert(!WorkspaceEdits::decode(QJsonObject{{"documentChanges",QJsonArray{QJsonObject{{"kind","rename"},{"oldUri",uri},{"newUri",uri+"2"}}}}},&documents,&error));
    assert(!WorkspaceEdits::decode(QJsonObject{{"documentChanges",QJsonArray{
        QJsonObject{{"textDocument",QJsonObject{{"uri",uri}}},{"edits",QJsonArray{}}},
        QJsonObject{{"textDocument",QJsonObject{{"uri",uri}}},{"edits",QJsonArray{}}}}}},&documents,&error));
}
