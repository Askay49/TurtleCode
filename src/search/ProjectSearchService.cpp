#include "SearchScanner.hpp"
#include "project/WorkspaceThreads.hpp"
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>
#include <optional>
#include "app/I18n.hpp"
#include "ProjectSearchService.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QCryptographicHash>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>


ProjectSearchService::ProjectSearchService(QObject* parent) : QObject(parent) {
    qRegisterMetaType<ProjectSearchResult>();
}

ProjectSearchService::~ProjectSearchService() { cancel(); }

void ProjectSearchService::cancel() {
    if (cancellation_) cancellation_->store(true, std::memory_order_relaxed);
    cancellation_.reset();
    ++generation_;
}

void ProjectSearchService::search(ProjectSearchOptions options) {
    cancel();
    const auto generation = generation_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    emit searchStarted();
    struct Mailbox { QMutex mutex; std::optional<ProjectSearchResult> latest; };
    auto mailbox=std::make_shared<Mailbox>();
    auto* timer=new QTimer(this);timer->setInterval(100);
    connect(timer,&QTimer::timeout,this,[this,mailbox,generation] {
        std::optional<ProjectSearchResult> value;
        {QMutexLocker lock(&mailbox->mutex);value=std::move(mailbox->latest);mailbox->latest.reset();}
        if(value && generation==generation_)emit searchProgress(*value);
    });timer->start();
    auto* watcher = new QFutureWatcher<ProjectSearchResult>(this);
    connect(watcher, &QFutureWatcher<ProjectSearchResult>::finished, this,
            [this, watcher, generation, cancellation, timer] {
        timer->stop();timer->deleteLater();
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != generation_ || cancellation->load(std::memory_order_relaxed)) return;
        emit searchFinished(result);
    });
    watcher->setFuture(QtConcurrent::run(&WorkspaceThreads::search(),[options = std::move(options), cancellation, mailbox] {
        try {return scanProject(options,cancellation,[mailbox](const ProjectSearchResult& value){QMutexLocker lock(&mailbox->mutex);mailbox->latest=value;});}
        catch(const std::exception& error){ProjectSearchResult result;result.error=QString::fromUtf8(error.what());return result;}
    }));
}

ProjectReplaceService::ProjectReplaceService(QObject* parent) : QObject(parent) {}
ProjectReplaceService::~ProjectReplaceService() { cancel(); }

void ProjectReplaceService::cancel() {
    if (cancellation_) cancellation_->store(true, std::memory_order_relaxed);
}

QString ProjectReplaceService::previewReplacement(const ProjectSearchMatch& match,
                                                  const QString& replacement,
                                                  bool regularExpression) {
    if (!regularExpression) return replacement;
    QString expanded;
    for (qsizetype i = 0; i < replacement.size(); ++i) {
        if (replacement[i] == '$' && i + 1 < replacement.size()) {
            const QChar next = replacement[i + 1];
            if (next == '$') { expanded += '$'; ++i; continue; }
            if (next.isDigit() && next.digitValue() < match.captures.size()) {
                expanded += match.captures[next.digitValue()];
                ++i;
                continue;
            }
        }
        expanded += replacement[i];
    }
    return expanded;
}

void ProjectReplaceService::replace(ProjectReplaceRequest request) {
    if (cancellation_) return;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = cancellation_;
    auto* watcher = new QFutureWatcher<ProjectReplaceResult>(this);
    connect(watcher, &QFutureWatcher<ProjectReplaceResult>::finished, this,
            [this, watcher, cancelled] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (cancellation_ == cancelled) cancellation_.reset();
        emit replaceFinished(result);
    });
    watcher->setFuture(QtConcurrent::run(&WorkspaceThreads::search(),[request = std::move(request), cancelled] {
        ProjectReplaceResult result;
        if (!request.selected.isEmpty() && request.selected.size() != request.matches.size()) {
            result.errors.push_back(I18n::text("Invalid selection state"));
            return result;
        }
        try {
        auto cursor=request.matches.ordered();qsizetype index{};ProjectSearchMatch match;
        const auto next=[&] {while(cursor.next(index,match)){if(cancelled->load())return false;if(request.selected.isEmpty() || request.selected.testBit(index))return true;}return false;};
        bool available=next();
        const QSet<QString> protectedPaths(request.protectedPaths.cbegin(), request.protectedPaths.cend());
        while(available) {
            if(cancelled->load()){result.cancelled=true;break;}
            const QString path=match.path;
            const auto skip=[&]{while(available && match.path==path)available=next();};
            if(protectedPaths.contains(path)){result.skippedProtected<<path;skip();continue;}
            const QFileInfo info(path);
            if(!info.isFile() || info.isSymLink()){result.errors<<path+I18n::text(": file is unavailable");skip();continue;}
            QFile input(path);
            if(!input.open(QIODevice::ReadOnly)){result.errors<<path+": "+input.errorString();skip();continue;}
            if(!request.fileHashes.contains(path)){result.skippedChanged<<path;skip();continue;}
            QSaveFile destination(path);
            if (!destination.open(QIODevice::WriteOnly)) {
                result.errors.push_back(path + QStringLiteral(": ") + destination.errorString());
                skip();continue;
            }
            enum class State { Ok, Cancelled, Stale, ReadError, WriteError };
            State state = State::Ok;
            QCryptographicHash hash(QCryptographicHash::Sha256);
            constexpr qint64 blockSize = 64 * 1024;
            const auto copyBytes = [&](qint64 count) {
                while (count > 0 && state == State::Ok) {
                    if (cancelled->load(std::memory_order_relaxed)) {
                        state = State::Cancelled;
                        break;
                    }
                    const QByteArray block = input.read(std::min(count, blockSize));
                    if (block.isEmpty()) {
                        state = input.error() == QFileDevice::NoError ? State::Stale : State::ReadError;
                        break;
                    }
                    hash.addData(block);
                    if (destination.write(block) != block.size()) {
                        state = State::WriteError;
                        break;
                    }
                    count -= block.size();
                }
            };
            qint64 byteCursor = 0;
            int replacedCount=0;
            while(available && match.path==path) {
                if(state!=State::Ok || cancelled->load()){state=cancelled->load()?State::Cancelled:state;break;}
                const QByteArray expected = match.matchedText.toUtf8();
                if (match.byteOffset < byteCursor || match.byteLength != expected.size()) {
                    state = State::Stale;
                    break;
                }
                copyBytes(match.byteOffset - byteCursor);
                if (state != State::Ok) break;
                const QByteArray actual = input.read(match.byteLength);
                if (actual != expected) {
                    state = State::Stale;
                    break;
                }
                hash.addData(actual);
                const QByteArray inserted = previewReplacement(
                    match, request.replacement, request.regularExpression).toUtf8();
                if (destination.write(inserted) != inserted.size()) {
                    state = State::WriteError;
                    break;
                }
                byteCursor = match.byteOffset + match.byteLength;
                ++replacedCount;available=next();
            }
            while (state == State::Ok && !input.atEnd()) {
                if (cancelled->load(std::memory_order_relaxed)) {
                    state = State::Cancelled;
                    break;
                }
                const QByteArray block = input.read(blockSize);
                if (block.isEmpty()) {
                    state = input.error() == QFileDevice::NoError ? State::Stale : State::ReadError;
                    break;
                }
                hash.addData(block);
                if (destination.write(block) != block.size()) state = State::WriteError;
            }
            if (state == State::Ok && hash.result() != request.fileHashes.value(path))
                state = State::Stale;
            if (state == State::Ok && cancelled->load(std::memory_order_relaxed))
                state = State::Cancelled;
            if (state != State::Ok) {
                destination.cancelWriting();
                if (state == State::Cancelled) { result.cancelled = true; break; }
                skip();
                if (state == State::Stale) result.skippedChanged.push_back(path);
                else result.errors.push_back(path + QStringLiteral(": ") +
                    (state == State::ReadError ? input.errorString() : destination.errorString()));
                continue;
            }
            if (!destination.commit()) {
                result.errors.push_back(path + QStringLiteral(": ") + destination.errorString());
                continue;
            }
            result.filesChanged++;
            result.matchesReplaced += replacedCount;
            result.changedPaths.push_back(path);
        }
        }catch(const std::exception& error){result.errors<<QString::fromUtf8(error.what());}
        if(cancelled->load())result.cancelled=true;
        return result;
    }));
}
