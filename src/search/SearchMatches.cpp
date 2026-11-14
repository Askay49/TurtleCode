#include "SearchMatches.hpp"
#include <QTemporaryDir>
#include <QDataStream>
#include <QMutex>
#include <QMutexLocker>
#include <QCache>
#include <QFile>
#include <sqlite3.h>
#include <stdexcept>
struct SearchMatches::Storage {
    QTemporaryDir directory;
    sqlite3* db{};sqlite3_stmt *read{},*append{},*erase{};
    QMutex mutex;
    qsizetype count{};
    QCache<qsizetype,ProjectSearchMatch> cache{8*1024*1024};
    Storage(){
        try {
        if(!directory.isValid())throw std::runtime_error("Cannot create search result storage");
        if(sqlite3_open(QFile::encodeName(directory.filePath("matches.sqlite")).constData(),&db)!=SQLITE_OK)fail();
        if(sqlite3_exec(db,"PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA cache_size=-2048; CREATE TABLE matches(id INTEGER PRIMARY KEY,data BLOB,path TEXT,offset INTEGER); CREATE INDEX sorted ON matches(path,offset,id); CREATE TABLE groups(first INTEGER PRIMARY KEY,count INTEGER); BEGIN",nullptr,nullptr,nullptr)!=SQLITE_OK)fail();
        if(sqlite3_prepare_v2(db,"SELECT data FROM matches WHERE id=?",-1,&read,nullptr)!=SQLITE_OK ||
           sqlite3_prepare_v2(db,"INSERT INTO matches VALUES(?,?,?,?)",-1,&append,nullptr)!=SQLITE_OK ||
           sqlite3_prepare_v2(db,"DELETE FROM matches WHERE id>=?",-1,&erase,nullptr)!=SQLITE_OK)fail();
        }catch(...){cleanup();throw;}
    }
    [[noreturn]] void fail(){throw std::runtime_error(db?sqlite3_errmsg(db):"Cannot open result storage");}
    void cleanup(){sqlite3_finalize(read);sqlite3_finalize(append);sqlite3_finalize(erase);if(db)sqlite3_close(db);}
    ~Storage(){cleanup();}
};
void SearchMatches::push_back(const ProjectSearchMatch& v) {
    if(!storage_)storage_=std::make_shared<Storage>();
    QMutexLocker lock(&storage_->mutex);
    if(size_!=storage_->count)throw std::runtime_error("Cannot append to a search result snapshot");
    QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);stream.setVersion(QDataStream::Qt_6_0);
    stream<<v.path<<v.relativePath<<v.line<<v.column<<v.length<<v.preview<<v.byteOffset<<v.byteLength<<v.matchedText<<v.captures;
    auto* stmt=storage_->append;sqlite3_reset(stmt);sqlite3_bind_int64(stmt,1,size_);sqlite3_bind_blob(stmt,2,bytes.constData(),bytes.size(),SQLITE_TRANSIENT);
    const auto path=v.path.toUtf8();sqlite3_bind_text(stmt,3,path.constData(),path.size(),SQLITE_TRANSIENT);sqlite3_bind_int64(stmt,4,v.byteOffset);
    if(sqlite3_step(stmt)!=SQLITE_DONE){sqlite3_reset(stmt);storage_->fail();}
    sqlite3_reset(stmt);sqlite3_clear_bindings(stmt);++size_;++storage_->count;
}
ProjectSearchMatch SearchMatches::operator[](qsizetype index) const {
    if(index<0 || index>=size_ || !storage_)throw std::out_of_range("Search result index");
    QMutexLocker lock(&storage_->mutex);
    if(const auto* cached=storage_->cache.object(index))return *cached;
    auto* stmt=storage_->read;sqlite3_reset(stmt);sqlite3_bind_int64(stmt,1,index);
    if(sqlite3_step(stmt)!=SQLITE_ROW){sqlite3_reset(stmt);storage_->fail();}
    QByteArray bytes(static_cast<const char*>(sqlite3_column_blob(stmt,0)),sqlite3_column_bytes(stmt,0));sqlite3_reset(stmt);
    QDataStream stream(bytes);stream.setVersion(QDataStream::Qt_6_0);ProjectSearchMatch v;
    stream>>v.path>>v.relativePath>>v.line>>v.column>>v.length>>v.preview>>v.byteOffset>>v.byteLength>>v.matchedText>>v.captures;
    if(stream.status()!=QDataStream::Ok)throw std::runtime_error("Invalid search result storage");
    if(bytes.size()<8*1024*1024)storage_->cache.insert(index,new ProjectSearchMatch(v),int(std::max<qsizetype>(1,bytes.size())));
    return v;
}
void SearchMatches::truncate(qsizetype count) {
    if(!storage_ || count>=size_)return;
    QMutexLocker lock(&storage_->mutex);
    auto* stmt=storage_->erase;sqlite3_reset(stmt);sqlite3_bind_int64(stmt,1,count);
    if(sqlite3_step(stmt)!=SQLITE_DONE){sqlite3_reset(stmt);storage_->fail();}sqlite3_reset(stmt);
    storage_->cache.clear();storage_->count=count;size_=count;
}

void SearchMatches::sealFile(qsizetype first) {
    if(first>=size_ || !storage_)return;
    QMutexLocker lock(&storage_->mutex);sqlite3_stmt* stmt{};
    if(sqlite3_prepare_v2(storage_->db,"INSERT OR REPLACE INTO groups VALUES(?,?)",-1,&stmt,nullptr)!=SQLITE_OK)storage_->fail();
    sqlite3_bind_int64(stmt,1,first);sqlite3_bind_int64(stmt,2,size_-first);
    const auto code=sqlite3_step(stmt);sqlite3_finalize(stmt);if(code!=SQLITE_DONE)storage_->fail();
    // Published files must survive rollback of a later file (e.g. a full temp disk).
    if(sqlite3_exec(storage_->db,"COMMIT; BEGIN",nullptr,nullptr,nullptr)!=SQLITE_OK)storage_->fail();
}
qsizetype SearchMatches::runLength(qsizetype first) const {
    if(!storage_ || first>=size_)return 0;
    {QMutexLocker lock(&storage_->mutex);sqlite3_stmt* stmt{};
    if(sqlite3_prepare_v2(storage_->db,"SELECT count FROM groups WHERE first=?",-1,&stmt,nullptr)!=SQLITE_OK)storage_->fail();
    sqlite3_bind_int64(stmt,1,first);qsizetype count=sqlite3_step(stmt)==SQLITE_ROW?sqlite3_column_int64(stmt,0):0;sqlite3_finalize(stmt);
    if(count)return std::min(count,size_-first);}
    const auto path=(*this)[first].path;qsizetype count=1;
    while(first+count<size_ && (*this)[first+count].path==path)++count;
    return count;
}

struct SearchMatches::Cursor::Impl {
    std::shared_ptr<Storage> storage;
    sqlite3_stmt* stmt{};
    ~Impl(){if(stmt){QMutexLocker lock(&storage->mutex);sqlite3_finalize(stmt);}}
};
SearchMatches::Cursor SearchMatches::ordered() const {
    Cursor cursor;if(!storage_)return cursor;
    cursor.impl_=std::make_shared<Cursor::Impl>();cursor.impl_->storage=storage_;
    QMutexLocker lock(&storage_->mutex);
    if(sqlite3_prepare_v2(storage_->db,"SELECT id,data FROM matches WHERE id<? ORDER BY path,offset,id",-1,&cursor.impl_->stmt,nullptr)!=SQLITE_OK)storage_->fail();
    sqlite3_bind_int64(cursor.impl_->stmt,1,size_);return cursor;
}
bool SearchMatches::Cursor::next(qsizetype& index,ProjectSearchMatch& v) {
    if(!impl_)return false;QMutexLocker lock(&impl_->storage->mutex);
    const auto code=sqlite3_step(impl_->stmt);if(code==SQLITE_DONE)return false;
    if(code!=SQLITE_ROW)impl_->storage->fail();
    index=sqlite3_column_int64(impl_->stmt,0);
    const QByteArray bytes(static_cast<const char*>(sqlite3_column_blob(impl_->stmt,1)),sqlite3_column_bytes(impl_->stmt,1));
    QDataStream stream(bytes);stream.setVersion(QDataStream::Qt_6_0);
    stream>>v.path>>v.relativePath>>v.line>>v.column>>v.length>>v.preview>>v.byteOffset>>v.byteLength>>v.matchedText>>v.captures;
    if(stream.status()!=QDataStream::Ok)throw std::runtime_error("Invalid search result storage");return true;
}

struct SearchHashes::Storage {
    QTemporaryDir directory;sqlite3* db{};QMutex mutex;
    Storage(){
        if(!directory.isValid())throw std::runtime_error("Cannot create search hash storage");
        if(sqlite3_open(QFile::encodeName(directory.filePath("hashes.sqlite")).constData(),&db)!=SQLITE_OK){const auto error=db?QString::fromUtf8(sqlite3_errmsg(db)):QStringLiteral("Cannot open search hashes");if(db)sqlite3_close(db);db=nullptr;throw std::runtime_error(error.toStdString());}
        if(sqlite3_exec(db,"PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA cache_size=-1024; CREATE TABLE hashes(path TEXT PRIMARY KEY,hash BLOB); BEGIN",nullptr,nullptr,nullptr)!=SQLITE_OK){const auto error=QString::fromUtf8(sqlite3_errmsg(db));sqlite3_close(db);db=nullptr;throw std::runtime_error(error.toStdString());}
    }
    ~Storage(){if(db)sqlite3_close(db);}
};
void SearchHashes::insert(const QString& path,const QByteArray& hash) {
    if(!storage_)storage_=std::make_shared<Storage>();QMutexLocker lock(&storage_->mutex);sqlite3_stmt* stmt{};
    if(sqlite3_prepare_v2(storage_->db,"INSERT OR REPLACE INTO hashes VALUES(?,?)",-1,&stmt,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(storage_->db));
    const auto bytes=path.toUtf8();sqlite3_bind_text(stmt,1,bytes.constData(),bytes.size(),SQLITE_TRANSIENT);sqlite3_bind_blob(stmt,2,hash.constData(),hash.size(),SQLITE_TRANSIENT);
    const auto code=sqlite3_step(stmt);sqlite3_finalize(stmt);if(code!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(storage_->db));
}
QByteArray SearchHashes::value(const QString& path) const {
    if(!storage_)return {};QMutexLocker lock(&storage_->mutex);sqlite3_stmt* stmt{};
    if(sqlite3_prepare_v2(storage_->db,"SELECT hash FROM hashes WHERE path=?",-1,&stmt,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(storage_->db));
    const auto bytes=path.toUtf8();sqlite3_bind_text(stmt,1,bytes.constData(),bytes.size(),SQLITE_TRANSIENT);
    const auto code=sqlite3_step(stmt);QByteArray result;
    if(code==SQLITE_ROW)result=QByteArray(static_cast<const char*>(sqlite3_column_blob(stmt,0)),sqlite3_column_bytes(stmt,0));sqlite3_finalize(stmt);
    if(code!=SQLITE_ROW && code!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(storage_->db));return result;
}
bool SearchMatches::containsPath(const QString& path) const {
    if(!storage_)return false;QMutexLocker lock(&storage_->mutex);sqlite3_stmt* stmt{};
    if(sqlite3_prepare_v2(storage_->db,"SELECT 1 FROM matches WHERE path=? AND id<? LIMIT 1",-1,&stmt,nullptr)!=SQLITE_OK)storage_->fail();
    const auto bytes=path.toUtf8();sqlite3_bind_text(stmt,1,bytes.constData(),bytes.size(),SQLITE_TRANSIENT);sqlite3_bind_int64(stmt,2,size_);
    const auto code=sqlite3_step(stmt);sqlite3_finalize(stmt);if(code!=SQLITE_ROW && code!=SQLITE_DONE)storage_->fail();return code==SQLITE_ROW;
}
