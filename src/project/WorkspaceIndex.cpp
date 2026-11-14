#include "WorkspaceIndex.hpp"
#include "WorkspaceThreads.hpp"
#include "document/FileState.hpp"
#include <QTemporaryDir>
#include <QFutureWatcher>
#include <QPointer>
#include <QDir>
#include <QDirIterator>
#include <QSet>
#include <QtConcurrent>
#include <sqlite3.h>
#include <stdexcept>
#include <algorithm>

namespace {
class Database {
public:
    sqlite3* db{};
    explicit Database(const QString& path) { if(sqlite3_open(QFile::encodeName(path).constData(),&db)!=SQLITE_OK) {QString e=db?QString::fromUtf8(sqlite3_errmsg(db)):"Cannot open index"; if(db)sqlite3_close(db); throw std::runtime_error(e.toStdString());} sqlite3_busy_timeout(db,5000); }
    ~Database(){sqlite3_close(db);}
    void exec(const char* sql) { if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db)); }
};
class Statement {
public:
    sqlite3_stmt* stmt{};
    Statement(Database& db,const char* sql){if(sqlite3_prepare_v2(db.db,sql,-1,&stmt,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db.db));}
    ~Statement(){sqlite3_finalize(stmt);}
    void bind(int index,const QString& value){const auto b=value.toUtf8();sqlite3_bind_text(stmt,index,b.constData(),b.size(),SQLITE_TRANSIENT);}
    QString text(int col) const {return QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt,col)));}
    bool next(){const int r=sqlite3_step(stmt);if(r!=SQLITE_ROW && r!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(stmt)));return r==SQLITE_ROW;}
};
}
struct WorkspaceIndex::State {
    QTemporaryDir storage;
    QString root;
    WorkspaceFileOptions options;
    std::atomic_bool cancel{false}, ready{false}, scanning{false};
    std::atomic<quint64> progress{0};
    QString path() const {return storage.filePath("index.sqlite");}
};
WorkspaceIndex::WorkspaceIndex(QObject* parent):QObject(parent) {
    debounce_.setSingleShot(true);debounce_.setInterval(300);
    connect(&debounce_,&QTimer::timeout,this,&WorkspaceIndex::refresh);
    progress_.setInterval(500);
    connect(&progress_,&QTimer::timeout,this,[this]{if(state_ && state_->progress.load()!=deliveredProgress_){deliveredProgress_=state_->progress.load();emit updated();}});progress_.start();
    poll_.setInterval(30000);connect(&poll_,&QTimer::timeout,this,&WorkspaceIndex::refresh);poll_.start();
}
WorkspaceIndex::~WorkspaceIndex(){if(state_)state_->cancel=true;if(queryCancel_)queryCancel_->store(true);}
void WorkspaceIndex::configure(const QStringList& roots,WorkspaceFileOptions options) {
    if(state_)state_->cancel=true;
    if(queryCancel_)queryCancel_->store(true);
    setProperty("indexError",QString{});
    state_=std::make_shared<State>();state_->root=roots.value(0);state_->options=std::move(options);state_->scanning=!state_->root.isEmpty();
    invalidate();
}
void WorkspaceIndex::invalidate(){debounce_.start();}
void WorkspaceIndex::refresh() {
    if(!state_ || state_->root.isEmpty())return;
    if(busy_){pending_=true;return;}
    busy_=true;const auto state=state_;state->scanning=true;
    auto* watcher=new QFutureWatcher<QString>(this);
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,state] {
        const auto error=watcher->result();watcher->deleteLater();busy_=false;state->scanning=false;
        if(state==state_){state->ready=error.isEmpty();setProperty("indexError",error);emit updated();}
        if(pending_ || state!=state_){pending_=false;debounce_.start();}
    });
    watcher->setFuture(QtConcurrent::run(&WorkspaceThreads::indexing(),[state]()->QString {
        try {
            if(!state->storage.isValid())throw std::runtime_error("Cannot create workspace index storage");
            Database db(state->path());sqlite3_progress_handler(db.db,1000,[](void* value){return static_cast<std::atomic_bool*>(value)->load()?1:0;},&state->cancel);db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-4096; CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY,parent TEXT); CREATE INDEX IF NOT EXISTS parents ON files(parent); CREATE TABLE IF NOT EXISTS dirs(path TEXT PRIMARY KEY,stamp TEXT,ignores TEXT); CREATE TABLE IF NOT EXISTS queue(path TEXT PRIMARY KEY); DELETE FROM queue;");
            bool full=!state->ready.load();
            {Statement dirs(db,"SELECT path,stamp,ignores FROM dirs");while(dirs.next()) {
                if(state->cancel)return {};
                const auto path=dirs.text(0);const auto ignores=QString::fromLatin1(diskFileState(path+"/.gitignore")+diskFileState(path+"/.turtleignore"));
                if(ignores!=dirs.text(2)) full=true;
                if(QString::fromLatin1(diskFileState(path))!=dirs.text(1)){Statement enqueue(db,"INSERT OR IGNORE INTO queue VALUES(?)");enqueue.bind(1,path);enqueue.next();}
            }}
            db.exec("BEGIN IMMEDIATE");
            if(full){db.exec("DELETE FROM files; DELETE FROM dirs; DELETE FROM queue;");Statement enqueue(db,"INSERT INTO queue VALUES(?)");enqueue.bind(1,state->root);enqueue.next();}
            WorkspaceFiles filter(state->root,state->options);quint64 inserted=0;
            while(!state->cancel) {
                QString directory;
                {Statement next(db,"SELECT path FROM queue LIMIT 1");if(!next.next())break;directory=next.text(0);}
                {Statement remove(db,"DELETE FROM queue WHERE path=?");remove.bind(1,directory);remove.next();}
                filter.clearRuleCache();
                if(!QFileInfo(directory).isDir() || (directory!=state->root && QFileInfo(directory).isSymLink())) {
                    Statement eraseFiles(db,"DELETE FROM files WHERE path=?1 OR (path>=?1||'/' AND path<?1||'0')");eraseFiles.bind(1,directory);eraseFiles.next();
                    Statement eraseDirs(db,"DELETE FROM dirs WHERE path=?1 OR (path>=?1||'/' AND path<?1||'0')");eraseDirs.bind(1,directory);eraseDirs.next();
                    Statement eraseQueue(db,"DELETE FROM queue WHERE path=?1 OR (path>=?1||'/' AND path<?1||'0')");eraseQueue.bind(1,directory);eraseQueue.next();continue;
                }
                const QString stamp=QString::fromLatin1(diskFileState(directory));
                const auto ignores=QString::fromLatin1(diskFileState(directory+"/.gitignore")+diskFileState(directory+"/.turtleignore"));
                Statement clear(db,"DELETE FROM files WHERE parent=?");clear.bind(1,directory);clear.next();
                QDirIterator it(directory,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::NoSymLinks);
                while(it.hasNext() && !state->cancel) {
                    it.next();const auto info=it.fileInfo();if(!filter.accepts(info))continue;
                    if(info.isDir()) {
                        Statement known(db,"SELECT 1 FROM dirs WHERE path=?");known.bind(1,info.absoluteFilePath());if(!known.next()){Statement enqueue(db,"INSERT OR IGNORE INTO queue VALUES(?)");enqueue.bind(1,info.absoluteFilePath());enqueue.next();}
                    } else if(info.isFile()) {
                        Statement add(db,"INSERT OR REPLACE INTO files VALUES(?,?)");add.bind(1,info.absoluteFilePath());add.bind(2,directory);add.next();
                        if(++inserted%2048==0){db.exec("COMMIT; BEGIN IMMEDIATE");state->ready=true;++state->progress;}
                    }
                }
                Statement add(db,"INSERT OR REPLACE INTO dirs VALUES(?,?,?)");add.bind(1,directory);add.bind(2,stamp);add.bind(3,ignores);add.next();
            }
            if(state->cancel)db.exec("ROLLBACK");else db.exec("COMMIT");
        }catch(const std::exception& e){return QString::fromUtf8(e.what());}
        return {};
    }));
}
int WorkspaceIndex::score(const QString& text,const QString& query) {
    const auto candidate=text.toCaseFolded();const auto needle=query.trimmed().toCaseFolded();
    int score=0,position=0,previous=-2;
    for(const auto ch:needle){const int found=candidate.indexOf(ch,position);if(found<0)return -1;score+=10+(found==previous+1?12:0);if(found==0 || candidate[found-1]=='/' || candidate[found-1]=='_' || candidate[found-1]=='-')score+=8;score-=std::min(8,found-position);previous=found;position=found+1;}
    return std::max(0,score-int((candidate.size()-needle.size())/20));
}
void WorkspaceIndex::query(const QString& query,const QStringList& recent,const QStringList& open,QObject* receiver,std::function<void(FileQueryResult)> callback) {
    if(queryCancel_)queryCancel_->store(true);queryCancel_=std::make_shared<std::atomic_bool>(false);
    const auto cancel=queryCancel_;const auto state=state_;const auto indexError=property("indexError").toString();QPointer<QObject> guard=receiver;
    auto* watcher=new QFutureWatcher<FileQueryResult>(this);
    connect(watcher,&QFutureWatcher<FileQueryResult>::finished,this,[watcher,guard,cancel,callback=std::move(callback)] {
        const auto result=watcher->result();watcher->deleteLater();if(guard && !cancel->load())callback(result);
    });
    watcher->setFuture(QtConcurrent::run(&WorkspaceThreads::queries(),[state,cancel,query,recent,open,indexError] {
        FileQueryResult result;result.error=indexError;result.indexing=state && state->scanning;QSet<QString> seen;
        auto better=[](const IndexedFile& a,const IndexedFile& b){return a.score==b.score?a.relative<b.relative:a.score>b.score;};
        auto consider=[&](const QString& path,int rank,bool deduplicate) {
            if(deduplicate && seen.contains(path))return;
            if(deduplicate)seen.insert(path);
            QString relative=state && !state->root.isEmpty()?QDir(state->root).relativeFilePath(path):QFileInfo(path).fileName();
            if(state && !state->root.isEmpty() && (relative==".." || relative.startsWith("../")))return;
            if((!state || state->root.isEmpty()) && !open.contains(path))return;
            int value=query.isEmpty()?0:score(relative,query);if(!query.isEmpty() && value<0)return;
            if(rank>=0)value+=10000-rank*100;
            IndexedFile item{path,relative,value};
            if(result.files.size()<100){result.files.push_back(item);std::push_heap(result.files.begin(),result.files.end(),better);}
            else if(better(item,result.files.front())){std::pop_heap(result.files.begin(),result.files.end(),better);result.files.back()=item;std::push_heap(result.files.begin(),result.files.end(),better);}
        };
        for(int i=0;i<recent.size();++i)if(QFileInfo::exists(recent[i]))consider(recent[i],i,true);
        for(const auto& path:open)consider(path,-1,true);
        if(state && state->ready && !cancel->load())try {
            Database db(state->path());sqlite3_progress_handler(db.db,1000,[](void* value){return static_cast<std::atomic_bool*>(value)->load()?1:0;},cancel.get());Statement total(db,"SELECT COUNT(*) FROM files");if(total.next())result.indexedFiles=sqlite3_column_int64(total.stmt,0);Statement files(db,query.isEmpty()?"SELECT path FROM files ORDER BY path LIMIT 100":"SELECT path FROM files");
            while(!cancel->load() && files.next()){const auto path=files.text(0);if(!seen.contains(path))consider(path,-1,false);}
        }catch(const std::exception& e){result.error=QString::fromUtf8(e.what());}
        std::sort(result.files.begin(),result.files.end(),better);return result;
    }));
}
