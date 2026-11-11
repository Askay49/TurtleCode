#pragma once
#include "core/text/PieceTreeBuffer.hpp"
#include <QHash>
#include <QJsonObject>
#include <QLockFile>
#include <QString>
#include <memory>
#include <mutex>

// Each window owns a locked directory. Workers retain ownership until their final write.
// Data is append-only; a separately committed manifest publishes complete checkpoints.
class RecoveryStore final {
public:
    explicit RecoveryStore(const QString& root = {});
    QString directory() const { return directory_; }
    QString writeDocument(const QString& id, const PieceTreeBuffer& snapshot, QJsonObject metadata);
    void forgetDocument(const QString& id);
    QString writeSession(QJsonObject session);
    static QString rootDirectory();
    static QJsonObject readObject(const QString& path);
    static bool restoreDocument(const QString& directory, const QString& id,
                                PieceTreeBuffer& buffer, QJsonObject& metadata, QString& error);
    static QString retireDocument(const QString& directory, const QString& id);
    static bool validId(const QString& id);
private:
    QString directory_;
    std::unique_ptr<QLockFile> lock_;
    std::mutex mutex_;
    QHash<QString, QHash<QString,QJsonObject>> pieces_;
};
