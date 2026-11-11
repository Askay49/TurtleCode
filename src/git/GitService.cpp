#include "GitService.hpp"
#include "GitBackend.hpp"
#include <QFutureWatcher>
#include <QtConcurrent>
#include <exception>
#include <QDir>
#include <QSet>
#include <QFileInfo>
#include <QFile>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <memory>

namespace {
// Workers capture values only. Deleting the service disconnects delivery while
// allowing an in-flight read to release its own repository safely.
template<class T> struct ReadResult { T value{}; QString error; };
template<class Work, class Done> void readAsync(QObject* owner, Work work, Done done) {
    using T=decltype(work());
    auto* watcher=new QFutureWatcher<ReadResult<T>>(owner);
    QObject::connect(watcher,&QFutureWatcher<ReadResult<T>>::finished,owner,[watcher,done] {
        auto result=watcher->result(); watcher->deleteLater(); done(std::move(result));
    });
    watcher->setFuture(QtConcurrent::run([work] {
        ReadResult<T> result;
        try { result.value=work(); }
        catch(const std::exception& error) { result.error=QString::fromUtf8(error.what()); }
        return result;
    }));
}
}
bool GitEntry::conflict() const {
    QByteArray xy; xy += index; xy += worktree;
    return xy == "DD" || xy == "AU" || xy == "UD" || xy == "UA" || xy == "DU" || xy == "AA" || xy == "UU";
}
QStringList GitEntry::paths() const {
    QStringList result{path};
    if ((index == 'R' || worktree == 'R') && !originalPath.isEmpty() && originalPath != path) result.append(originalPath);
    return result;
}
bool GitService::parseStatus(const QByteArray& bytes, GitStatus& status) {
    status = {};
    const auto records = bytes.split('\0');
    if (records.isEmpty() || !records.front().startsWith("## ")) return false;
    const auto branch = records.front().mid(3);
    status.unborn = branch.startsWith("No commits yet on ") || branch.startsWith("Initial commit on ");
    status.branch = QString::fromUtf8(branch);
    if (status.unborn) {
        status.currentBranch = status.branch.mid(18);
    } else if (!branch.startsWith("HEAD ")) {
        status.currentBranch = status.branch.section("...", 0, 0).section(' ', 0, 0);
    }
    for (qsizetype i = 1; i < records.size(); ++i) {
        const auto& record = records[i];
        if (record.isEmpty() && i == records.size() - 1) continue;
        if (record.size() < 4 || record[2] != ' ') return false;
        GitEntry entry{QFile::decodeName(record.mid(3)), {}, record[0], record[1]};
        if (entry.index == 'R' || entry.index == 'C' || entry.worktree == 'R' || entry.worktree == 'C') {
            if (++i >= records.size() || records[i].isEmpty()) return false;
            entry.originalPath = QFile::decodeName(records[i]);
        }
        status.entries.append(entry);
    }
    status.valid = true;
    return true;
}
GitService::GitService(QObject* parent) : QObject(parent) {}
GitService::~GitService() {
    if (process_) { process_->disconnect(this); process_->kill(); process_->waitForFinished(1000); }
}
void GitService::run(const QString& directory, const QStringList& args, Completion completion, const QByteArray& input) {
    auto* process = new QProcess(this);
    process_ = process;
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto* key : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR", "GIT_EXTERNAL_DIFF", "GIT_DIFF_OPTS"}) env.remove(key);
    env.insert("LC_ALL", "C");
    env.insert("GIT_TERMINAL_PROMPT", "0");
    env.insert("GIT_LITERAL_PATHSPECS", "1");
    env.insert("GIT_OPTIONAL_LOCKS", "0");
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(directory);
    process->setProgram(QStandardPaths::findExecutable("git"));
    process->setArguments(QStringList{"--no-pager", "-c", "color.ui=false", "-c", "core.quotepath=false"} + args);
    struct Output { QByteArray out, err; bool limited{false}, done{false}, timedOut{false}; };
    auto output = std::make_shared<Output>();
    auto drain = [process, output] {
        const auto out = process->readAllStandardOutput();
        const auto err = process->readAllStandardError();
        constexpr qsizetype limit = 8 * 1024 * 1024;
        if (output->out.size() + out.size() > limit || output->err.size() + err.size() > limit) {
            output->limited = true; process->kill();
        } else { output->out += out; output->err += err; }
    };
    connect(process, &QProcess::readyReadStandardOutput, this, drain);
    connect(process, &QProcess::readyReadStandardError, this, drain);
    auto* timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [process, output] { output->timedOut = true; process->kill(); });
    auto done = [this, process, output, drain, completion, timer](int code) {
        if (output->done) return;
        output->done = true;
        timer->stop(); drain();
        QString message = QString::fromUtf8(output->err).trimmed();
        if (output->limited) { code = -1; message = "Git output exceeds 8 MiB."; }
        else if (output->timedOut) { code = -1; message = "Git operation timed out. Check repository state before retrying."; }
        else if (process->error() == QProcess::FailedToStart) { code = -1; message = process->errorString(); }
        else if (process->exitStatus() == QProcess::CrashExit) { code = -1; message = process->errorString(); }
        if (code != 0 && message.isEmpty()) message = QString::fromUtf8(output->out).trimmed();
        if (process_ == process) process_ = nullptr;
        process->deleteLater();
        completion(code, output->out, message);
    };
    connect(process, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this, [done](int code, QProcess::ExitStatus) { done(code); });
    connect(process, &QProcess::errorOccurred, this, [done](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) done(-1); });
    connect(process, &QProcess::started, this, [process, input] { if (!input.isEmpty()) process->write(input); process->closeWriteChannel(); });
    process->start();
    timer->start(300000);
}
bool GitService::begin() {
    if (busy_) return false;
    busy_ = true; emit busyChanged(true); return true;
}
void GitService::finish() {
    busy_ = false; emit busyChanged(false);
    if (pendingRefresh_) { pendingRefresh_ = false; QTimer::singleShot(0, this, &GitService::refresh); }
}
void GitService::setRoots(const QStringList& roots) {
    roots_ = roots; ++generation_; rediscover_ = true;
    repository_.clear(); repositories_.clear(); statuses_.clear(); status_ = {}; emit changed(); refresh();
}
void GitService::switchBranch(const QString& branch) {
    if (busy_) return;
    if (repository_.isEmpty() || !status_.branches.contains(branch)) {
        emit error("The selected local branch is no longer available.");
        emit branchSwitchFinished(repository_, false); return;
    }
    if (branch == status_.currentBranch) { emit branchSwitchFinished(repository_, true); return; }
    writing_ = true; begin();
    const auto generation = generation_; const auto repository = repository_;
    run(repository, {"switch", "--no-guess", "--", branch}, [this, generation, repository](int code, const QByteArray&, const QString& message) {
        writing_ = false;
        if (code != 0) emit error(message);
        emit branchSwitchFinished(repository, code == 0);
        if (generation == generation_) readStatus(generation); else finish();
    });
}
void GitService::refresh() {
    if (!begin()) { pendingRefresh_ = true; return; }
    if (rediscover_) { rediscover_ = false; discover(generation_); }
    else if (!repository_.isEmpty()) readStatus(generation_);
    else finish();
}
const GitStatus& GitService::status(const QString& repository) const {
    const auto it = statuses_.constFind(repository);
    static const GitStatus empty;
    return it == statuses_.cend() ? empty : it.value();
}
bool GitService::selectRepository(const QString& repository) {
    if (repository == repository_) return true;
    if (busy_ || !repositories_.contains(repository)) return false;
    repository_ = repository;
    status_ = statuses_.value(repository);
    return true;
}
void GitService::rescan() { rediscover_ = true; refresh(); }
void GitService::discard(const GitEntry& entry) {
    if(busy_ || repository_.isEmpty() || entry.untracked() || entry.conflict() || entry.index=='D' || entry.worktree=='R') return;
    writing_=true; begin(); const auto generation=generation_; const auto repository=repository_;
    run(repository,QStringList{"restore","--worktree","--"}+entry.paths(),[this,generation,repository](int code,const QByteArray&,const QString& message) {
        writing_=false;
        if(code!=0) emit error(message);
        emit branchSwitchFinished(repository,code==0);
        if(generation==generation_) readStatus(generation); else finish();
    });
}
void GitService::fetch() { mutate({"fetch", "--all", "--prune"}); }
void GitService::pull() {
    if (busy_ || repository_.isEmpty()) return;
    writing_ = true; begin();
    const auto generation = generation_; const auto repository = repository_;
    run(repository, {"pull", "--ff-only"}, [this,generation,repository](int code,const QByteArray&,const QString& message) {
        writing_ = false;
        if (code != 0) emit error(message);
        emit branchSwitchFinished(repository, code == 0);
        if (generation == generation_) readStatus(generation); else finish();
    });
}
void GitService::push() { mutate({"push"}); }
void GitService::discover(quint64 generation) {
    const auto roots = roots_;
    readAsync(this,[roots] {
        QStringList repositories;
        QSet<QString> visited;
        int remaining = 12000;
        const QSet<QString> skip{".git", ".cache", "node_modules", "build", "dist", ".venv", "__pycache__"};
        std::function<void(const QString&,int)> scan = [&](const QString& path, int depth) {
            if (depth > 8 || --remaining < 0) return;
            const auto canonical = QFileInfo(path).canonicalFilePath();
            if (canonical.isEmpty() || visited.contains(canonical)) return;
            visited.insert(canonical);
            if (depth == 0 || QFileInfo::exists(path + "/.git")) {
                const auto repository = GitBackend::discover(path);
                if (!repository.isEmpty() && !repositories.contains(repository)) repositories.append(repository);
            }
            const auto children = QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
            for (const auto& child : children) if (!skip.contains(child.fileName())) scan(child.absoluteFilePath(), depth + 1);
        };
        for (const auto& root : roots) scan(root, 0);
        return repositories;
    },[this,generation](auto result) {
        if(generation==generation_) {
            if(!result.error.isEmpty()) emit error(result.error);
            else {
                repositories_ = result.value;
                if (!repositories_.contains(repository_)) repository_ = repositories_.value(0);
                if (!repository_.isEmpty()) { readStatus(generation); return; }
                statuses_.clear(); status_ = {}; emit changed();
            }
        }
        finish();
    });
}
void GitService::readStatus(quint64 generation) {
    const auto repositories = repositories_;
    readAsync(this,[repositories] {
        QHash<QString, GitStatus> statuses;
        for (const auto& repository : repositories) {
            // A removed/broken nested repository must not hide the others.
            try { statuses.insert(repository, GitBackend::status(repository)); }
            catch (const std::exception&) { statuses.insert(repository, GitStatus{}); }
        }
        return statuses;
    },[this,generation](auto result) {
        if(generation==generation_) {
            if(!result.error.isEmpty()) { status_={}; emit changed(); emit error(result.error); }
            else {
                const bool changedStatus = statuses_ != result.value;
                statuses_ = std::move(result.value);
                status_ = statuses_.value(repository_);
                if (changedStatus) emit changed();
            }
        }
        finish();
    });
}
void GitService::mutate(const QStringList& args, const QByteArray& input, bool isCommit) {
    if (repository_.isEmpty() || busy_) return;
    writing_ = true;
    begin();
    const auto generation = generation_;
    run(repository_, args, [this, generation, isCommit](int code, const QByteArray&, const QString& message) {
        writing_ = false;
        if (code != 0) {
            if (isCommit) emit commitFailed(message);
            else emit error(message);
        }
        if (generation == generation_) {
            if (code == 0 && isCommit) emit committed();
            readStatus(generation);
        } else finish();
    }, input);
}
void GitService::stage(const QStringList& paths) {
    if (!paths.isEmpty()) mutate(QStringList{"add", "--all", "--"} + paths);
}
void GitService::unstage(const QStringList& paths) {
    if (paths.isEmpty()) return;
    if(busy_) {
        if(!writing_) { const auto generation=generation_; QTimer::singleShot(30,this,[this,generation,paths] { if(generation==generation_) unstage(paths); }); }
        return;
    }
    mutate((status_.unborn ? QStringList{"rm", "--cached", "-r", "--ignore-unmatch", "--"}
                          : QStringList{"reset", "-q", "HEAD", "--"}) + paths);
}
void GitService::commit(const QString& message) {
    if (message.trimmed().isEmpty()) return;
    bool staged = false;
    for (const auto& entry : status_.entries) { if (entry.conflict()) return; staged |= entry.staged(); }
    if (staged) mutate({"commit", "--file=-"}, message.toUtf8(), true);
}
void GitService::diff(const GitEntry& entry, bool staged) {
    if(repository_.isEmpty() || !begin()) return;
    const auto generation=generation_;
    GitDiffSnapshot snapshot; snapshot.repository=repository_; snapshot.entry=entry; snapshot.staged=staged;
    readAsync(this,[snapshot] { return GitBackend::versions(snapshot); },[this,generation](auto result) {
        if(generation==generation_) {
            if(!result.error.isEmpty()) emit error(result.error);
            else {
                const auto& snapshot=result.value;
                emit diffReady(snapshot.entry.path,snapshot.staged,snapshot.patch);
                if(generation==generation_) emit versionsReady(snapshot);
            }
        }
        finish();
    });
}

void GitService::applySelection(const QString& repository,const QString& path,
                               const QByteArray& expectedIndex,const QByteArray& patch,bool indexExists) {
    if(repository!=repository_ || patch.isEmpty()) return;
    if(busy_) {
        if(!writing_) {
            const auto generation=generation_;
            QTimer::singleShot(30,this,[this,generation,repository,path,expectedIndex,patch,indexExists] {
                if(generation==generation_) applySelection(repository,path,expectedIndex,patch,indexExists);
            });
        }
        return;
    }
    // Rename metadata and conflict stages use the whole-file actions in GitPanel.
    for(const auto& e:status_.entries) if(e.path==path && (e.conflict() || e.worktree=='R')) return;
    writing_=true; begin(); const auto generation=generation_;
    run(repository_,indexExists?QStringList{"show",":"+path}:QStringList{"ls-files","--stage","-z","--",path},[this,generation,expectedIndex,patch,indexExists](int code,const QByteArray& out,const QString& message) {
        if(generation!=generation_ || code!=0 || (indexExists?out!=expectedIndex:!out.isEmpty())) {
            writing_=false;
            if(generation==generation_) emit error(code!=0?message:"The index changed. Refresh the diff and retry.");
            finish(); return;
        }
        run(repository_,{"apply","--cached","--whitespace=nowarn","-"},[this,generation](int code,const QByteArray&,const QString& message) {
            writing_=false; if(code!=0) emit error(message);
            if(generation==generation_) readStatus(generation); else finish();
        },patch);
    });
}
