#include "LspManager.hpp"
#include "LanguageRegistry.hpp"
#include "app/I18n.hpp"
#include <QSettings>
#include <QJsonDocument>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

LspConfiguration LspConfiguration::load() {
    LspConfiguration result;
    QSettings settings;
    if (!settings.contains("lsp/profiles")) {
        auto server = LspServerConfig::clangd();
        server.command = settings.value("lsp/clangd/command", server.command).toString();
        server.arguments = QProcess::splitCommand(settings.value("lsp/clangd/arguments", "--background-index").toString());
        const auto directory = settings.value("lsp/clangd/compileCommandsDirectory").toString().trimmed();
        if (!directory.isEmpty()) server.arguments.append("--compile-commands-dir=" + directory);
        result.profiles.append({"clangd", server, true});
        result.bindings = {{"c", "clangd"}, {"cpp", "clangd"}};
        return result;
    }
    const auto object = QJsonDocument::fromJson(settings.value("lsp/profiles").toByteArray()).object();
    QSet<QString> ids;
    for (const auto& value : object.value("profiles").toArray()) {
        const auto item = value.toObject();
        LspProfile profile;
        profile.id = item.value("id").toString();
        if (profile.id.isEmpty() || ids.contains(profile.id)) continue;
        ids.insert(profile.id);
        auto& server = profile.server;
        server.name = item.value("name").toString(profile.id);
        server.command = item.value("command").toString();
        server.arguments.clear();
        for (const auto& arg : item.value("arguments").toArray()) server.arguments.append(arg.toString());
        server.initializationOptions = item.value("initializationOptions").toObject();
        server.settings = item.value("settings").toObject();
        server.logLevel = std::clamp(item.value("logLevel").toInt(1), 0, 2);
        profile.enabled = item.value("enabled").toBool(true);
        result.profiles.append(profile);
    }
    const auto bindings = object.value("bindings").toObject();
    for (auto it=bindings.begin(); it!=bindings.end(); ++it) result.bindings[it.key()] = it.value().toString();
    const auto languages = object.value("languageIds").toObject();
    for (auto it=languages.begin(); it!=languages.end(); ++it) result.languageIds[it.key()] = it.value().toString();
    return result;
}
QJsonObject LspConfiguration::json() const {
    QJsonArray profilesJson;
    for (const auto& profile : profiles) {
        const auto& server = profile.server;
        QJsonArray arguments; for (const auto& arg : server.arguments) arguments.append(arg);
        profilesJson.append(QJsonObject{{"id", profile.id}, {"name", server.name}, {"command", server.command},
            {"arguments", arguments}, {"enabled", profile.enabled}, {"initializationOptions", server.initializationOptions},
            {"settings", server.settings}, {"logLevel", server.logLevel}});
    }
    QJsonObject bindingsJson, languagesJson;
    for (auto it=bindings.begin(); it!=bindings.end(); ++it) bindingsJson[it.key()] = it.value();
    for (auto it=languageIds.begin(); it!=languageIds.end(); ++it) languagesJson[it.key()] = it.value();
    return {{"profiles", profilesJson}, {"bindings", bindingsJson}, {"languageIds", languagesJson}};
}
void LspConfiguration::save() const { QSettings().setValue("lsp/profiles", QJsonDocument(json()).toJson(QJsonDocument::Compact)); }
LspManager::LspManager(QObject* parent) : QObject(parent) {}
LspManager::~LspManager() {
    for (const auto& instance : instances_) { instance.client->disconnect(this); delete instance.client; }
}
const LspProfile* LspManager::profile(const QString& id) const {
    for (const auto& item : configuration_.profiles) if (item.id == id) return &item;
    return nullptr;
}
void LspManager::configure(const LspConfiguration& configuration) {
    const bool enabled=QSettings().value("lsp/enabled",true).toBool();
    auto withoutLogLevels=[](QJsonObject object) {
        auto profiles=object.value("profiles").toArray();
        for(int i=0;i<profiles.size();++i){auto profile=profiles[i].toObject();profile.remove("logLevel");profiles[i]=profile;}
        object["profiles"]=profiles;return object;
    };
    if(enabled==enabled_ && withoutLogLevels(configuration.json())==withoutLogLevels(configuration_.json())) {
        configuration_=configuration;
        for(const auto& instance:instances_) if(const auto* item=profile(instance.profile)) instance.client->setLogLevel(item->server.logLevel);
        return;
    }
    // Disconnect forwarding before destruction to keep stale diagnostics out of a
    // new configuration. Editors are explicitly cleared by the settings owner.
    documents_.clear();
    for (const auto& instance : instances_) { instance.client->disconnect(this); delete instance.client; }
    instances_.clear(); configuration_ = configuration;
    enabled_ = QSettings().value("lsp/enabled", true).toBool();
    // Keep an idle clangd client for existing lifecycle integrations; no process
    // starts until a document is attached or a connection check is requested.
    if (const auto* initial = profile("clangd")) createClient(*initial, {});
    emit statusChanged(status());
}
void LspManager::setRoot(const QString& root) {
    const auto normalized = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    if (root_ == normalized) return;
    root_ = normalized;
    // Open documents retain their own server/root. Newly opened documents use
    // the selected workspace, avoiding reassignment when tabs change.
}
QString LspManager::profileFor(const QString& path) const {
    const auto* language = LanguageRegistry::languageForPath(path);
    return language ? configuration_.bindings.value(language->id) : QString{};
}
QString LspManager::protocolLanguage(const QString& path) const {
    const auto* language = LanguageRegistry::languageForPath(path);
    if (!language) return {};
    const auto override = configuration_.languageIds.value(language->id);
    if (!override.isEmpty()) return override;
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (language->id == "javascript" && suffix == "jsx") return "javascriptreact";
    if (language->id == "typescript" && suffix == "tsx") return "typescriptreact";
    if (language->id == "bash") return "shellscript";
    if (language->id == "assembly") return "asm";
    if (language->id == "ini" && QFileInfo(path).fileName().toLower() == ".editorconfig") return "editorconfig";
    return language->id;
}
bool LspManager::supports(const QString& path) const {
    const auto* item = profile(profileFor(path));
    return enabled_ && item && item->enabled && !item->server.command.trimmed().isEmpty();
}
bool LspManager::available() const {
    if (!enabled_) return false;
    for (const auto& item : configuration_.profiles) if (item.enabled && !item.server.command.isEmpty()) return true;
    return false;
}
QString LspManager::rootFor(const QString& path) const {
    if (!root_.isEmpty() && root_ != QDir::homePath()
        && (path == root_ || path.startsWith(root_ + '/'))) return root_;
    QDir directory(QFileInfo(path).absolutePath());
    const auto fallback = directory.absolutePath();
    const QStringList markers{".git", "compile_commands.json", "CMakeLists.txt", "Cargo.toml", "go.mod", "package.json", "pyproject.toml", "pom.xml", "build.gradle", "build.gradle.kts", "pubspec.yaml", "Package.swift", "meson.build"};
    do { for (const auto& marker : markers) if (directory.exists(marker)) return directory.absolutePath(); }
    while (directory.cdUp());
    return fallback;
}
LspClient* LspManager::createClient(const LspProfile& profile, const QString& root) {
    auto config = profile.server;
    config.acceptAnyLanguage = true;
    if (!enabled_ || !profile.enabled) config.command.clear();
    auto* client = new LspClient(config, this);
    if (!root.isEmpty()) client->setRoot(root);
    instances_.append({profile.id, root, client});
    connect(client, &LspClient::diagnostics, this, &LspManager::diagnostics);
    connect(client, &LspClient::error, this, &LspManager::error);
    connect(client, &LspClient::statusChanged, this, [this, client](const QString& state) {
        emit clientStateChanged(client); emit statusChanged(state);
    });
    connect(client, &LspClient::semanticTokensRefresh, this, [this, client] { emit semanticTokensRefresh(client); });
    return client;
}
LspClient* LspManager::existing(const QString& path) const { return documents_.value(path).data(); }
LspClient* LspManager::clientFor(const QString& path) {
    if (auto* client = existing(path)) return client;
    if (!supports(path)) return nullptr;
    const auto id = profileFor(path), root = rootFor(path);
    for (auto& instance : instances_) if (instance.profile == id && (instance.root == root || instance.root.isEmpty()
        || std::none_of(documents_.cbegin(),documents_.cend(),[&](const auto& document){return document.data()==instance.client;}))) {
        if (instance.root != root) { instance.root = root; instance.client->setRoot(root); }
        documents_[path] = instance.client;
        return instance.client;
    }
    auto* client = createClient(*profile(id), root); documents_[path] = client; return client;
}
QVector<LspClient*> LspManager::clients(const QString& id) const {
    QVector<LspClient*> result;
    for (const auto& instance : instances_) if (instance.profile == id) result.append(instance.client);
    return result;
}
LspClient* LspManager::checkConnection(const QString& id) {
    const auto* item = profile(id);
    if (!enabled_ || !item || !item->enabled || item->server.command.isEmpty()) return nullptr;
    const auto root = root_.isEmpty() ? QDir::currentPath() : root_;
    for (auto& instance : instances_) if (instance.profile == id && (instance.root == root || instance.root.isEmpty())) {
        instance.root = root; instance.client->setRoot(root); instance.client->checkConnection(); return instance.client;
    }
    auto* client = createClient(*item, root); client->checkConnection(); return client;
}
void LspManager::restart(const QString& id) { for (auto* client : clients(id)) client->restart(); }
void LspManager::clearLog(const QString& id) { for (auto* client : clients(id)) client->clearLog(); }
QString LspManager::log(const QString& id) const {
    QString result;
    for (const auto& instance : instances_) if (instance.profile == id && !instance.client->log().isEmpty())
        result += instance.root + '\n' + instance.client->log() + '\n';
    return result;
}
QString LspManager::status(const QString& path) const {
    if (!path.isEmpty()) {
        if (auto* client = existing(path)) return client->status();
        if (!supports(path)) return I18n::text("Language server not configured");
        const auto* item = profile(profileFor(path));
        return item->server.name + I18n::text(": waiting for documents");
    }
    return available() ? I18n::text("Waiting for documents") : I18n::text("Language server disabled or not found");
}
bool LspManager::ready(const QString& path) const { auto* client = existing(path); return client && client->ready(); }
void LspManager::open(const QString& path, const QString& text) { if(auto* client=clientFor(path)) client->open(path,text,protocolLanguage(path)); }
void LspManager::change(const QString& path, const QString& text) { open(path,text); }
void LspManager::saved(const QString& path) { if(auto* client=existing(path)) client->saved(path); }
void LspManager::close(const QString& path) { if(auto* client=existing(path)) client->close(path); documents_.remove(path); }
int LspManager::documentVersion(const QString& path) const { auto* client=existing(path); return client ? client->documentVersion(path) : 0; }
int LspManager::documentVersionForServer(const QString& source, const QString& target) const {
    auto* client=existing(source); return client ? client->documentVersion(target) : 0;
}
#define LSP_QUERY(method) bool LspManager::method(const QString& path) const { auto* client=existing(path); return client && client->method(); }
LSP_QUERY(documentSymbolsAvailable)
LSP_QUERY(canFormat)
LSP_QUERY(canRename)
LSP_QUERY(semanticTokensAvailable)
LSP_QUERY(semanticTokensRangeAvailable)
LSP_QUERY(semanticTokensFullAvailable)
#undef LSP_QUERY
void LspManager::cancelSemanticTokens(const QString& path) { if(auto* client=existing(path)) client->cancelSemanticTokens(path); }
#define LSP_POSITION(method) void LspManager::method(const QString& path,int line,int column,Callback callback) { if(auto* client=existing(path)) client->method(path,line,column,std::move(callback)); else if(callback) callback({}); }
LSP_POSITION(completion)
LSP_POSITION(definition)
LSP_POSITION(references)
LSP_POSITION(typeDefinition)
LSP_POSITION(declaration)
LSP_POSITION(hover)
LSP_POSITION(signatureHelp)
LSP_POSITION(prepareRename)
#undef LSP_POSITION
void LspManager::renameSymbol(const QString& path,int line,int column,const QString& name,Callback callback) {
    if(auto* client=existing(path)) client->renameSymbol(path,line,column,name,std::move(callback)); else if(callback) callback({});
}
void LspManager::formatDocument(const QString& path,int tabSize,bool spaces,Callback callback) {
    if(auto* client=existing(path)) client->formatDocument(path,tabSize,spaces,std::move(callback)); else if(callback) callback(QJsonValue(QJsonValue::Undefined));
}
void LspManager::semanticTokens(const QString& path,Callback callback,const QJsonObject& range) {
    if(auto* client=existing(path)) client->semanticTokens(path,std::move(callback),range); else if(callback) callback({});
}

void LspManager::documentSymbols(const QString& path,Callback callback) {
    if(auto* client=existing(path)) client->documentSymbols(path,std::move(callback));
    else if(callback) callback(QJsonValue(QJsonValue::Undefined));
}
