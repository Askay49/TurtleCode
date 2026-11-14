#include "DirectoryListing.hpp"
#include <QDirIterator>
#include <QFile>
#include <sqlite3.h>
#include <stdexcept>
namespace {
struct Db {
    sqlite3* value{};
    explicit Db(const QString& path){if(sqlite3_open(QFile::encodeName(path).constData(),&value)!=SQLITE_OK){const auto error=value?QString::fromUtf8(sqlite3_errmsg(value)):QStringLiteral("Cannot open directory listing");if(value)sqlite3_close(value);throw std::runtime_error(error.toStdString());}}
    ~Db(){sqlite3_close(value);}
    void exec(const char* sql){if(sqlite3_exec(value,sql,nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(value));}
};
struct Stmt {
    sqlite3_stmt* value{};
    Stmt(Db& db,const char* sql){if(sqlite3_prepare_v2(db.value,sql,-1,&value,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db.value));}
    ~Stmt(){sqlite3_finalize(value);}
    void bind(int index,const QString& string){const auto bytes=string.toUtf8();sqlite3_bind_text(value,index,bytes.constData(),bytes.size(),SQLITE_TRANSIENT);}
};
}
std::shared_ptr<DirectoryListing> DirectoryListing::scan(const QString& root,const QString& path,WorkspaceFileOptions options,const std::shared_ptr<std::atomic_bool>& cancel) {
    auto listing=std::make_shared<DirectoryListing>();if(!listing->storage_.isValid())throw std::runtime_error("Cannot create directory listing");
    Db db(listing->storage_.filePath("entries.sqlite"));db.exec("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA cache_size=-2048; PRAGMA temp_store=FILE; CREATE TABLE entries(name TEXT,path TEXT,directory INTEGER,sort TEXT); BEGIN;");
    if(cancel)sqlite3_progress_handler(db.value,1000,[](void* value){return static_cast<std::atomic_bool*>(value)->load()?1:0;},cancel.get());
    WorkspaceFiles filter(root,std::move(options));QDirIterator it(path,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System|QDir::NoSymLinks);
    Stmt add(db,"INSERT INTO entries VALUES(?,?,?,?)");
    while(it.hasNext()) {
        if(cancel && cancel->load())throw std::runtime_error("Directory listing cancelled");
        it.next();const auto info=it.fileInfo();if(!filter.accepts(info))continue;
        sqlite3_reset(add.value);add.bind(1,info.fileName());add.bind(2,info.absoluteFilePath());sqlite3_bind_int(add.value,3,info.isDir());add.bind(4,info.fileName().toCaseFolded());
        if(sqlite3_step(add.value)!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db.value));++listing->count_;
    }
    db.exec("COMMIT; CREATE INDEX ordered ON entries(directory DESC,sort,path);");return listing;
}
QList<DirectoryEntry> DirectoryListing::page(qint64 first,qint64 count) const {
    Db db(storage_.filePath("entries.sqlite"));Stmt read(db,"SELECT name,path,directory FROM entries ORDER BY directory DESC,sort,path LIMIT ? OFFSET ?");
    sqlite3_bind_int64(read.value,1,count);sqlite3_bind_int64(read.value,2,first);QList<DirectoryEntry> entries;
    for(;;){const auto code=sqlite3_step(read.value);if(code==SQLITE_DONE)break;if(code!=SQLITE_ROW)throw std::runtime_error(sqlite3_errmsg(db.value));entries.push_back({QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(read.value,0))),QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(read.value,1))),bool(sqlite3_column_int(read.value,2))});}return entries;
}
