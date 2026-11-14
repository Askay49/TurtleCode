#pragma once

#include <QMainWindow>
#include <functional>
#include <QStringList>
#include <QPointer>
#include <QList>
#include <QVector>
#include <QHash>
#include <QJsonArray>
#include <memory>
#include <vector>
#include <QSet>
#include <QJsonObject>

class OutlinePanel;
class QLockFile;
class RecoveryStore;
class ProjectExplorer;
class EditorWidget;
class QLabel;
class QTabWidget;
class EditorTabs;
class QCloseEvent;
class GrammarManager;
class LspManager;
class SearchPanel;
class GitPanel;
class DebugPanel;
class GitDiffView;
class TerminalPanel;
class QStackedWidget;
class QWidget;
class QToolButton;
class QTimer;
class QFileSystemWatcher;
class QMessageBox;
class QFrame;
class QListWidget;
struct ProjectReplaceRequest;
struct ProjectReplaceResult;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    void openPaths(const QStringList& paths);
    void restorePreviousSession(bool restoreClean = true);

protected:
    void closeEvent(QCloseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void createUi();
    void requestClose(QVector<QPointer<EditorWidget>> editors, bool window);
    void advanceClose();
    void finishClose(bool accepted);
    void checkpointRecovery();
    QJsonObject sessionState(bool clean = false) const;
    void initializeRecovery();
    void applyRecoverySettings();
    std::shared_ptr<RecoveryStore> recovery_;
    QTimer* recoveryTimer_{nullptr};
    QTimer* recoveryDeadline_{nullptr};
    bool recoveryWriting_{false};
    bool recoveryStopped_{false};
    bool closing_{false};
    bool closingWindow_{false};
    bool allowWindowClose_{false};
    int closingIndex_{0};
    QVector<QPointer<EditorWidget>> closingEditors_;
    QHash<EditorWidget*,quint64> discardedRevisions_;
    QHash<EditorWidget*,quint64> recoveredRevisions_;
    QSet<QString> recoveredSourceDirectories_;
    std::vector<std::shared_ptr<QLockFile>> restorationLocks_;
    bool recoveryMigrationFailed_{false};

    void initializeFileMonitoring();
    void checkExternalFileChanges();
    void reloadExternalFile(EditorWidget* editor, const QString& path, const QByteArray& state);
    QFileSystemWatcher* documentWatcher_{nullptr};
    QTimer* documentChangeTimer_{nullptr};
    QPointer<QMessageBox> externalChangeDialog_;
    struct ExternalFileState {
        QString path;
        QByteArray observed;
        QByteArray acknowledged;
        qint64 observedAt{0};
    };
    QHash<EditorWidget*, ExternalFileState> externalFileStates_;
    QSet<EditorWidget*> externallyReloading_;

    void createMenus();
    void scheduleOutline();
    void refreshOutline();
    OutlinePanel* outline_{nullptr};
    QTimer* outlineTimer_{nullptr};
    QPointer<EditorWidget> outlineEditor_;
    quint64 outlineGeneration_{0};
    void openFile();
    void quickOpen();
    void goToLine();
    void rememberRecentFile(const QString& path);
    void recordNavigationLocation();
    void commitPendingNavigationLocation();
    void navigateHistory(int direction);
    void switchRecentTab(int direction);
    void finishTabSwitch(bool accept);
    void positionTabSwitcher();
    void alignCurrentTabTitle();
    bool closeTab(int index);
    void closeAllTabs();
    void updateTabMru(QWidget* current);
    void restoreNavigationLocation(const QString& path, int line, int column);
    void openFolder();
    void searchInProject();
    void startProjectReplace(ProjectReplaceRequest request);
    void finishProjectReplace(const ProjectReplaceResult& result);
    void setProjectRoots(const QStringList& roots, bool explicitProject = true);
    void openFile(const QString& path, int line = 0, int column = 0, int length = 0,
                  bool centerTarget = true);
    EditorWidget* createEditor(const QString& path);
    void formatDocument();
    void renameSymbol();
    void navigateSymbol(const QString& method);
    bool renameInProgress_{false};
    void switchGitBranch(const QString& repository, const QString& branch, quint64 generation);
    void finishGitBranchSwitch(const QString& repository);
    QVector<QPointer<EditorWidget>> branchLockedEditors_;
    QString branchRepository_;
    bool gitDiscardInProgress_{false};
    void stageGitPaths(const QString& repository, const QStringList& paths, quint64 generation);
    void saveCurrentFile();
    void saveEditor(EditorWidget* editor, std::function<void(bool)> completion = {}, bool saveAs = false);
    void writeEditor(EditorWidget* editor,const QString& path,const QByteArray& expected,std::function<void(bool)> completion);
    void saveAllFiles();
    void saveCurrentFileAs();
    void updateWindowTitle();
    void showLanguageSettings();
    void showLspSettings();
    void showEditorSettings();
    void showInterfaceLanguageSettings();
    void configureLsp();
    bool ensureManualLsp(EditorWidget* editor,std::function<void()> retry);
    void requestSemanticHighlighting(EditorWidget* editor, bool force = false);
    void requestCompletion(EditorWidget* editor, bool automatic);
    void showCodeHelp(bool signature);
    void showLspProblems();
    QHash<QString, QJsonArray> lspProblems_;
    QLabel* lspStatus_{nullptr};

    ProjectExplorer* explorer_{nullptr};
    SearchPanel* searchPanel_{nullptr};
    GitPanel* gitPanel_{nullptr};
    DebugPanel* debugPanel_{nullptr};
    QToolButton* debugActivity_{nullptr};
    QString debugPath_;
    int debugLine_{0};
    GitDiffView* gitDiff_{nullptr};
    QString pendingGitRevert_;
    QMainWindow* editorArea_{nullptr};
    void ensureTerminal();
    TerminalPanel* terminalPanel_{nullptr};
    QPointer<QWidget> terminalPreviousFocus_;
    QToolButton* gitActivity_{nullptr};
    QStackedWidget* sidebarStack_{nullptr};
    QWidget* sidebar_{nullptr};
    void setSidebarVisible(bool visible);
    void updateSidebarActivities();
    QToolButton* explorerActivity_{nullptr};
    QToolButton* searchActivity_{nullptr};
    QStringList projectRoots_;
    bool explicitProject_{false};
    EditorTabs* tabs_{nullptr};
    GrammarManager* grammarManager_{nullptr};
    LspManager* lsp_{nullptr};
    QList<QPointer<QWidget>> replaceLockedEditors_;
    struct NavigationLocation {
        QString path;
        int line{1};
        int column{1};
        bool operator==(const NavigationLocation&) const = default;
    };
    QTimer* navigationPollTimer_{nullptr};
    QTimer* navigationCommitTimer_{nullptr};
    QVector<NavigationLocation> navigationHistory_;
    NavigationLocation lastObservedLocation_;
    NavigationLocation pendingNavigationLocation_;
    int navigationHistoryIndex_{-1};
    bool hasObservedNavigationLocation_{false};
    bool restoringNavigation_{false};
    QVector<QPointer<QWidget>> tabMru_;
    QVector<QPointer<QWidget>> tabSwitchOrder_;
    int tabSwitchIndex_{-1};
    QFrame* tabSwitcher_{nullptr};
    QListWidget* tabSwitchList_{nullptr};
    QPointer<QWidget> tabSwitchFocus_;
};
