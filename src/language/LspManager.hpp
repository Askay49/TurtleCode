#pragma once
#include "LspClient.hpp"
#include <QPointer>
#include <QVector>
#include <QDir>

struct LspProfile {
    QString id;
    LspServerConfig server;
    bool enabled{true};
};
struct LspConfiguration {
    QVector<LspProfile> profiles;
    QMap<QString, QString> bindings;
    QMap<QString, QString> languageIds;
    static LspConfiguration load();
    QJsonObject json() const;
    void save() const;
};

// Owns one client per configured profile/workspace root. Document routes are
// stable until close/reconfigure; switching tabs never switches a process.
class LspManager final : public QObject {
    Q_OBJECT
public:
    explicit LspManager(QObject* parent = nullptr);
    ~LspManager() override;
    void configure(const LspConfiguration& configuration);
    void setRoot(const QString& root);
    QString root() const { return root_.isEmpty() ? QDir::currentPath() : root_; }
    bool supports(const QString& path) const;
    bool available() const;
    bool ready(const QString& path) const;
    bool serves(LspClient* client, const QString& path) const { return existing(path)==client; }
    QString status(const QString& path = {}) const;
    QString profileFor(const QString& path) const;
    QString protocolLanguage(const QString& path) const;
    LspClient* clientFor(const QString& path);
    QVector<LspClient*> clients(const QString& profile) const;
    LspClient* checkConnection(const QString& profile);
    void restart(const QString& profile);
    void clearLog(const QString& profile);
    QString log(const QString& profile) const;
    void open(const QString& path, const QString& text);
    void change(const QString& path, const QString& text);
    void saved(const QString& path);
    void close(const QString& path);
    int documentVersion(const QString& path) const;
    int documentVersionForServer(const QString& source, const QString& target) const;
    bool documentSymbolsAvailable(const QString& path) const;
    void documentSymbols(const QString& path, std::function<void(QJsonValue)> callback);
    bool canFormat(const QString& path) const;
    bool canRename(const QString& path) const;
    bool semanticTokensAvailable(const QString& path) const;
    bool semanticTokensRangeAvailable(const QString& path) const;
    bool semanticTokensFullAvailable(const QString& path) const;
    void cancelSemanticTokens(const QString& path);
    using Callback = std::function<void(QJsonValue)>;
    void completion(const QString& path, int line, int column, Callback callback);
    void definition(const QString& path, int line, int column, Callback callback);
    void references(const QString& path, int line, int column, Callback callback);
    void typeDefinition(const QString& path, int line, int column, Callback callback);
    void declaration(const QString& path, int line, int column, Callback callback);
    void hover(const QString& path, int line, int column, Callback callback);
    void signatureHelp(const QString& path, int line, int column, Callback callback);
    void prepareRename(const QString& path, int line, int column, Callback callback);
    void renameSymbol(const QString& path, int line, int column, const QString& name, Callback callback);
    void formatDocument(const QString& path, int tabSize, bool spaces, Callback callback);
    void semanticTokens(const QString& path, Callback callback, const QJsonObject& range = {});
    const LspConfiguration& configuration() const { return configuration_; }
signals:
    void statusChanged(const QString& status);
    void clientStateChanged(LspClient* client);
    void diagnostics(const QString& path, const QJsonArray& items);
    void error(const QString& message);
    void semanticTokensRefresh(LspClient* client);
private:
    struct Instance { QString profile, root; LspClient* client; };
    LspClient* createClient(const LspProfile& profile, const QString& root);
    const LspProfile* profile(const QString& id) const;
    LspClient* existing(const QString& path) const;
    QString rootFor(const QString& path) const;
    LspConfiguration configuration_;
    QVector<Instance> instances_;
    QHash<QString, QPointer<LspClient>> documents_;
    QString root_;
    bool enabled_{true};
};
