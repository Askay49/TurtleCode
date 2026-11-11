#include "DapClient.hpp"
#include <QJsonDocument>
#include <QDateTime>
#include <QMetaMethod>

DapClient::DapClient(QObject* parent):QObject(parent) {
    connect(&process_,&QProcess::started,this,&DapClient::ready);
    connect(&process_,&QProcess::readyReadStandardOutput,this,&DapClient::read);
    connect(&process_,&QProcess::readyReadStandardError,this,[this]{ emit eventReceived("output",{{"output",QString::fromUtf8(process_.readAllStandardError())}}); });
    connect(&process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){ if(e==QProcess::FailedToStart) { emit fatalError(process_.errorString()); emit error(process_.errorString()); pending_.clear(); emit finished(); } });
    connect(&process_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus status){
        input_.clear(); pending_.clear(); timer_.stop();
        if(code!=0 || status==QProcess::CrashExit) {const auto message=tr("Debug adapter exited (%1)").arg(code); emit fatalError(message); emit error(message);}
        emit finished();
    });
    timer_.setInterval(500);
    connect(&timer_,&QTimer::timeout,this,[this]{
        const auto now=QDateTime::currentMSecsSinceEpoch();
        for(auto it=pending_.cbegin();it!=pending_.cend();++it) if(it->deadline<now) { fail(tr("Debug request timed out: %1").arg(it->command)); return; }
    });
}
DapClient::~DapClient() {
    disconnect(&process_,nullptr,this,nullptr);
    if(active()) {
        request("disconnect",{{"terminateDebuggee",true}});
        process_.waitForBytesWritten(200);
        if(!process_.waitForFinished(800)) { process_.kill(); process_.waitForFinished(500); }
    }
}
void DapClient::start(const QString& command,const QStringList& arguments,const QString& workingDirectory,const QProcessEnvironment& environment) {
    if(active()) return;
    ++generation_; input_.clear(); pending_.clear(); sequence_=0;
    process_.setWorkingDirectory(workingDirectory);process_.setProcessEnvironment(environment);
    process_.start(command,arguments); timer_.start();
}
int DapClient::request(const QString& command,const QJsonObject& arguments,Reply reply) {
    if(!active()) return -1;
    const int seq=++sequence_;
    const auto data=QJsonDocument(QJsonObject{{"seq",seq},{"type","request"},{"command",command},{"arguments",arguments}}).toJson(QJsonDocument::Compact);
    pending_.insert(seq,{std::move(reply),QDateTime::currentMSecsSinceEpoch()+30000,command});
    process_.write("Content-Length: "+QByteArray::number(data.size())+"\r\n\r\n"+data);
    return seq;
}
void DapClient::respond(const QJsonObject& request,bool success,const QJsonObject& body,const QString& message) {
    if(!active()) return;
    const auto data=QJsonDocument(QJsonObject{{"seq",++sequence_},{"type","response"},{"request_seq",request.value("seq")},{"command",request.value("command")},{"success",success},{"body",body},{"message",message}}).toJson(QJsonDocument::Compact);
    process_.write("Content-Length: "+QByteArray::number(data.size())+"\r\n\r\n"+data);
}
void DapClient::stop() {
    if(!active()) return;
    request("disconnect",{{"terminateDebuggee",true}},[this](const QJsonObject&){ process_.closeWriteChannel(); });
    const auto generation=generation_;
    QTimer::singleShot(1500,this,[this,generation]{ if(generation==generation_ && active()) process_.terminate(); });
    QTimer::singleShot(2500,this,[this,generation]{ if(generation==generation_ && active()) process_.kill(); });
}
void DapClient::fail(const QString& message) { pending_.clear(); emit fatalError(message); emit error(message); process_.kill(); }
void DapClient::read() {
    input_+=process_.readAllStandardOutput();
    constexpr int limit=8*1024*1024;
    while(true) {
        const auto end=input_.indexOf("\r\n\r\n");
        if(end<0) { if(input_.size()>8192) fail(tr("Invalid DAP header")); return; }
        int length=-1;
        for(const auto& line:input_.left(end).split('\n')) if(line.trimmed().toLower().startsWith("content-length:")) {
            bool ok=false; length=line.mid(line.indexOf(':')+1).trimmed().toInt(&ok);
            if(!ok) length=-1;
        }
        if(length<0 || length>limit) { fail(tr("Invalid DAP message size")); return; }
        if(input_.size()<end+4+length) return;
        QJsonParseError parse;
        const auto doc=QJsonDocument::fromJson(input_.mid(end+4,length),&parse);
        input_.remove(0,end+4+length);
        if(parse.error!=QJsonParseError::NoError || !doc.isObject()) { fail(tr("Invalid DAP JSON")); return; }
        auto obj=doc.object(); const auto type=obj.value("type").toString();
        if(type=="response") {
            auto it=pending_.find(obj.value("request_seq").toInt());
            if(it==pending_.end()) continue;
            auto callback=std::move(it->reply); const auto command=it->command; pending_.erase(it);
            if(!obj.value("success").toBool()) {
                QString message=obj.value("message").toString();
                const auto detail=obj.value("body").toObject().value("error").toObject();
                QString formatted=detail.value("format").toString(); const auto variables=detail.value("variables").toObject();
                for(auto var=variables.begin();var!=variables.end();++var) formatted.replace("{"+var.key()+"}",var.value().toString());
                if(!formatted.isEmpty()) message=formatted;
                obj["message"]=message;
                if(command=="initialize" || command=="launch" || command=="attach" || command=="configurationDone") emit fatalError(command+": "+message);
                emit error(command+": "+message);
            }
            if(callback) callback(obj);
        } else if(type=="event") emit eventReceived(obj.value("event").toString(),obj.value("body").toObject());
        else if(type=="request") {
            if(obj.value("command").toString()=="runInTerminal" && isSignalConnected(QMetaMethod::fromSignal(&DapClient::reverseRequest))) emit reverseRequest(obj);
            else respond(obj,false,{},"Unsupported client request");
        }
    }
}
