#pragma once
#include <QDockWidget>
#include <QHash>
#include <QJsonObject>
class QLabel;
class QTabWidget;
class QToolButton;
class TerminalWidget;
class TerminalSession;
class TerminalPanel final : public QDockWidget {
    Q_OBJECT
public:
    explicit TerminalPanel(QWidget* parent = nullptr);
    ~TerminalPanel() override;
    void open(const QString& directory);
    void openNewSession(const QString& directory);
    // Only the primary shell follows the project directory.
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
    TerminalSession* session() const;
private:
    struct Shell {
        TerminalSession* session;
        QString directory;
        bool restartPending{false};
    };
    TerminalWidget* addShell(const QString& title, const QString& directory);
    void showShell(TerminalWidget* terminal);
    void startSession(TerminalWidget* terminal);
    void restartSession();
    void closeCurrentSession();
    void updateShellActions();

    QTabWidget* tabs_;
    QLabel* shellIcon_;
    QLabel* shellName_;
    QToolButton* restart_;
    QToolButton* closeSession_;
    TerminalWidget* debugTerminal_;
    TerminalSession* debugSession_;
    QWidget* debugConsole_{nullptr};
    QJsonObject pendingRequest_;
    TerminalWidget* terminal_{nullptr};
    QHash<QWidget*, Shell> shells_;
    bool shuttingDown_{false};
};
