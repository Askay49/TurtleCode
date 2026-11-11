#include "app/I18n.hpp"
#include "GrammarManager.hpp"
#include "LanguageRegistry.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QLibrary>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QHash>
#include <QSet>
#include <QDirIterator>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>
#include <algorithm>
#ifdef TURTLE_CODE_WITH_TREE_SITTER
#include <tree_sitter/api.h>
#endif

namespace {
QString rootDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/grammars";
}
}

GrammarManager::GrammarManager(QObject* parent) : QObject(parent) {}

GrammarManager::~GrammarManager() {
    for (auto* process : processes_) {
        if (process && process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    for (auto* directory : temporaryDirectories_) delete directory;
}

QString GrammarManager::grammarDirectory(const QString& id) { return rootDirectory() + "/" + id; }
QString GrammarManager::libraryPath(const QString& id) {
    if (id == "cpp") {
        const QString testOverride = qEnvironmentVariable("TURTLE_CODE_CPP_GRAMMAR_LIBRARY");
        if (!testOverride.isEmpty()) return testOverride;
    }
#ifdef Q_OS_MACOS
    const QString suffix = ".dylib";
#else
    const QString suffix = ".so";
#endif
    const QString moduleName = id == "cpp" ? QStringLiteral("tree-sitter-cpp")
                                             : QStringLiteral("tree-sitter-") + id;
    return grammarDirectory(id) + "/lib" + moduleName + suffix;
}
QString GrammarManager::queryPath(const QString& id) { return grammarDirectory(id) + "/highlights.scm"; }

bool GrammarManager::isInstalled(const QString& id) {
    const auto* language = LanguageRegistry::language(id);
    if (!language || !QFileInfo::exists(libraryPath(id))) return false;
    if (id != "cpp" && !QFileInfo::exists(queryPath(id))) return false;
    QLibrary library(libraryPath(id));
    if (!library.load()) return false;
    const auto symbol = language->parserSymbol.toUtf8();
#ifdef TURTLE_CODE_WITH_TREE_SITTER
    using LanguageFunction = const TSLanguage* (*)();
    auto languageFunction = reinterpret_cast<LanguageFunction>(library.resolve(symbol.constData()));
    bool valid = languageFunction != nullptr;
    if (valid) {
        TSParser* parser = ts_parser_new();
        valid = parser && ts_parser_set_language(parser, languageFunction());
        if (parser) ts_parser_delete(parser);
        if (valid && id != "cpp") {
            QFile queryFile(queryPath(id));
            if (!queryFile.open(QIODevice::ReadOnly)) {
                valid = false;
            } else {
                const QByteArray queryText = queryFile.readAll();
                uint32_t errorOffset = 0;
                TSQueryError errorType{};
                TSQuery* query = ts_query_new(languageFunction(), queryText.constData(),
                    static_cast<uint32_t>(queryText.size()), &errorOffset, &errorType);
                valid = query != nullptr;
                if (query) ts_query_delete(query);
            }
        }
    }
#else
    const bool valid = library.resolve(symbol.constData()) != nullptr;
#endif
    library.unload();
    return valid;
}

void GrammarManager::ensureCppInstalled() { if (LanguageRegistry::isEnabled("cpp") && !isInstalled("cpp")) install("cpp"); }

void GrammarManager::install(const QString& id) {
    const auto* language = LanguageRegistry::language(id);
    if (!language || installing_.contains(id)) return;
    if (isInstalled(id)) {
        emit statusChanged(id, language->name + I18n::text(" grammar is ready"));
        emit installationFinished(id, true);
        return;
    }
    installing_.insert(id);
    const QString branch = language->defaultBranch.isEmpty() ? QStringLiteral("master") : language->defaultBranch;
    downloadCandidate(id, branch);
}

void GrammarManager::downloadCandidate(const QString& id, const QString& ref, bool tag) {
    selectedRefs_[id] = ref;
    const auto* language = LanguageRegistry::language(id);
    emit statusChanged(id, I18n::text("Downloading ") + language->name + " (" + ref + ")…");
    const QString kind = tag ? "tags/" : "heads/";
    QNetworkRequest request(QUrl("https://codeload.github.com/" + language->repository +
        "/tar.gz/refs/" + kind + QString::fromLatin1(QUrl::toPercentEncoding(ref))));
    request.setHeader(QNetworkRequest::UserAgentHeader, "TurtleCode/0.9.8");
    request.setTransferTimeout(60000);
    auto* reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, id, ref, tag, reply] {
        const QByteArray archive = reply->readAll();
        const auto error = reply->error();
        const QString errorText = reply->errorString();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (error != QNetworkReply::NoError || archive.isEmpty()) {
            if (http == 404) {
                if (!tag && ref == "master") { downloadCandidate(id, "main"); return; }
                tryOlderVersion(id, I18n::text("Source ref not found: ") + ref); return;
            }
            finish(id, false, I18n::text("Download failed (") + ref + "): " + errorText); return;
        }
        extractAndBuild(id, archive);
    });
}

void GrammarManager::fetchTags(const QString& id, int page) {
    const auto* language = LanguageRegistry::language(id);
    QNetworkRequest request(QUrl("https://api.github.com/repos/" + language->repository +
        "/tags?per_page=100&page=" + QString::number(page)));
    request.setHeader(QNetworkRequest::UserAgentHeader, "TurtleCode/0.9.8");
    request.setTransferTimeout(30000);
    emit statusChanged(id, I18n::text("Finding compatible versions of ") + language->name + "…");
    auto* reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, id, page, reply] {
        const auto data = reply->readAll();
        const auto error = reply->error();
        const auto errorText = reply->errorString();
        reply->deleteLater();
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(data, &parseError);
        if (error != QNetworkReply::NoError || parseError.error != QJsonParseError::NoError || !doc.isArray()) {
            finish(id, false, I18n::text("Cannot list grammar versions (GitHub may be unavailable or rate limited): ") +
                errorText + I18n::text(". Previous candidate: ") + candidateErrors_.value(id)); return;
        }
        const auto tags = doc.array();
        for (const auto& value : tags) {
            const auto name = value.toObject().value("name").toString();
            if (!name.isEmpty() && !candidates_[id].contains(name)) candidates_[id].append(name);
        }
        if (tags.size() == 100) { fetchTags(id, page + 1); return; }
        auto version = [](QString ref) {
            if (ref.startsWith('v')) ref.remove(0, 1);
            return QVersionNumber::fromString(ref);
        };
        auto& refs = candidates_[id];
        std::stable_sort(refs.begin(), refs.end(), [&](const QString& a, const QString& b) {
            return QVersionNumber::compare(version(a), version(b)) > 0;
        });
        tagsLoaded_.insert(id);
        tryOlderVersion(id, candidateErrors_.value(id));
    });
}

void GrammarManager::tryOlderVersion(const QString& id, const QString& reason) {
    candidateErrors_[id] = reason;
    if (auto* temp = temporaryDirectories_.take(id)) delete temp;
    if (!tagsLoaded_.contains(id)) { fetchTags(id); return; }
    auto& refs = candidates_[id];
    if (refs.isEmpty()) {
        finish(id, false, I18n::text("No compatible tagged grammar version found. Last error: ") + reason); return;
    }
    const auto ref = refs.takeFirst();
    downloadCandidate(id, ref, true);
}

void GrammarManager::extractAndBuild(const QString& id, const QByteArray& archive) {
    auto* temp = new QTemporaryDir();
    temporaryDirectories_.insert(id, temp);
    if (!temp->isValid()) { finish(id, false, I18n::text("Could not create temporary directory")); return; }
    const QString archivePath = temp->path() + "/grammar.tar.gz";
    QFile archiveFile(archivePath);
    if (!archiveFile.open(QIODevice::WriteOnly) || archiveFile.write(archive) != archive.size()) {
        finish(id, false, I18n::text("Could not save grammar archive")); return;
    }
    archiveFile.close();
    const QString sourceRoot = temp->path() + "/source";
    if (!QDir().mkpath(sourceRoot)) { finish(id, false, I18n::text("Could not create source directory")); return; }

    auto* extract = new QProcess(this);
    processes_.insert(id, extract);
    connect(extract, &QProcess::finished, this, [this, id, sourceRoot](int exitCode, QProcess::ExitStatus exitStatus) {
        auto* process = processes_.take(id);
        if (process) process->deleteLater();
        if (exitStatus != QProcess::NormalExit || exitCode != 0) { finish(id, false, I18n::text("Could not unpack grammar (tar is required)")); return; }
        const auto* definition = LanguageRegistry::language(id);
        const QString grammarRoot = definition->sourceSubdirectory.isEmpty()
            ? sourceRoot : sourceRoot + "/" + definition->sourceSubdirectory;
        const QString parser = grammarRoot + "/src/parser.c";
        const QString scanner = grammarRoot + "/src/scanner.c";
#ifdef TURTLE_CODE_WITH_TREE_SITTER
        QFile parserFile(parser);
        if (parserFile.open(QIODevice::ReadOnly)) {
            const auto source = QString::fromUtf8(parserFile.readAll());
            const auto match = QRegularExpression("#\\s*define\\s+LANGUAGE_VERSION\\s+(\\d+)").match(source);
            if (match.hasMatch()) {
                const int abi = match.captured(1).toInt();
                if (abi > TREE_SITTER_LANGUAGE_VERSION || abi < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION) {
                    tryOlderVersion(id, I18n::text("Grammar ABI ") + QString::number(abi) +
                        I18n::text("; runtime supports ") + QString::number(TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION) +
                        "–" + QString::number(TREE_SITTER_LANGUAGE_VERSION)); return;
                }
            }
        }
#endif
        QString highlights;
        QDirIterator grammarQueryIterator(grammarRoot, QStringList{"highlights.scm"}, QDir::Files, QDirIterator::Subdirectories);
        if (grammarQueryIterator.hasNext()) {
            highlights = grammarQueryIterator.next();
        } else if (grammarRoot != sourceRoot) {
            QDirIterator repositoryQueryIterator(sourceRoot, QStringList{"highlights.scm"}, QDir::Files, QDirIterator::Subdirectories);
            if (repositoryQueryIterator.hasNext()) highlights = repositoryQueryIterator.next();
        }
        if (!QFileInfo::exists(parser)) { tryOlderVersion(id, I18n::text("Grammar source has no generated src/parser.c")); return; }
        QString compiler = QStandardPaths::findExecutable("cc");
        if (compiler.isEmpty()) compiler = QStandardPaths::findExecutable("clang");
        if (compiler.isEmpty()) compiler = QStandardPaths::findExecutable("gcc");
        if (compiler.isEmpty()) { finish(id, false, I18n::text("A C compiler (cc, clang or gcc) is required to install grammars")); return; }
        const QString installDir = GrammarManager::grammarDirectory(id);
        if (!QDir().mkpath(installDir)) { finish(id, false, I18n::text("Could not create grammar installation directory")); return; }
        const QString output = GrammarManager::libraryPath(id) + ".new";
        QStringList args{"-shared", "-fPIC", "-O2", "-I" + grammarRoot + "/src", parser};
        if (QFileInfo::exists(scanner)) args << scanner;
        args << "-o" << output;
        emit statusChanged(id, I18n::text("Building ") + definition->name + I18n::text(" parser module…"));
        auto* build = new QProcess(this);
        processes_.insert(id, build);
        connect(build, &QProcess::finished, this, [this, id, output, highlights, installDir, sourceRoot](int code, QProcess::ExitStatus status) {
            auto* process = processes_.take(id);
            const QString error = process ? QString::fromLocal8Bit(process->readAllStandardError()).trimmed() : QString{};
            if (process) process->deleteLater();
            if (status != QProcess::NormalExit || code != 0) {
                QFile::remove(output);
                tryOlderVersion(id, I18n::text("Could not compile grammar") + (error.isEmpty() ? QString{} : ": " + error.left(400)));
                return;
            }
            if (id != "cpp") {
                if (!QFileInfo::exists(highlights)) {
                    QFile::remove(output);
                    tryOlderVersion(id, I18n::text("Grammar has no highlights.scm"));
                    return;
                }
                QFile querySource(highlights);
                QFile queryTarget(installDir + "/highlights.scm");
                if (!querySource.open(QIODevice::ReadOnly) || !queryTarget.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                    queryTarget.write(querySource.readAll()) < 0) {
                    QFile::remove(output);
                    finish(id, false, I18n::text("Could not install Tree-sitter highlight query"));
                    return;
                }
                queryTarget.close();
            }
            const QString licenseSource = QDir(sourceRoot).filePath("LICENSE");
            if (QFileInfo::exists(licenseSource)) {
                QFile::remove(installDir + "/LICENSE");
                QFile::copy(licenseSource, installDir + "/LICENSE");
            }
            QFile::remove(GrammarManager::libraryPath(id));
            if (!QFile::rename(output, GrammarManager::libraryPath(id))) {
                QFile::remove(output);
                finish(id, false, I18n::text("Could not activate grammar module"));
                return;
            }
            const bool valid = GrammarManager::isInstalled(id);
            if (!valid) {
                QFile::remove(GrammarManager::libraryPath(id));
                QFile::remove(GrammarManager::queryPath(id));
            }
            if (!valid) { tryOlderVersion(id, I18n::text("Grammar failed Tree-sitter ABI or query validation")); return; }
            QFile versionFile(installDir + "/version.json");
            if (versionFile.open(QIODevice::WriteOnly))
                versionFile.write(QJsonDocument(QJsonObject{{"ref", selectedRefs_.value(id)},
                    {"repository", LanguageRegistry::language(id)->repository}}).toJson());
            finish(id, true, I18n::text("Grammar installed and validated: ") + selectedRefs_.value(id));
        });
        connect(build, &QProcess::errorOccurred, this, [this, id](QProcess::ProcessError) {
            finish(id, false, I18n::text("Could not start the C compiler"));
        });
        build->start(compiler, args);
    });
    connect(extract, &QProcess::errorOccurred, this, [this, id](QProcess::ProcessError) {
        finish(id, false, I18n::text("Could not start tar to unpack grammar sources"));
    });
    extract->start("tar", {"-xzf", archivePath, "-C", sourceRoot, "--strip-components=1"});
}

void GrammarManager::finish(const QString& id, bool success, const QString& message) {
    if (!installing_.contains(id)) return;
    installing_.remove(id);
    candidates_.remove(id);
    selectedRefs_.remove(id);
    candidateErrors_.remove(id);
    tagsLoaded_.remove(id);
    if (auto* process = processes_.take(id)) { process->kill(); process->deleteLater(); }
    if (auto* temp = temporaryDirectories_.take(id)) delete temp;
    emit statusChanged(id, message);
    emit installationFinished(id, success);
}

