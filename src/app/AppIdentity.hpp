#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QLockFile>
#include <memory>
#include <vector>
#include <QStandardPaths>

namespace AppIdentity {
inline void setCurrent() {
    QCoreApplication::setApplicationName("Turtle Code");
    QCoreApplication::setOrganizationName("Turtle Code");
}

// The previous identity is used only to preserve user data across the rename.
inline QString migrate() {
    QCoreApplication::setApplicationName("BlinkCode");
    QCoreApplication::setOrganizationName("BlinkCode");
    const auto oldConfig = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const auto oldData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QSettings previous;
    setCurrent();
    QSettings current;
    if (current.value("migration/previousIdentity", false).toBool()) return {};
    // Detached previous-version windows may outlive their primary IPC server.
    // Check their journal locks before moving the data directory.
    std::vector<std::unique_ptr<QLockFile>> recoveryLocks;
    const QDir recovery(oldData + "/recovery");
    for (const auto& name : recovery.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        auto lock = std::make_unique<QLockFile>(recovery.filePath(name + "/lock"));
        lock->setStaleLockTime(0);
        if (!lock->tryLock())
            return QStringLiteral("Close the previous editor windows before starting Turtle Code.");
        recoveryLocks.push_back(std::move(lock));
    }
    recoveryLocks.clear();
    const auto moveDirectory = [](const QString& from, const QString& to) -> QString {
        if (!QFileInfo::exists(from) || from == to) return {};
        if (QFileInfo::exists(to))
            return QStringLiteral("Cannot migrate application data: destination already exists: %1").arg(to);
        if (!QDir().mkpath(QFileInfo(to).absolutePath()) || !QDir().rename(from, to))
            return QStringLiteral("Cannot migrate application data from %1 to %2").arg(from, to);
        return {};
    };
    auto error = moveDirectory(oldConfig, QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    if (!error.isEmpty()) return error;
    error = moveDirectory(oldData, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (!error.isEmpty()) return error;
    for (const auto& key : previous.allKeys())
        if (!current.contains(key)) current.setValue(key, previous.value(key));
    current.setValue("migration/previousIdentity", true);
    current.sync();
    return current.status() == QSettings::NoError ? QString() : QStringLiteral("Cannot save migrated application settings");
}
}
