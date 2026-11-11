#pragma once
#include <QWidget>
#include "DebugState.hpp"
#include "DebugConfiguration.hpp"
#include <QMap>
#include <QSet>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>
class QPersistentModelIndex;
class DapClient;
class QProcess;
class QTcpServer;
class QComboBox;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QToolButton;
class QPushButton;
class QFileSystemWatcher;
class DebugPanel final : public QWidget {
    Q_OBJECT
public:
    explicit DebugPanel(QWidget* parent=nullptr);
    ~DebugPanel() override;
    QString configurationPath() const { return DebugConfiguration::filePath(workspace_); }
    void editConfiguration();
    void setWorkspace(const QString& root);
    void renameSource(const QString& oldPath,const QString& newPath);
    QSet<int> disabledBreakpoints(const QString& path) const {return state_.disabled.value(path);}
    void remapBreakpoints(const QString& path,const QMap<int,int>& anchors);
    void moveBreakpoints(const QString& path,int row,int column,int removed,int inserted);
    QWidget* console() const { return console_; }
    void setTerminalAvailable(bool available) { terminalAvailable_=available; }
    void start();
    void terminalReply(const QJsonObject& request,bool success,const QJsonObject& body={},const QString& message={});
    void perform(const QString& command);
    void useFile(const QString& path);
    void toggleBreakpoint(const QString& path,int line);
    QSet<int> breakpoints(const QString& path) const { return state_.points.value(path); }
    bool active() const;
signals:
    void configurationRequested(const QString& path);
    void consoleRequested();
    void terminalRequested(const QJsonObject& request);
    void terminalStopRequested();
    void startRequested();
    void currentFileRequested();
    void breakpointRequested();
    void sourceActivated(const QString& path,int line);
    void locationChanged(const QString& path,int line);
    void breakpointsChanged(const QString& path);
private:
    void refreshConfiguration();
    QComboBox* configurations_{nullptr};
    QJsonObject configurationTemplate() const;
    void saveState();
    void loadState();
    QString settingsKey() const;
    void refreshWatches();
    void initContextMenus();
    void valueMenu(QTreeWidget* tree,const QPoint& point);
    void editWatch(const QString& expression={});
    void setValue(QTreeWidget* tree,const QPersistentModelIndex& index);
    bool canSetValue(const QPersistentModelIndex& index) const;
    QJsonObject formatArguments(QJsonObject arguments) const;
    bool hexValues_{false};
    QList<QToolButton*> formatButtons_;
    void updateFormatButtons();
    quint64 formatRequestGeneration_{0};
    QHash<QString,bool> valueExpansion_;
    QString valuePath(QTreeWidgetItem* item) const;
    void restoreValueExpansion(QTreeWidgetItem* item,bool initiallyExpanded=false);
    bool cortexValueFormat() const;
    bool canFormatValues() const;
    void setHexValues(bool hex);
    void evaluate(const QString& expression);
    void launch();
    void startAdapter();
    void prepareNext();
    void finishPreparation(const QString& error={});
    void stopPreparation();
    QProcess* preparation_{nullptr};
    QTcpServer* serverConsole_{nullptr};
    int preparationIndex_{0};
    bool preparing_{false};
    void configure();
    void sendBreakpoints(const QString& path,std::function<void()> done={});
    void event(const QString& name,const QJsonObject& body);
    void setStatus(const QString& text);
    void setState(const QString& text,bool stopped=false);
    void stack();
    void scopes(int frame);
    void variables(QTreeWidgetItem* parent,int reference);
    void clearLocation();
    void rebuildBreakpoints();
    void applyBreakpoint(const QString& path,int requested,const QJsonObject& breakpoint);
    void append(const QString& text);
    DapClient* client_;
    QToolButton* settingsButton_;
    QPushButton* configureButton_;
    QFileSystemWatcher* configurationWatcher_;
    DebugConfiguration configuration_;
    QString currentFile_;
    QLabel* status_;
    QWidget* debugToolbar_{nullptr};
    QWidget* console_;
    QLineEdit* evaluate_;
    QTreeWidget* watches_;
    QPlainTextEdit* output_;
    QTreeWidget *variables_,*frames_,*breakpoints_;
    QToolButton *start_,*stop_,*pause_,*resume_,*next_,*stepIn_,*stepOut_;
    DebugState state_;
    QString workspace_,pendingWorkspace_;
    bool workspacePending_{false},terminalAvailable_{false};
    DebugSessionState session_;
};
