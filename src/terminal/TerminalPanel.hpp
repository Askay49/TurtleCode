#pragma once
#include <QDockWidget>
#include <QJsonObject>
class QTabWidget;
class TerminalWidget;
class TerminalSession;
class TerminalPanel final : public QDockWidget {
    Q_OBJECT
public:
    explicit TerminalPanel(QWidget* parent = nullptr);
    void open(const QString& directory);
    void setDirectory(const QString& directory);
    void shutdown();
    void setDebugConsole(QWidget* console);
    void showDebugConsole();
    void runDebugProgram(const QJsonObject& request);
    void stopDebugProgram();
signals:
    void debugTerminalReply(const QJsonObject& request,bool success,const QJsonObject& body,const QString& message);
public:
    TerminalWidget* terminal() const { return terminal_; }
    TerminalSession* session() const { return session_; }
private:
    void startSession();
    QTabWidget* tabs_;
    TerminalWidget* debugTerminal_;
    TerminalSession* debugSession_;
    QWidget* debugConsole_{nullptr};
    QJsonObject pendingRequest_;
    TerminalWidget* terminal_;
    TerminalSession* session_;
    QString directory_;
    bool restartPending_{false};
    bool shuttingDown_{false};
};
