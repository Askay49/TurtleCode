#pragma once
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace DebugJson {
// Replace comments/commas with whitespace so diagnostics retain original offsets.
inline bool read(const QString& path,QJsonObject& object,QString& error,bool optional=false) {
    QFile file(path);
    if(optional && !file.exists()) {object={};return true;}
    if(!file.open(QIODevice::ReadOnly)) {error=path+": "+file.errorString();return false;}
    if(file.size()>1024*1024) {error=path+": Configuration exceeds 1 MiB";return false;}
    auto data=file.readAll();
    bool string=false,escape=false;
    for(int i=0;i<data.size();++i) {
        const char ch=data[i];
        if(string) {if(escape)escape=false;else if(ch=='\\')escape=true;else if(ch=='"')string=false;continue;}
        if(ch=='"') {string=true;continue;}
        if(ch=='/' && i+1<data.size() && data[i+1]=='/') {
            data[i++]= ' ';data[i]=' ';
            while(i+1<data.size() && data[i+1]!='\n' && data[i+1]!='\r')data[++i]=' ';
        } else if(ch=='/' && i+1<data.size() && data[i+1]=='*') {
            const int start=i; data[i++]=' ';data[i]=' ';bool closed=false;
            while(++i<data.size()) {
                if(data[i]=='*' && i+1<data.size() && data[i+1]=='/') {data[i++]=' ';data[i]=' ';closed=true;break;}
                if(data[i]!='\n' && data[i]!='\r')data[i]=' ';
            }
            if(!closed) {error=path+":"+QString::number(data.left(start).count('\n')+1)+": Unterminated block comment";return false;}
        }
    }
    string=false;escape=false;
    for(int i=0;i<data.size();++i) {
        const char ch=data[i];
        if(string) {if(escape)escape=false;else if(ch=='\\')escape=true;else if(ch=='"')string=false;continue;}
        if(ch=='"') {string=true;continue;}
        if(ch==',') {int next=i+1;while(next<data.size() && (data[next]==' ' || data[next]=='\t' || data[next]=='\r' || data[next]=='\n'))++next;
            if(next<data.size() && (data[next]==']' || data[next]=='}'))data[i]=' ';}
    }
    QJsonParseError parse;const auto doc=QJsonDocument::fromJson(data,&parse);
    if(parse.error!=QJsonParseError::NoError) {
        const auto prefix=data.left(parse.offset);
        error=path+QString(":%1:%2: %3").arg(prefix.count('\n')+1).arg(prefix.size()-prefix.lastIndexOf('\n')).arg(parse.errorString());return false;
    }
    if(!doc.isObject()) {error=path+": Expected a JSON object";return false;}
    object=doc.object();return true;
}
}
