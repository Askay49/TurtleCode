#include "app/I18n.hpp"
#include "LspClient.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>
namespace {
QJsonObject position(const QString& text, int offset) {
    offset = std::clamp(offset, 0, static_cast<int>(text.size()));
    const auto prefix = text.left(offset);
    const int line = prefix.count('\n');
    const int last = prefix.lastIndexOf('\n');
    return {{"line", line}, {"character", offset - last - 1}};
}
QJsonObject incrementalChange(const QString& before, const QString& after) {
    int prefix = 0;
    while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) ++prefix;
    if (prefix > 0 && prefix < before.size() && before[prefix].isLowSurrogate()) --prefix;
    int suffix = 0;
    while (suffix < before.size()-prefix && suffix < after.size()-prefix && before[before.size()-suffix-1] == after[after.size()-suffix-1]) ++suffix;
    if (suffix > 0 && before[before.size()-suffix].isLowSurrogate()) --suffix;
    return {{"range", QJsonObject{{"start", position(before, prefix)}, {"end", position(before, before.size()-suffix)}}},
            {"text", after.mid(prefix, after.size()-prefix-suffix)}};
}
}
LspServerConfig LspServerConfig::clangd() {
    LspServerConfig config;
    for (const auto& name : QStringList{"clangd", "clangd-20", "clangd-19", "clangd-18", "clangd-17", "clangd-16", "clangd-15", "clangd-14"}) {
        config.command = QStandardPaths::findExecutable(name);
        if (!config.command.isEmpty()) break;
    }
    return config;
}
LspClient::LspClient(QObject* parent) : LspClient(LspServerConfig::clangd(), parent) {}
LspClient::LspClient(const LspServerConfig& config, QObject* parent) : QObject(parent), config_(config) {
    status_=I18n::text("Stopped");
    connect(&process_, &QProcess::started, this, &LspClient::initialize);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &LspClient::readMessages);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const auto bytes = process_.readAllStandardError();
        static const QRegularExpression failure("(^E\\[|\\berror\\b|\\bfatal\\b|\\bexception\\b)", QRegularExpression::CaseInsensitiveOption);
        for(const auto& line:bytes.split('\n')) if(!line.isEmpty()) {
            const auto text=QString::fromUtf8(line);
            appendLog("stderr: " + text, failure.match(text).hasMatch());
        }
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError errorCode) {
        if (stopping_ || destroying_) return;
        emit error(config_.name + ": " + process_.errorString());
        if (errorCode == QProcess::FailedToStart) setStatus(I18n::text("Failed to start: ") + process_.errorString());
    });
    connect(&process_, &QProcess::finished, this, [this] {
        initialized_ = false; opened_.clear(); callbacks_.clear(); input_.clear();
        for (auto it = documents_.cbegin(); it != documents_.cend(); ++it) emit diagnostics(it.key(), {});
        if (stopping_ || destroying_) return;
        setStatus(config_.name + I18n::text(" stopped")); emit error(status_ + I18n::text(". See Settings → Language server for the log."));
        if (!documents_.isEmpty() && restarts_++ < 2) QTimer::singleShot(1000, this, [this] { start(); });
    });
    deadlines_.setInterval(1000);
    connect(&deadlines_, &QTimer::timeout, this, [this] {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto ids = callbacks_.keys();
        for (int id : ids) if (callbacks_.contains(id) && callbacks_[id].deadline < now) {
            auto pending = callbacks_.take(id);
            notify("$/cancelRequest", {{"id", id}});
            if (pending.callback) pending.callback((pending.method=="textDocument/formatting" || pending.method=="textDocument/documentSymbol") ? QJsonValue(QJsonValue::Undefined) : QJsonValue{});
            emit error(config_.name + I18n::text(": request timed out (") + pending.method + ")");
            if (pending.method == "initialize") { stop(); setStatus(I18n::text("Initialization timed out")); }
        }
    });
    connect(this, &LspClient::error, this, [this](const QString& message) { appendLog(message, true); });
    deadlines_.start();
}
LspClient::~LspClient() { destroying_ = true; stop(); }
void LspClient::setStatus(const QString& status) { status_ = status; appendLog(status); emit statusChanged(status); }
namespace {
void trimLog(QByteArray& log, int budget) {
    if (log.size() <= budget) return;
    log = log.right(budget);
    const auto newline = log.indexOf('\n');
    if (newline >= 0) log.remove(0, newline + 1);
    // A truncated UTF-8 prefix must never appear as a broken character.
    else while (!log.isEmpty() && (static_cast<unsigned char>(log.front()) & 0xc0) == 0x80) log.remove(0, 1);
}
}
void LspClient::appendLog(const QString& message, bool failure) {
    if (config_.logLevel == 0) return;
    const auto entry = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz ").toUtf8()
        + message.left(failure ? 512 : 1024).toUtf8() + '\n';
    auto& destination = failure ? errorLog_ : eventLog_;
    destination += entry;
    trimLog(destination, NormalLogBytes / 2);
}
void LspClient::traceMessage(const QString& direction, const QJsonObject& message) {
    if (config_.logLevel != 2) return;
    auto payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if(payload.size()>64*1024) payload=QString::fromUtf8(payload.left(64*1024)).toUtf8()+" [truncated]";
    traceLog_ += QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz ").toUtf8()
        + direction.toUtf8() + ' ' + payload + '\n';
    trimLog(traceLog_, DetailedLogBytes - NormalLogBytes);
}
QString LspClient::log() const {
    return QString::fromUtf8(errorLog_ + eventLog_ + traceLog_);
}
void LspClient::clearLog() { eventLog_.clear(); errorLog_.clear(); traceLog_.clear(); }
void LspClient::setLogLevel(int level) {
    config_.logLevel = std::clamp(level, 0, 2);
    if (config_.logLevel != 2) traceLog_.clear();
    if (config_.logLevel == 0) clearLog();
}
void LspClient::checkConnection() { checking_ = true; restart(); }
QString LspClient::languageId(const QString& path) const {
    const auto name = QFileInfo(path).fileName().toLower();
    return config_.fileNames.value(name, config_.languageIds.value(QFileInfo(path).suffix().toLower()));
}
void LspClient::stop() {
    stopping_ = true;
    callbacks_.clear();
    if (process_.state() == QProcess::Running) {
        shutdownId_ = nextId_++; shutdownAcknowledged_ = false;
        send({{"jsonrpc", "2.0"}, {"id", shutdownId_}, {"method", "shutdown"}, {"params", QJsonValue::Null}});
        process_.waitForBytesWritten(100);
        QElapsedTimer timer; timer.start();
        while (!shutdownAcknowledged_ && process_.state() == QProcess::Running && timer.elapsed() < 300) {
            process_.waitForReadyRead(40); readMessages();
        }
        if (shutdownAcknowledged_) { notify("exit", {}); process_.waitForBytesWritten(100); }
        if (!process_.waitForFinished(500)) { process_.terminate(); if (!process_.waitForFinished(300)) { process_.kill(); process_.waitForFinished(300); } }
    } else if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(500); }
    initialized_ = false; opened_.clear(); input_.clear(); capabilities_ = {};
    stopping_ = false;
}
void LspClient::configure(const LspServerConfig& config) {
    stop(); config_ = config; setLogLevel(config.logLevel); restarts_ = 0;
    if(!available()) { documents_.clear(); versions_.clear(); }
    const auto paths = documents_.keys();
    for (const auto& path : paths) if (!supports(path)) close(path);
    setStatus(available() ? config_.name + I18n::text(": waiting for documents") : I18n::text("Language server disabled or not found"));
    start();
}
void LspClient::restart() { stop(); restarts_ = 0; start(); }
void LspClient::setRoot(const QString& root) {
    const QString normalized = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    if (root_ == normalized) return;
    root_ = normalized;
    if (process_.state() != QProcess::NotRunning) restart();
}
bool LspClient::supports(const QString& path) const { return config_.acceptAnyLanguage || !languageId(path).isEmpty(); }
void LspClient::start() {
    if (process_.state() != QProcess::NotRunning || (documents_.isEmpty() && !checking_)) return;
    if (!available()) { setStatus(I18n::text("Language server not configured")); return; }
    process_.setWorkingDirectory(root_.isEmpty() ? (documents_.isEmpty() ? QDir::currentPath() : QFileInfo(documents_.cbegin().key()).absolutePath()) : root_);
    setStatus(I18n::text("Starting ") + config_.name + "…");
    appendLog("Launch: " + config_.command + " " + config_.arguments.join(' '));
    input_.clear(); process_.start(config_.command, config_.arguments);
}
void LspClient::initialize() {
    QJsonObject capabilities{
        {"general", QJsonObject{{"positionEncodings", QJsonArray{"utf-16"}}}},
        {"workspace", QJsonObject{{"configuration", true}, {"workspaceFolders", true}, {"workspaceEdit", QJsonObject{{"documentChanges", true}}}, {"semanticTokens", QJsonObject{{"refreshSupport", true}}}}},
        {"textDocument", QJsonObject{
            {"completion", QJsonObject{{"completionItem", QJsonObject{{"snippetSupport", false}, {"insertReplaceSupport", true}}}}},
            {"semanticTokens", QJsonObject{
                {"requests", QJsonObject{{"full", true},{"range",true}}},
                {"tokenTypes", QJsonArray{"namespace","type","class","enum","interface","struct","typeParameter","parameter","variable","property","enumMember","event","function","method","macro","keyword","modifier","comment","string","number","regexp","operator","decorator"}},
                {"tokenModifiers", QJsonArray{"declaration","definition","readonly","static","deprecated","abstract","async","modification","documentation","defaultLibrary"}},
                {"formats", QJsonArray{"relative"}}, {"overlappingTokenSupport",false}, {"multilineTokenSupport",false}
            }},
            {"definition", QJsonObject{{"linkSupport", true}}},
            {"declaration", QJsonObject{{"linkSupport", true}}},
            {"typeDefinition", QJsonObject{{"linkSupport", true}}},
            {"references", QJsonObject{{"dynamicRegistration", false}}},
            {"documentSymbol", QJsonObject{{"hierarchicalDocumentSymbolSupport",true}}},
            {"formatting", QJsonObject{{"dynamicRegistration", false}}},
            {"rename", QJsonObject{{"prepareSupport", true}, {"prepareSupportDefaultBehavior", 1}}},
            {"hover", QJsonObject{{"contentFormat", QJsonArray{"plaintext"}}}},
            {"signatureHelp", QJsonObject{{"signatureInformation", QJsonObject{{"documentationFormat", QJsonArray{"plaintext"}}}}}},
            {"publishDiagnostics", QJsonObject{{"versionSupport", true}}}
        }}
    };
    const auto uri = QUrl::fromLocalFile(process_.workingDirectory()).toString();
    request("initialize", {{"processId", static_cast<int>(QCoreApplication::applicationPid())},
        {"rootUri", uri}, {"capabilities", capabilities}, {"initializationOptions", config_.initializationOptions},
        {"workspaceFolders", QJsonArray{QJsonObject{{"uri", uri}, {"name", QFileInfo(process_.workingDirectory()).fileName()}}}}}, [this](QJsonValue result) {
        if (!result.isObject() || !result.toObject().contains("capabilities")) {
            stop(); setStatus(I18n::text("Server initialization failed")); emit error(status_); return;
        }
        capabilities_ = result.toObject().value("capabilities").toObject();
        const auto encoding = capabilities_.value("positionEncoding").toString("utf-16");
        if (encoding != "utf-16") { stop(); setStatus(I18n::text("Server does not support UTF-16 positions")); emit error(status_); return; }
        const auto sync = capabilities_.value("textDocumentSync");
        syncKind_ = sync.isDouble() ? sync.toInt() : sync.toObject().value("change").toInt(1);
        initialized_ = true;
        notify("initialized", {});
        if (!config_.settings.isEmpty()) notify("workspace/didChangeConfiguration", {{"settings", config_.settings}});
        checking_ = false;
        for (auto it = documents_.cbegin(); it != documents_.cend(); ++it) sendOpen(it.key());
        setStatus(config_.name + I18n::text(" ready"));
    });
}
QJsonObject LspClient::documentId(const QString& path) const { return {{"uri", QUrl::fromLocalFile(path).toString()}}; }
void LspClient::sendOpen(const QString& path) {
    if (!initialized_ || opened_.contains(path) || !documents_.contains(path)) return;
    auto document = documentId(path);
    document.insert("languageId", documentLanguages_.value(path, languageId(path)));
    document.insert("version", versions_.value(path)); document.insert("text", documents_.value(path));
    notify("textDocument/didOpen", {{"textDocument", document}}); opened_.insert(path);
}
void LspClient::open(const QString& path, const QString& text, const QString& id) {
    if (path.isEmpty() || !supports(path) || !available()) return;
    if (documents_.contains(path)) { change(path, text); return; }
    documentLanguages_[path] = id.isEmpty() ? languageId(path) : id;
    documents_[path] = text; versions_[path] = 1; start(); sendOpen(path);
}
void LspClient::cancel(const QString& path, const QString& method) {
    const auto ids = callbacks_.keys();
    for (int id : ids) if (callbacks_[id].path == path && (method.isEmpty() || callbacks_[id].method == method)) {
        if(config_.logLevel==2) appendLog("Cancel " + callbacks_[id].method + " #" + QString::number(id));
        callbacks_.remove(id); notify("$/cancelRequest", {{"id", id}});
    }
}
void LspClient::change(const QString& path, const QString& text) {
    if (!documents_.contains(path)) { open(path, text); return; }
    const auto previous = documents_.value(path);
    if (previous == text) return;
    cancel(path); documents_[path] = text; ++versions_[path];
    emit diagnostics(path, {});
    if (!opened_.contains(path)) { start(); sendOpen(path); return; }
    auto document = documentId(path); document.insert("version", versions_.value(path));
    const QJsonObject edit = syncKind_ == 2 ? incrementalChange(previous, text) : QJsonObject{{"text", text}};
    if (syncKind_ != 0) notify("textDocument/didChange", {{"textDocument", document}, {"contentChanges", QJsonArray{edit}}});
}
void LspClient::saved(const QString& path) {
    if (!opened_.contains(path)) return;
    const auto save = capabilities_.value("textDocumentSync").toObject().value("save");
    if (save.isBool() && !save.toBool()) return;
    QJsonObject params{{"textDocument", documentId(path)}};
    if (save.toObject().value("includeText").toBool()) params.insert("text", documents_.value(path));
    notify("textDocument/didSave", params);
}
void LspClient::close(const QString& path) {
    cancel(path);
    if (opened_.remove(path)) notify("textDocument/didClose", {{"textDocument", documentId(path)}});
    documents_.remove(path); documentLanguages_.remove(path); versions_.remove(path); emit diagnostics(path, {});
}
void LspClient::references(const QString& path, int line, int character, std::function<void(QJsonValue)> callback) {
    if (!initialized_ || !opened_.contains(path)) { if (callback) callback({}); return; }
    cancel(path, "textDocument/references");
    request("textDocument/references", {{"textDocument", documentId(path)},
        {"position", QJsonObject{{"line", line}, {"character", character}}},
        {"context", QJsonObject{{"includeDeclaration", true}}}}, std::move(callback), path);
}
void LspClient::positionRequest(const QString& method, const QString& path, int line, int character, std::function<void(QJsonValue)> callback) {
    if (!initialized_ || !opened_.contains(path)) { if (callback) callback({}); return; }
    cancel(path, method);
    request(method, {{"textDocument", documentId(path)}, {"position", QJsonObject{{"line", line}, {"character", character}}}}, std::move(callback), path);
}
void LspClient::completion(const QString& p, int l, int c, std::function<void(QJsonValue)> f) { positionRequest("textDocument/completion", p, l, c, std::move(f)); }
void LspClient::definition(const QString& p, int l, int c, std::function<void(QJsonValue)> f) { positionRequest("textDocument/definition", p, l, c, std::move(f)); }
void LspClient::typeDefinition(const QString& p, int l, int c, std::function<void(QJsonValue)> f) { positionRequest("textDocument/typeDefinition", p, l, c, std::move(f)); }
void LspClient::declaration(const QString& p, int l, int c, std::function<void(QJsonValue)> f) { positionRequest("textDocument/declaration", p, l, c, std::move(f)); }
void LspClient::hover(const QString& p, int l, int c, std::function<void(QJsonValue)> f) { positionRequest("textDocument/hover", p, l, c, std::move(f)); }
void LspClient::signatureHelp(const QString& p, int l, int c, std::function<void(QJsonValue)> f) { positionRequest("textDocument/signatureHelp", p, l, c, std::move(f)); }
bool LspClient::documentSymbolsAvailable() const {
    const auto provider=capabilities_.value("documentSymbolProvider");
    return initialized_ && (provider.toBool() || provider.isObject());
}
void LspClient::documentSymbols(const QString& path,std::function<void(QJsonValue)> callback) {
    if(!documentSymbolsAvailable() || !opened_.contains(path)) { if(callback) callback(QJsonValue(QJsonValue::Undefined)); return; }
    cancel(path,"textDocument/documentSymbol");
    request("textDocument/documentSymbol",{{"textDocument",documentId(path)}},std::move(callback),path);
}
bool LspClient::canFormat() const {
    const auto provider=capabilities_.value("documentFormattingProvider");
    return initialized_ && (provider.toBool() || provider.isObject());
}
void LspClient::formatDocument(const QString& path, int tabSize, bool insertSpaces, std::function<void(QJsonValue)> callback) {
    if (!canFormat() || !opened_.contains(path)) { if(callback) callback(QJsonValue(QJsonValue::Undefined)); return; }
    cancel(path,"textDocument/formatting");
    request("textDocument/formatting",{{"textDocument",documentId(path)},
        {"options",QJsonObject{{"tabSize",std::max(1,tabSize)},{"insertSpaces",insertSpaces}}}},std::move(callback),path);
}
void LspClient::send(QJsonObject message) {
    if (process_.state() != QProcess::Running) return;
    traceMessage("→", message);
    const auto payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    process_.write("Content-Length: " + QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
}
void LspClient::notify(const QString& method, QJsonObject params) { send({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}); }
void LspClient::request(const QString& method, QJsonObject params, std::function<void(QJsonValue)> callback, const QString& path) {
    const int id = nextId_++;
    const auto now=QDateTime::currentMSecsSinceEpoch();
    callbacks_.insert(id, {std::move(callback), path, method, versions_.value(path), now + ((method == "initialize" || method == "textDocument/rename") ? 30000 : method.startsWith("textDocument/semanticTokens/") ? 2500 : 10000), now});
    send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
}
void LspClient::readMessages() {
    input_ += process_.readAllStandardOutput();
    while (true) {
        const auto end = input_.indexOf("\r\n\r\n");
        if (end < 0) { if (input_.size() > 8192) { emit error(I18n::text("Invalid LSP header")); stop(); } return; }
        qint64 length = -1;
        for (const auto& line : input_.left(end).split('\n')) {
            const auto colon = line.indexOf(':');
            if (colon >= 0 && line.left(colon).trimmed().toLower() == "content-length") {
                bool ok; length = line.mid(colon+1).trimmed().toLongLong(&ok); if (!ok) length = -1;
            }
        }
        if (length < 0 || length > 64*1024*1024) { emit error(I18n::text("Invalid LSP Content-Length")); stop(); return; }
        if (input_.size() < end+4+length) return;
        const auto payload = input_.mid(end+4, length); input_.remove(0, end+4+length);
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(payload, &error);
        if (error.error == QJsonParseError::NoError && doc.isObject()) dispatch(doc.object());
        else emit this->error(I18n::text("Invalid JSON from language server"));
    }
}
void LspClient::dispatch(const QJsonObject& message) {
    traceMessage("←", message);
    if (stopping_ || destroying_) {
        if (!message.contains("method") && message.value("id").toInt(-1) == shutdownId_) shutdownAcknowledged_ = true;
        return;
    }
    const auto method = message.value("method").toString();
    if (message.contains("id")) {
        if (!method.isEmpty()) {
            const auto id = message.value("id");
            if (method == "workspace/semanticTokens/refresh") {
                send({{"jsonrpc","2.0"},{"id",id},{"result",QJsonValue::Null}});
                emit semanticTokensRefresh();
            } else if (method == "workspace/configuration") {
                QJsonArray result; const auto items = message.value("params").toObject().value("items").toArray();
                for (const auto& item : items) {
                    QJsonValue value = config_.settings;
                    const auto section = item.toObject().value("section").toString();
                    if (!section.isEmpty()) for (const auto& key : section.split('.')) value = value.toObject().value(key);
                    result.append(value.isUndefined() ? QJsonValue(QJsonObject{}) : value);
                }
                send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
            } else if (method == "workspace/workspaceFolders") {
                send({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonArray{QJsonObject{{"uri", QUrl::fromLocalFile(process_.workingDirectory()).toString()}, {"name", QFileInfo(process_.workingDirectory()).fileName()}}}}});
            } else {
                appendLog("Unsupported client request: " + method, true);
                send({{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", -32601}, {"message", I18n::text("Client method not supported: ") + method}}}});
            }
        } else {
            const int id = message.value("id").toInt(-1);
            if (!callbacks_.contains(id)) { if(config_.logLevel==2) appendLog("Ignored response: " + QString::number(id)); return; }
            auto pending = callbacks_.take(id);
            if(config_.logLevel==2) appendLog("Response " + pending.method + " #" + QString::number(id) + " " + QString::number(QDateTime::currentMSecsSinceEpoch()-pending.startedAt) + " ms");
            if (!pending.path.isEmpty() && (!documents_.contains(pending.path) || pending.version != versions_.value(pending.path))) return;
            if (message.contains("error")) {
                const auto object = message.value("error").toObject(); const int code = object.value("code").toInt();
                if (code != -32800 && code != -32801) emit error(config_.name + ": " + object.value("message").toString());
                if (pending.callback) pending.callback((pending.method=="textDocument/formatting" || pending.method=="textDocument/documentSymbol") ? QJsonValue(QJsonValue::Undefined) : QJsonValue{});
            } else if (pending.callback) pending.callback(message.value("result"));
        }
        return;
    }
    const auto params = message.value("params").toObject();
    if (method == "textDocument/publishDiagnostics") {
        const auto path = QUrl(params.value("uri").toString()).toLocalFile();
        if (!documents_.contains(path)) return;
        const auto version = params.value("version");
        if (version.isDouble() && version.toInt() != versions_.value(path)) return;
        emit diagnostics(path, params.value("diagnostics").toArray());
    } else if (method == "window/showMessage") emit error(config_.name + ": " + params.value("message").toString());
    else if (method == "window/logMessage") appendLog(params.value("message").toString(), params.value("type").toInt() == 1);
}

bool LspClient::canRename() const {
    const auto provider=capabilities_.value("renameProvider");
    return initialized_ && (provider.isObject() || provider.toBool());
}
void LspClient::prepareRename(const QString& path,int line,int character,std::function<void(QJsonValue)> callback) {
    if (!canRename() || !opened_.contains(path)) { callback({}); return; }
    if (!capabilities_.value("renameProvider").toObject().value("prepareProvider").toBool()) {
        callback(QJsonObject{{"defaultBehavior",true}}); return;
    }
    positionRequest("textDocument/prepareRename",path,line,character,std::move(callback));
}
void LspClient::renameSymbol(const QString& path,int line,int character,const QString& newName,std::function<void(QJsonValue)> callback) {
    if (!canRename() || !opened_.contains(path)) { callback({}); return; }
    cancel(path,"textDocument/rename");
    request("textDocument/rename",{{"textDocument",documentId(path)},
        {"position",QJsonObject{{"line",line},{"character",character}}},{"newName",newName}},std::move(callback),path);
}

bool LspClient::semanticTokensAvailable() const { return semanticTokensFullAvailable() || semanticTokensRangeAvailable(); }
bool LspClient::semanticTokensFullAvailable() const {
    const auto provider=capabilities_.value("semanticTokensProvider").toObject();
    const auto full=provider.value("full");
    return initialized_ && provider.value("legend").isObject() && (full.isObject() || full.toBool());
}
bool LspClient::semanticTokensRangeAvailable() const {
    const auto provider=capabilities_.value("semanticTokensProvider").toObject();
    const auto range=provider.value("range");
    return initialized_ && provider.value("legend").isObject() && (range.isObject() || range.toBool());
}
void LspClient::cancelSemanticTokens(const QString& path) {
    cancel(path,"textDocument/semanticTokens/full"); cancel(path,"textDocument/semanticTokens/range");
}
void LspClient::semanticTokens(const QString& path,std::function<void(QJsonValue)> callback,const QJsonObject& range) {
    const bool ranged=!range.isEmpty();
    if (!(ranged?semanticTokensRangeAvailable():semanticTokensFullAvailable()) || !opened_.contains(path)) { callback({}); return; }
    cancelSemanticTokens(path);
    const auto legend=capabilities_.value("semanticTokensProvider").toObject().value("legend").toObject();
    QJsonObject params{{"textDocument",documentId(path)}}; if(ranged) params.insert("range",range);
    request(ranged?"textDocument/semanticTokens/range":"textDocument/semanticTokens/full",params,
        [callback=std::move(callback),legend](QJsonValue result) {
            if (!result.isObject()) { callback({}); return; }
            auto object=result.toObject(); object.insert("legend",legend); callback(object);
        },path);
}
