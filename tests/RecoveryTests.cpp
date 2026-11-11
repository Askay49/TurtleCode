#include "document/RecoveryStore.hpp"
#include "document/FileState.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QProcess>
#include <QLockFile>
#include <QDebug>
#include <cassert>
#include <cstdio>

static QByteArray bytes(const PieceTreeBuffer& buffer) {
    QByteArray result;
    assert(buffer.forEachChunk([&](std::string_view chunk){result.append(chunk.data(),qsizetype(chunk.size()));return true;}));
    return result;
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(argc==3 && QByteArray(argv[1])=="--crash-writer") {
        // Parent kills us with an active lock and a deliberately incomplete tail.
        RecoveryStore store(QString::fromLocal8Bit(argv[2]));
        PieceTreeBuffer buffer("\xef\xbb\xbfint пример = 1;\r\n");
        assert(store.writeDocument("document",buffer,{{"path","original.cpp"},{"diskState","old"}}).isEmpty());
        assert(store.writeSession({{"clean",false},{"documents",QJsonArray{QJsonObject{{"id","document"},{"modified",true}}}}}).isEmpty());
        QFile tail(store.directory()+"/document.data");assert(tail.open(QIODevice::Append));tail.write("incomplete payload");tail.flush();
        QFile ready(QString::fromLocal8Bit(argv[2])+"/ready");assert(ready.open(QIODevice::WriteOnly));ready.write(store.directory().toUtf8());ready.close();
        return app.exec();
    }
    QTemporaryDir temporary;assert(temporary.isValid());
    const auto root=temporary.filePath("recovery");
    RecoveryStore store(root);
    const auto input=temporary.filePath("large.txt");
    QByteArray expected="\xef\xbb\xbf";
    while(expected.size()<8*1024*1024)expected+="int пример = 1;\r\n";
    {QFile file(input);assert(file.open(QIODevice::WriteOnly));assert(file.write(expected)==expected.size());}
    PieceTreeBuffer buffer;assert(buffer.loadFile(input.toStdString()));assert(buffer.isFileBacked());
    QElapsedTimer timer;timer.start();
    auto checkpoint=[&] {
        const auto snapshot=buffer.snapshot(true);
        assert(store.writeDocument("large",snapshot,{{"path",input},{"diskState",QString::fromLatin1(diskFileState(input))}}).isEmpty());
        PieceTreeBuffer restored;QJsonObject metadata;QString error;
        assert(RecoveryStore::restoreDocument(store.directory(),"large",restored,metadata,error));
        assert(bytes(restored)==expected);
    };
    checkpoint();
    const auto originalBytes=QFileInfo(store.directory()+"/large.data").size();
    buffer.insert(17,"changed");expected.insert(17,"changed");checkpoint();
    const auto changedBytes=QFileInfo(store.directory()+"/large.data").size();
    assert(changedBytes-originalBytes<16*1024);
    checkpoint();assert(QFileInfo(store.directory()+"/large.data").size()==changedBytes);
    buffer.erase(4,11);expected.remove(4,11);checkpoint();
    std::fprintf(stderr,"8 MiB checkpoint/restore sequence: %lld ms; small-edit journal growth: %lld bytes\n",static_cast<long long>(timer.elapsed()),static_cast<long long>(changedBytes-originalBytes));
    // A spilling add arena must receive a new identity when its offset restarts at zero.
    PieceTreeBuffer arena("base");
    const std::string oneMiB(1024*1024-1,'a');
    for(int i=0;i<5;++i)arena.insert(arena.size(),oneMiB);
    assert(store.writeDocument("arena",arena.snapshot(true),{}).isEmpty());
    arena.insert(arena.size(),"new tail");
    assert(store.writeDocument("arena",arena.snapshot(true),{}).isEmpty());
    PieceTreeBuffer recovered;QJsonObject metadata;QString error;
    assert(RecoveryStore::restoreDocument(store.directory(),"arena",recovered,metadata,error));assert(bytes(recovered)==bytes(arena));
    assert(!RecoveryStore::validId("../escape"));
    // Unpublished data from a killed writer cannot alter the last manifest.
    const auto crashRoot=temporary.filePath("crash");assert(QDir().mkpath(crashRoot));
    QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--crash-writer",crashRoot});assert(child.waitForStarted());
    QElapsedTimer deadline;deadline.start();
    while(!QFileInfo::exists(crashRoot+"/ready") && deadline.elapsed()<10000)child.waitForReadyRead(10);
    QFile ready(crashRoot+"/ready");assert(ready.open(QIODevice::ReadOnly));const auto crashed=QString::fromUtf8(ready.readAll());
    QLockFile liveLock(crashed+"/lock");liveLock.setStaleLockTime(0);assert(!liveLock.tryLock());
    child.kill();assert(child.waitForFinished());
    QLockFile staleLock(crashed+"/lock");staleLock.setStaleLockTime(0);assert(staleLock.tryLock());
    PieceTreeBuffer afterCrash;
    assert(RecoveryStore::restoreDocument(crashed,"document",afterCrash,metadata,error));
    assert(bytes(afterCrash)==QByteArray("\xef\xbb\xbfint пример = 1;\r\n"));
    // Corruption is reported without replacing the caller's existing document.
    {QFile file(crashed+"/document.data");assert(file.open(QIODevice::ReadWrite));assert(file.write("!")==1);}
    PieceTreeBuffer untouched("keep me");
    assert(!RecoveryStore::restoreDocument(crashed,"document",untouched,metadata,error));assert(untouched.text()=="keep me");
    // A first checkpoint without a session manifest remains recoverable and can be discarded.
    assert(store.writeDocument("orphan",PieceTreeBuffer("orphan text"),{}).isEmpty());
    assert(RecoveryStore::restoreDocument(store.directory(),"orphan",untouched,metadata,error));
    assert(untouched.text()=="orphan text");
    assert(RecoveryStore::retireDocument(store.directory(),"orphan").isEmpty());
    assert(!QFileInfo::exists(store.directory()+"/orphan.json"));
    store.forgetDocument("large");assert(!QFileInfo::exists(store.directory()+"/large.data"));assert(!QFileInfo::exists(store.directory()+"/large.json"));
}
