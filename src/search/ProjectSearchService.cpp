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

namespace {
QStringList splitPatterns(const QString& value) {
    QString normalized = value;
    normalized.replace(';', ',');
    QStringList patterns;
    for (QString pattern : normalized.split(',', Qt::SkipEmptyParts)) {
        pattern = pattern.trimmed();
        if (!pattern.isEmpty()) patterns.push_back(pattern);
    }
    return patterns;
}

bool matchesPatterns(const QString& relativePath, const QStringList& patterns) {
    if (patterns.isEmpty()) return false;
    for (const QString& pattern : patterns) {
        const auto expression = QRegularExpression::wildcardToRegularExpression(
            pattern, QRegularExpression::UnanchoredWildcardConversion);
        if (QRegularExpression(expression, QRegularExpression::CaseInsensitiveOption)
                .match(relativePath).hasMatch())
            return true;
    }
    return false;
}

struct PendingDirectory {
    QString path;
};

ProjectSearchResult runSearch(const ProjectSearchOptions& options,
                              const std::shared_ptr<std::atomic_bool>& cancelled) {
    ProjectSearchResult result;
    if (options.query.isEmpty() || options.roots.isEmpty()) return result;

    QRegularExpression::PatternOptions patternOptions;
    if (!options.matchCase) patternOptions |= QRegularExpression::CaseInsensitiveOption;
    QString pattern = options.regularExpression
        ? options.query : QRegularExpression::escape(options.query);
    if (options.wholeWord)
        pattern = QStringLiteral("(?<![\\p{L}\\p{N}_])(?:%1)(?![\\p{L}\\p{N}_])").arg(pattern);
    const QRegularExpression expression(pattern, patternOptions);
    if (!expression.isValid()) {
        result.error = expression.errorString();
        return result;
    }

    const QStringList includes = splitPatterns(options.filesToInclude);
    const QStringList excludes = splitPatterns(options.filesToExclude);
    QSet<QString> visitedFiles;
    for (const QString& inputRoot : options.roots) {
        const QString root = QDir::cleanPath(QFileInfo(inputRoot).absoluteFilePath());
        QList<PendingDirectory> pending{{root}};
        while (!pending.isEmpty() && !cancelled->load(std::memory_order_relaxed)) {
            PendingDirectory current = pending.takeLast();
            QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::NoSymLinks;
            if (options.showHidden) filters |= QDir::Hidden;
            const QFileInfoList entries = QDir(current.path).entryInfoList(
                filters,
                QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
            for (const QFileInfo& info : entries) {
                if (cancelled->load(std::memory_order_relaxed)) return result;
                const QString path = info.absoluteFilePath();
                const QString relative = QDir(root).relativeFilePath(path);
                if (matchesPatterns(relative, excludes)) continue;
                if (info.isDir()) {
                    pending.push_back({path});
                    continue;
                }
                if (!info.isFile()) continue;
                if (!includes.isEmpty() && !matchesPatterns(relative, includes)) continue;
                if (visitedFiles.contains(path)) continue;
                visitedFiles.insert(path);

                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) continue;
                const QByteArray probe = file.peek(8192);
                if (probe.contains('\0')) continue;
                ++result.filesSearched;
                int lineNumber = 0;
                qint64 lineOffset = 0;
                const auto firstMatch = result.matches.size();
                QCryptographicHash hash(QCryptographicHash::Sha256);
                bool validText = true;
                while (!file.atEnd()) {
                    if (cancelled->load(std::memory_order_relaxed)) return result;
                    const QByteArray bytes = file.readLine();
                    hash.addData(bytes);
                    const QString line = QString::fromUtf8(bytes.constData(), bytes.size());
                    if (bytes.contains('\0') || line.toUtf8() != bytes) {
                        validText = false;
                        break;
                    }
                    ++lineNumber;
                    auto iterator = expression.globalMatch(line);
                    const QString preview = line.trimmed().left(180);
                    qsizetype lastColumn = 0;
                    qint64 lastByte = 0;
                    while (iterator.hasNext()) {
                        if ((result.matches.size() & 4095) == 0 &&
                            cancelled->load(std::memory_order_relaxed)) return result;
                        const auto match = iterator.next();
                        if (match.capturedLength() == 0) continue;
                        lastByte += line.mid(lastColumn, match.capturedStart() - lastColumn).toUtf8().size();
                        lastColumn = match.capturedStart();
                        ProjectSearchMatch item;
                        item.path = path;
                        item.relativePath = relative;
                        item.line = lineNumber;
                        item.column = match.capturedStart() + 1;
                        item.length = static_cast<int>(match.capturedLength());
                        item.preview = preview;
                        item.byteOffset = lineOffset + lastByte;
                        item.byteLength = match.captured().toUtf8().size();
                        item.matchedText = match.captured();
                        if (options.regularExpression) item.captures = match.capturedTexts();
                        result.matches.push_back(std::move(item));
                        lastColumn = match.capturedEnd();
                        lastByte += result.matches.back().byteLength;
                    }
                    lineOffset += bytes.size();
                }
                if (!validText || file.error() != QFileDevice::NoError) {
                    while (result.matches.size() > firstMatch) result.matches.pop_back();
                    continue;
                }
                if (result.matches.size() > firstMatch) result.fileHashes.insert(path, hash.result());
            }
        }
    }
    return result;
}
}

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
    auto* watcher = new QFutureWatcher<ProjectSearchResult>(this);
    connect(watcher, &QFutureWatcher<ProjectSearchResult>::finished, this,
            [this, watcher, generation, cancellation] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != generation_ || cancellation->load(std::memory_order_relaxed)) return;
        emit searchFinished(result);
    });
    watcher->setFuture(QtConcurrent::run([options = std::move(options), cancellation] {
        return runSearch(options, cancellation);
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
    watcher->setFuture(QtConcurrent::run([request = std::move(request), cancelled] {
        ProjectReplaceResult result;
        if (!request.selected.isEmpty() && request.selected.size() != request.matches.size()) {
            result.errors.push_back(I18n::text("Invalid selection state"));
            return result;
        }
        QMap<QString, QVector<qsizetype>> files;
        for (qsizetype index = 0; index < request.matches.size(); ++index) {
            if (!request.selected.isEmpty() && !request.selected.testBit(index)) continue;
            files[request.matches[index].path].push_back(index);
        }
        const QSet<QString> protectedPaths(request.protectedPaths.cbegin(), request.protectedPaths.cend());
        for (auto it = files.begin(); it != files.end(); ++it) {
            if (cancelled->load(std::memory_order_relaxed)) {
                result.cancelled = true;
                break;
            }
            const QString path = it.key();
            if (protectedPaths.contains(path)) {
                result.skippedProtected.push_back(path);
                continue;
            }
            const QFileInfo info(path);
            if (!info.isFile() || info.isSymLink()) {
                result.errors.push_back(path + I18n::text(": file is unavailable"));
                continue;
            }
            QFile input(path);
            if (!input.open(QIODevice::ReadOnly)) {
                result.errors.push_back(path + QStringLiteral(": ") + input.errorString());
                continue;
            }
            if (!request.fileHashes.contains(path)) {
                result.skippedChanged.push_back(path);
                continue;
            }
            auto indices = it.value();
            std::sort(indices.begin(), indices.end(), [&request](qsizetype a, qsizetype b) {
                return request.matches[a].byteOffset < request.matches[b].byteOffset;
            });
            QSaveFile destination(path);
            if (!destination.open(QIODevice::WriteOnly)) {
                result.errors.push_back(path + QStringLiteral(": ") + destination.errorString());
                continue;
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
            qint64 cursor = 0;
            for (qsizetype position = 0; position < indices.size(); ++position) {
                if (state != State::Ok) break;
                if ((position & 1023) == 0 && cancelled->load(std::memory_order_relaxed)) {
                    state = State::Cancelled;
                    break;
                }
                const auto index = indices[position];
                const auto& match = request.matches[index];
                const QByteArray expected = match.matchedText.toUtf8();
                if (match.byteOffset < cursor || match.byteLength != expected.size()) {
                    state = State::Stale;
                    break;
                }
                copyBytes(match.byteOffset - cursor);
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
                cursor = match.byteOffset + match.byteLength;
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
            result.matchesReplaced += indices.size();
            result.changedPaths.push_back(path);
        }
        return result;
    }));
}
