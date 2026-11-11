#include "RecoveryStore.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUuid>
#include <QCryptographicHash>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>

namespace {
QString commitObject(const QString& path,const QJsonObject& object) {
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly))return file.errorString();
    const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact);
    if(bytes.size()>128ll*1024*1024)return QStringLiteral("Recovery manifest exceeds the supported size");
    if(file.write(bytes)!=bytes.size() || !file.flush() || ::fsync(file.handle())!=0 || !file.commit())return file.errorString();
    const int fd=::open(QFile::encodeName(QFileInfo(path).absolutePath()).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return QStringLiteral("Cannot open recovery directory for synchronization");
    const bool ok=::fsync(fd)==0;::close(fd);
    return ok?QString():QStringLiteral("Cannot synchronize recovery directory");
}
}
QString RecoveryStore::rootDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/recovery";
}
bool RecoveryStore::validId(const QString& id) {
    if(id.isEmpty() || id.size()>80)return false;
    for(const auto c:id)if(!c.isLetterOrNumber() && c!='-')return false;
    return true;
}
RecoveryStore::RecoveryStore(const QString& root) {
    directory_=(root.isEmpty()?rootDirectory():root)+"/"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(!QDir().mkpath(directory_))throw std::runtime_error("Cannot create recovery directory");
    QFile::setPermissions(directory_,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
    lock_=std::make_unique<QLockFile>(directory_+"/lock");lock_->setStaleLockTime(0);
    if(!lock_->tryLock())throw std::runtime_error("Cannot lock recovery directory");
}
QJsonObject RecoveryStore::readObject(const QString& path) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly) || file.size()>128ll*1024*1024)return {};
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(file.readAll(),&error);
    return error.error==QJsonParseError::NoError && document.isObject()?document.object():QJsonObject{};
}
QString RecoveryStore::writeSession(QJsonObject session) {
    std::lock_guard guard(mutex_);
    session["format"]=1;
    return commitObject(directory_+"/session.json",session);
}
QString RecoveryStore::writeDocument(const QString& id,const PieceTreeBuffer& snapshot,QJsonObject metadata) {
    std::lock_guard guard(mutex_);
    if(!validId(id))return "Invalid recovery identifier";
    QFile data(directory_+"/"+id+".data");
    if(!data.open(QIODevice::ReadWrite))return data.errorString();
    QFile::setPermissions(data.fileName(),QFile::ReadOwner|QFile::WriteOwner);
    if(!data.seek(data.size()))return data.errorString();
    auto& known=pieces_[id];
    QHash<QString,QJsonObject> pending;
    QJsonArray entries;
    for(const auto& piece:snapshot.recoveryPieces()) {
        const auto key=QString::number(piece.source)+':'+QString::number(piece.start)+':'+QString::number(piece.length);
        auto found=known.constFind(key);
        QJsonObject entry;
        if(found!=known.cend())entry=found.value();
        else {
            const auto bytes=snapshot.slice(piece.position,piece.length);
            if(bytes.size()!=piece.length)return "Incomplete recovery source";
            const auto start=data.pos();
            if(data.write(bytes.data(),qint64(bytes.size()))!=qint64(bytes.size()))return data.errorString();
            entry={{"offset",QString::number(start)},{"length",int(piece.length)},
                   {"sha256",QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromRawData(bytes.data(),int(bytes.size())),QCryptographicHash::Sha256).toHex())}};
            pending.insert(key,entry);
        }
        entries.append(entry);
    }
    // Flush new payload before publishing references to it. A torn tail remains unreferenced.
    if(!data.flush() || ::fsync(data.handle())!=0) { known.clear();return "Cannot synchronize recovery data"; }
    for(auto it=pending.cbegin();it!=pending.cend();++it)known.insert(it.key(),it.value());
    metadata["format"]=1;metadata["pieces"]=entries;
    metadata["bytes"]=QString::number(snapshot.size());
    return commitObject(directory_+"/"+id+".json",metadata);
}
bool RecoveryStore::restoreDocument(const QString& directory,const QString& id,PieceTreeBuffer& buffer,
                                    QJsonObject& metadata,QString& error) {
    if(!validId(id)){error="Invalid recovery identifier";return false;}
    metadata=readObject(directory+"/"+id+".json");
    if(metadata["format"].toInt()!=1 || !metadata["pieces"].isArray()) {error="Invalid recovery manifest";return false;}
    QFile data(directory+"/"+id+".data");
    if(!data.open(QIODevice::ReadOnly)){error=data.errorString();return false;}
    QTemporaryFile restored(directory+"/restore-XXXXXX");
    if(!restored.open()){error=restored.errorString();return false;}
    qint64 total=0;
    for(const auto value:metadata["pieces"].toArray()) {
        const auto entry=value.toObject();bool ok=false;
        const auto offset=entry["offset"].toString().toLongLong(&ok);
        const auto length=entry["length"].toInt(-1);
        if(!ok || offset<0 || length<=0 || length>4096 || offset>data.size()-length || !data.seek(offset)) {error="Invalid recovery range";return false;}
        const auto bytes=data.read(length);
        if(bytes.size()!=length || QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=entry["sha256"].toString()) {error="Damaged recovery data";return false;}
        if(restored.write(bytes)!=length){error=restored.errorString();return false;}
        total+=length;
    }
    bool ok=false;
    if(total!=metadata["bytes"].toString().toLongLong(&ok) || !ok){error="Incomplete recovery document";return false;}
    if(!restored.flush()){error=restored.errorString();return false;}
    std::string problem;
    if(!buffer.loadFile(QFile::encodeName(restored.fileName()).toStdString(),&problem)){error=QString::fromStdString(problem);return false;}
    return true;
}

void RecoveryStore::forgetDocument(const QString& id) {
    if(!validId(id))return;
    std::lock_guard guard(mutex_);
    QFile::remove(directory_+"/"+id+".json");
    QFile::remove(directory_+"/"+id+".data");
    pieces_.remove(id);
}

QString RecoveryStore::retireDocument(const QString& directory,const QString& id) {
    if(!validId(id))return "Invalid recovery identifier";
    auto state=readObject(directory+"/session.json");
    if(state["format"].toInt()!=1 || !state["documents"].isArray()) {
        // The initial checkpoint may have survived without a session manifest.
        for(const auto& suffix:{QStringLiteral(".json"),QStringLiteral(".data")}) {
            const auto path=directory+"/"+id+suffix;
            if(QFileInfo::exists(path) && !QFile::remove(path))return "Cannot remove discarded recovery data";
        }
        return {};
    }
    QJsonArray documents;
    for(const auto value:state["documents"].toArray())if(value.toObject()["id"].toString()!=id)documents.append(value);
    state["documents"]=documents;
    const auto error=commitObject(directory+"/session.json",state);
    if(!error.isEmpty())return error;
    QFile::remove(directory+"/"+id+".json");QFile::remove(directory+"/"+id+".data");
    return {};
}
