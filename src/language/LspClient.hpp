#pragma once
#include <QObject>
#include <QProcess>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QMap>
#include <QStringList>
#include <QSet>
#include <QTimer>
#include <functional>

// One protocol client per server/root. LspManager routes languages above this
// class without coupling the transport to a particular language server.
struct LspServerConfig {
    QString name{"clangd"};
    QString command;
    QStringList arguments{"--background-index"};
    QMap<QString, QString> languageIds{{"c", "c"}, {"h", "cpp"}, {"cpp", "cpp"},
        {"cc", "cpp"}, {"cxx", "cpp"}, {"hpp", "cpp"}, {"hh", "cpp"}, {"hxx", "cpp"}, {"ino", "cpp"}};
    QJsonObject initializationOptions;
    QJsonObject settings;
    QMap<QString, QString> fileNames;
    bool acceptAnyLanguage{false};
    int logLevel{1}; // 0: off, 1: bounded events/errors, 2: bounded protocol trace.
    static LspServerConfig clangd();
};
class LspClient final : public QObject {
    Q_OBJECT
public:
    explicit LspClient(QObject* parent = nullptr);
    explicit LspClient(const LspServerConfig& config, QObject* parent = nullptr);
    ~LspClient() override;
    void configure(const LspServerConfig& config);
    void restart();
    void checkConnection();
    void setLogLevel(int level);
    void clearLog();
    QJsonObject capabilities() const { return capabilities_; }
    static constexpr int NormalLogBytes = 16 * 1024;
    static constexpr int DetailedLogBytes = 1024 * 1024;
    void setRoot(const QString& root);
    bool supports(const QString& path) const;
    void open(const QString& path, const QString& text, const QString& languageId = {});
    void change(const QString& path, const QString& text);
    void saved(const QString& path);
    void close(const QString& path);
    void completion(const QString& path, int line, int character, std::function<void(QJsonValue)> callback);
    void definition(const QString& path, int line, int character, std::function<void(QJsonValue)> callback);
    void references(const QString& path, int line, int character, std::function<void(QJsonValue)> callback);
    void typeDefinition(const QString& path, int line, int character, std::function<void(QJsonValue)> callback);
    void declaration(const QString& path, int line, int character, std::function<void(QJsonValue)> callback);
    void hover(const QString& path, int line, int character, std::function<void(QJsonValue)> callback);
    void signatureHelp(const QString& path, int line, int character, std::function<void(QJsonValue)> callback);
    bool semanticTokensAvailable() const;
    bool semanticTokensRangeAvailable() const;
    bool semanticTokensFullAvailable() const;
    void cancelSemanticTokens(const QString& path);
    void semanticTokens(const QString& path,std::function<void(QJsonValue)> callback,const QJsonObject& range = {});
    bool documentSymbolsAvailable() const;
    void documentSymbols(const QString& path, std::function<void(QJsonValue)> callback);
    bool canFormat() const;
    void formatDocument(const QString& path, int tabSize, bool insertSpaces, std::function<void(QJsonValue)> callback);
    bool canRename() const;
    void prepareRename(const QString& path, int line, int character, std::function<void(QJsonValue)> callback);
    void renameSymbol(const QString& path, int line, int character, const QString& newName, std::function<void(QJsonValue)> callback);
    bool available() const { return !config_.command.isEmpty(); }
    bool ready() const { return initialized_; }
    QString status() const { return status_; }
    QString log() const;
    int documentVersion(const QString& path) const { return versions_.value(path); }
signals:
    void diagnostics(const QString& path, const QJsonArray& items);
    void error(const QString& message);
    void statusChanged(const QString& status);
    void semanticTokensRefresh();
private:
    struct Pending {
        std::function<void(QJsonValue)> callback;
        QString path;
        QString method;
        int version{};
        qint64 deadline{};
        qint64 startedAt{};
    };
    void start();
    void stop();
    void setStatus(const QString& status);
    void initialize();
    void cancel(const QString& path, const QString& method = {});
    void positionRequest(const QString& method, const QString& path, int line, int character, std::function<void(QJsonValue)> callback);
    void send(QJsonObject message);
    void notify(const QString& method, QJsonObject params);
    void request(const QString& method, QJsonObject params, std::function<void(QJsonValue)> callback, const QString& path = {});
    void readMessages();
    void dispatch(const QJsonObject& message);
    QJsonObject documentId(const QString& path) const;
    void sendOpen(const QString& path);
    void appendLog(const QString& message, bool failure = false);
    void traceMessage(const QString& direction, const QJsonObject& message);
    QString languageId(const QString& path) const;
    QProcess process_;
    QTimer deadlines_;
    QByteArray input_;
    LspServerConfig config_;
    QString root_, status_{"Stopped"};
    QByteArray eventLog_, errorLog_, traceLog_;
    bool checking_{false};
    QHash<QString, QString> documentLanguages_;
    QJsonObject capabilities_;
    bool initialized_{false};
    bool stopping_{false};
    bool destroying_{false};
    int shutdownId_{-1};
    bool shutdownAcknowledged_{false};
    int syncKind_{1};
    int restarts_{0};
    int nextId_{1};
    QHash<int, Pending> callbacks_;
    QHash<QString, QString> documents_;
    QHash<QString, int> versions_;
    QSet<QString> opened_;
};
