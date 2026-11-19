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
class WorkspaceIndex;
class ProjectExplorer;
class EditorWidget;
namespace VimEx {struct Invocation;}
class QLabel;
class QTabWidget;
class QTabBar;
class EditorPane;
class EditorPaneHeader;
class EditorPaneWorkspace;
class Document;
class VimDiffSession;
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
    void requestClose(QVector<QPointer<EditorWidget>> editors, bool window, bool discard = false,
                      bool inactivePreview = false);
    void advanceClose();
    void finishClose(bool accepted);
    void removeClosedEditor(EditorWidget* editor);
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
    bool closingInactivePreview_{false};
    QHash<EditorWidget*,quint64> discardedRevisions_;
    QHash<EditorWidget*,quint64> recoveredRevisions_;
    QSet<QString> recoveredSourceDirectories_;
    std::vector<std::shared_ptr<QLockFile>> restorationLocks_;
    bool recoveryMigrationFailed_{false};

    void initializeFileMonitoring();
    void checkExternalFileChanges();
    bool fileCheckRunning_{false};
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
    void applyVimSettings();
    void updateVimIndicator();
    QLabel* vimStatus_{nullptr};
    void scheduleOutline();
    void refreshOutline();
    OutlinePanel* outline_{nullptr};
    QTimer* outlineTimer_{nullptr};
    QPointer<EditorWidget> outlineEditor_;
    quint64 outlineGeneration_{0};
    void openFile();
    WorkspaceIndex* workspaceIndex_{nullptr};
    void quickOpen();
    void commandPalette();
    void goToLine();
    void rememberRecentFile(const QString& path);
    void recordNavigationLocation();
    void commitPendingNavigationLocation();
    void navigateHistory(int direction);
    void switchRecentTab(int direction);
    void finishTabSwitch(bool accept);
    void positionTabSwitcher();
    bool closeTab(int index);
    void closeAllTabs();
    void updateTabMru(QWidget* current);
    void restoreNavigationLocation(const QString& path, int line, int column);
    void openFolder();
    void showProjectSearch(const QString& directory = {});
    void startProjectReplace(ProjectReplaceRequest request);
    void finishProjectReplace(const ProjectReplaceResult& result);
    void setProjectRoots(const QStringList& roots, bool explicitProject = true);
    enum class FileOpenMode { Regular, Preview };
    void openFile(const QString& path, int line = 0, int column = 0, int length = 0,
                  bool centerTarget = true, FileOpenMode mode = FileOpenMode::Regular);
    EditorWidget* createEditor(const QString& path, FileOpenMode mode = FileOpenMode::Regular);
    void pinFilePreview(EditorWidget* editor);
    void scheduleFilePreviewCleanup();
    void closeInactiveFilePreviews();
    EditorWidget* createUntitledEditor();
    EditorWidget* openVimFile(const QString& path);
    void connectEditor(EditorWidget* editor, const QString& path, bool primary);
    EditorWidget* activeEditor() const;
    EditorWidget* documentEditor(const EditorWidget* view) const;
    EditorWidget* documentEditor(const Document* document) const;
    EditorWidget* activateEditor(EditorWidget* editor, bool focus = false);
    EditorWidget* showBufferInPane(EditorPane* pane, EditorWidget* source);
    void updateWorkspaceFocus(bool focus);
    void updateEditorPaneActions();
    void updateEditorHeaders();
    void connectEditorHeader(EditorPaneHeader* header, EditorPane* pane = nullptr);
    void showPaneBuffers(EditorPane* pane, const QPoint& position);
    void showPaneActions(EditorPane* pane, const QPoint& position);
    QString paneBufferPath(const EditorWidget* editor) const;
    bool keepOnlyEditorPane(EditorWidget* editor);
    void viewFocused(EditorWidget* editor);
    void initializePaneWorkspace();
    EditorWidget* splitEditor(Qt::Orientation orientation);
    bool closeEditorPane(EditorWidget* editor);
    bool hideEditorView(EditorWidget* editor);
    void removeBufferViews(EditorWidget* editor);
    EditorWidget* alternateBuffer(const EditorWidget* view) const;
    void executeVimWorkspaceKey(EditorWidget* editor, const QString& command, int count, bool explicitCount);
    bool executeVimWorkspaceEx(EditorWidget* editor, const VimEx::Invocation& command,
                               const std::function<void(QString)>& completed);
    VimDiffSession* vimDiffSession(bool create = false);
    void removeVimDiffView(EditorWidget* editor);
    QString transferVimDiff(EditorWidget* editor, bool put, int first, int last, bool range, const QString& buffer);
    EditorPaneWorkspace* paneWorkspace_{nullptr};
    QTabBar* vimPageTabs_{nullptr};
    bool updatingWorkspace_{false};
    struct DeletedVimBuffer { quint64 id; QString path; };
    QVector<DeletedVimBuffer> unloadedVimBuffers_;
    QPointer<EditorWidget> closingVimBuffer_;
    bool wipeVimBuffer_{false};
    quint64 vimNextBufferId_{1};
    QPointer<EditorWidget> vimLastEditor_,vimAlternateEditor_;
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
    void writeEditor(EditorWidget* editor,const QString& path,const QByteArray& expected,std::function<void(bool)> completion,bool reportDialog=true);
    void executeVimEx(EditorWidget* editor,const VimEx::Invocation& command,std::function<void(QString)> completed);
    void saveAllFiles();
    void saveCurrentFileAs();
    void updateWindowTitle();
    enum class SettingsPage { Last, Editor, Appearance, Theme, Shortcuts, Highlighting, LanguageServers, Files, History };
    void showSettings(SettingsPage page = SettingsPage::Last);
    QWidget* createHighlightingSettings(QWidget* parent);
    void applyLspSettings(bool serverChanged);
    void applyFileVisibilitySettings();
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
    [[nodiscard]] QString activeExplorerPath() const;
    void syncExplorerToActiveFile();
    bool autoRevealActiveFile_{false};
    QToolButton* explorerActivity_{nullptr};
    QToolButton* searchActivity_{nullptr};
    QStringList projectRoots_;
    bool explicitProject_{false};
    QTabWidget* tabs_{nullptr};
    QSet<EditorWidget*> filePreviews_;
    bool filePreviewCleanupScheduled_{false};
    EditorPaneHeader* singleEditorHeader_{nullptr};
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
    QFrame* tabSwitcher_{nullptr};
    QListWidget* tabSwitchList_{nullptr};
    QPointer<QWidget> tabSwitchFocus_;
};
