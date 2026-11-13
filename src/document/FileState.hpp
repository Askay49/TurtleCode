#pragma once
#include <QByteArray>
#include <QFile>
#include <QString>
#include <sys/stat.h>

#ifdef __APPLE__
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#endif

// Nanosecond timestamps plus identity detect replacement, deletion and in-place edits.
inline QByteArray diskFileState(const QString& path) {
    struct stat info{};
    if(path.isEmpty() || ::stat(QFile::encodeName(path).constData(), &info)!=0) return "missing";
    return QByteArray::number(qulonglong(info.st_dev))+':'+QByteArray::number(qulonglong(info.st_ino))+':'
        +QByteArray::number(qlonglong(info.st_size))+':'+QByteArray::number(qlonglong(info.st_mtim.tv_sec))+':'
        +QByteArray::number(qlonglong(info.st_mtim.tv_nsec))+':'+QByteArray::number(qlonglong(info.st_ctim.tv_sec))+':'
        +QByteArray::number(qlonglong(info.st_ctim.tv_nsec));
}
