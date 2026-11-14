#pragma once
#include "WorkspaceFiles.hpp"
#include <QTemporaryDir>
#include <QList>
struct DirectoryEntry { QString name,path;bool directory{}; };
class DirectoryListing {
public:
    static std::shared_ptr<DirectoryListing> scan(const QString& root,const QString& path,WorkspaceFileOptions options, const std::shared_ptr<std::atomic_bool>& cancel = {});
    QList<DirectoryEntry> page(qint64 first,qint64 count) const;
    qint64 size() const {return count_;}
private:
    QTemporaryDir storage_;
    qint64 count_{};
};
