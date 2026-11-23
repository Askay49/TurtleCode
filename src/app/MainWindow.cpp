#include "CommandPalette.hpp"
#include "app/IconDialogs.hpp"
#include "document/RecoveryStore.hpp"
#include "document/FileState.hpp"
#include <QCloseEvent>
#include <QUuid>
#include <QLockFile>
#include <QScrollBar>
#include <QScrollArea>
#include "app/FileIcons.hpp"
#include "app/Codicons.hpp"
#include "app/I18n.hpp"
#include "terminal/TerminalPanel.hpp"
#include "terminal/TerminalWidget.hpp"
#include "ShortcutManager.hpp"
#include "ThemeManager.hpp"
#include "WorkspaceBackground.hpp"
#include <QPainter>
#include "MainWindow.hpp"

#include <QAction>
#include <QDateTime>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QTreeWidget>
#include "language/WorkspaceEdits.hpp"
#include <QCursor>
#include <QTableWidget>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QStandardPaths>
#include <QToolTip>
#include <QApplication>
#include <QFileDialog>
#include <QHeaderView>
#include <QMessageBox>
#include <QMenuBar>
#include <QStatusBar>
#include <QKeySequence>
#include <QShortcut>
#include <QDir>
#include <QSplitter>
#include <QTabWidget>
#include <QTabBar>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QAbstractItemModel>
#include <QFrame>
#include <QFileInfo>
#include <QFile>
#include <QFileSystemWatcher>
#include <QLineEdit>
#include <QInputDialog>
#include <QDialog>
#include <QSettings>
#include <QListWidget>
#include <QFutureWatcher>
#include <QPromise>
#include <QProgressDialog>
#include <QtConcurrent/QtConcurrentRun>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QLabel>
#include <QFont>
#include <QVBoxLayout>
#include <QPair>
#include <QVector>
#include <QScrollArea>
#include <QPushButton>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <utility>
#include <QSignalBlocker>
#include <QPointer>
#include <QSet>
#include <QSaveFile>
#include <QStackedWidget>
#include <QToolButton>
#include <QTimer>
#include <QMenu>
#include <QUrl>
#include <memory>
#include <atomic>
#include <limits>
#include <algorithm>

#include "editor/EditorWidget.hpp"
#include "editor/OutlinePanel.hpp"
#include "editor/EditorPaneWorkspace.hpp"
#include "project/ProjectExplorer.hpp"
#include "project/WorkspaceIndex.hpp"
#include "search/SearchPanel.hpp"
#include "git/GitPanel.hpp"
#include "debug/DebugPanel.hpp"
#include "git/GitDiffView.hpp"
#include "language/LanguageRegistry.hpp"
#include "language/GrammarManager.hpp"
#include "language/LspManager.hpp"

namespace {

constexpr auto focusNavigationHint = "h Tree · j Terminal · k/l Editor · Esc Cancel";

struct LoadedFile {
    std::shared_ptr<PieceTreeBuffer> buffer;
    QString error;
    bool preview{false};
    QByteArray diskState;
};

struct SavedFile {
    bool ok{false};
    QByteArray diskState;
    QString error;
};


}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setObjectName("workspaceBackground");
    workspaceIndex_ = new WorkspaceIndex(this);
    createUi();
    lsp_ = new LspManager(this);
    lspStatus_ = new QLabel(this);
    statusBar()->addPermanentWidget(lspStatus_);
    vimStatus_=new QLabel(this);vimStatus_->setObjectName("vimModeIndicator");
    vimStatus_->setMinimumWidth(std::max({vimStatus_->fontMetrics().horizontalAdvance("NORMAL"),vimStatus_->fontMetrics().horizontalAdvance("INSERT"),vimStatus_->fontMetrics().horizontalAdvance("VISUAL")})+12);
    vimStatus_->setAlignment(Qt::AlignCenter);
    vimStatus_->setToolTip(I18n::text("Vim mode for the current editor"));
    statusBar()->addPermanentWidget(vimStatus_);vimStatus_->hide();
    connect(qApp,&QApplication::focusChanged,this,[this](QWidget*,QWidget*){updateVimIndicator();});
    connect(lsp_, &LspManager::statusChanged, this, [this](const QString&) {
        auto* editor=activeEditor();
        lspStatus_->setText(lsp_->status(editor?editor->filePath():QString{}));
    });
    connect(lsp_, &LspManager::clientStateChanged, this, [this](LspClient* client) {
        scheduleOutline();
        for(int i=0;i<tabs_->count();++i) if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i))) {
            if(!lsp_->serves(client,editor->filePath())) continue;
            if(!client->ready()) {editor->clearSemanticHighlighting();editor->lspPolicy().reset();editor->setProperty("semanticPending",false);}
            else if(activeEditor()==editor) requestSemanticHighlighting(editor);
        }
    });
    connect(lsp_, &LspManager::semanticTokensRefresh, this, [this](LspClient* client) {
        auto* editor=activeEditor();
        if(editor && lsp_->serves(client,editor->filePath())) requestSemanticHighlighting(editor,true);
    });
    configureLsp();
    lspStatus_->setText(lsp_->status());
    connect(lsp_, &LspManager::diagnostics, this, [this](const QString& path, const QJsonArray& items) {
        if (items.isEmpty()) lspProblems_.remove(path); else lspProblems_[path] = items;
        for (auto* editor : findChildren<EditorWidget*>())
            if (editor->filePath() == path) editor->setDiagnostics(items);
    });
    connect(lsp_, &LspManager::error, this, [this](const QString& message) {
        statusBar()->showMessage(message, 6000);
    });
    focusNavigationTimer_ = new QTimer(this);
    focusNavigationTimer_->setSingleShot(true);
    connect(focusNavigationTimer_, &QTimer::timeout, this, &MainWindow::finishFocusNavigation);
    createMenus();
    navigationPollTimer_ = new QTimer(this);
    navigationPollTimer_->setInterval(250);
    connect(navigationPollTimer_, &QTimer::timeout, this, [this] {
        if (restoringNavigation_) return;
        auto* editor = activeEditor();
        if (!editor || editor->isBusy() || editor->filePath().isEmpty()) return;
        const QPoint position = editor->navigationPosition();
        const NavigationLocation current{editor->filePath(), position.x(), position.y()};
        if (!hasObservedNavigationLocation_ || !(current == lastObservedLocation_)) {
            lastObservedLocation_ = current;
            hasObservedNavigationLocation_ = true;
            pendingNavigationLocation_ = current;
            navigationCommitTimer_->start();
        }
    });
    navigationCommitTimer_ = new QTimer(this);
    navigationCommitTimer_->setSingleShot(true);
    navigationCommitTimer_->setInterval(650);
    connect(navigationCommitTimer_, &QTimer::timeout, this,
            &MainWindow::commitPendingNavigationLocation);
    navigationPollTimer_->start();
    updateWindowTitle();
    statusBar()->showMessage(I18n::text("Ready"));
    grammarManager_ = new GrammarManager(this);
    connect(grammarManager_, &GrammarManager::statusChanged, this,
            [this](const QString& id, const QString& message) {
        Q_UNUSED(id);
        statusBar()->showMessage(message, 8000);
    });
    connect(grammarManager_, &GrammarManager::installationFinished, this,
            [this](const QString& id, bool installed) {
        if (!installed) return;
        for (auto* editor : findChildren<EditorWidget*>()) editor->refreshSyntaxHighlighting();
        const auto* language = LanguageRegistry::language(id);
        statusBar()->showMessage((language ? language->name : id) + I18n::text(" highlighting is ready"), 5000);
    });
#ifdef TURTLE_CODE_WITH_TREE_SITTER
    grammarManager_->ensureCppInstalled();
#endif
    initializeRecovery();
    initializeFileMonitoring();
}

MainWindow::~MainWindow() {
    finishFocusNavigation();
    qApp->removeEventFilter(this);
    // QObject deletes children after C++ members have been destroyed. Disconnect
    // callbacks and dispose of the editors/services while our state is still alive.
    const auto children = findChildren<QObject*>();
    for (QObject* child : children) {
        QObject::disconnect(child, nullptr, this, nullptr);
        if (auto* timer = qobject_cast<QTimer*>(child)) timer->stop();
    }
    delete takeCentralWidget();
    tabs_ = nullptr;
    paneWorkspace_ = nullptr;
    vimPageTabs_ = nullptr;
    delete lsp_;
    lsp_ = nullptr;
    delete grammarManager_;
    grammarManager_ = nullptr;
}

void MainWindow::paintEvent(QPaintEvent* event) {
    QMainWindow::paintEvent(event);
    QPainter painter(this);
    WorkspaceBackground::instance().paint(this, painter, rect(), ThemeManager::instance().color("sidebar.background"));
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    update();
    for (auto* child : findChildren<QWidget*>()) child->update();
}

void MainWindow::createUi() {
    auto* workspace = new QWidget(this);
    auto* workspaceLayout = new QHBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0,0,0,0);
    workspaceLayout->setSpacing(0);
    auto* splitter = new QSplitter(workspace);
    connect(splitter, &QSplitter::splitterMoved, this, [this] {
        update();
        for (auto* child : findChildren<QWidget*>()) child->update();
    });
    splitter->setChildrenCollapsible(false);
    splitter->setObjectName("workspaceSplitter");
    splitter->setHandleWidth(3);

    sidebar_ = new QWidget(splitter);
    auto* sidebarLayout = new QHBoxLayout(sidebar_);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);
    auto* activityBar = new QWidget(workspace);
    activityBar->setObjectName("activityBar");
    activityBar->setFixedWidth(46);
    auto* activityLayout = new QVBoxLayout(activityBar);
    activityLayout->setContentsMargins(0, 0, 0, 0);
    activityLayout->setSpacing(0);
    explorerActivity_ = new QToolButton(activityBar);
    explorerActivity_->setObjectName("explorerActivity");
    explorerActivity_->setIcon(Codicons::icon("files"));
    explorerActivity_->setIconSize(QSize(24, 24));
    explorerActivity_->setFixedSize(40, 40);
    explorerActivity_->setAccessibleName(I18n::text("Explorer"));
    explorerActivity_->setToolTip(I18n::text("Explorer"));
    explorerActivity_->setCheckable(true);
    explorerActivity_->setChecked(true);
    searchActivity_ = new QToolButton(activityBar);
    searchActivity_->setObjectName("searchActivity");
    searchActivity_->setIcon(Codicons::icon("search"));
    searchActivity_->setIconSize(QSize(24, 24));
    searchActivity_->setFixedSize(40, 40);
    searchActivity_->setAccessibleName(I18n::text("Search"));
    searchActivity_->setToolTip(I18n::text("Search"));
    searchActivity_->setCheckable(true);
    activityLayout->addWidget(explorerActivity_);
    activityLayout->addWidget(searchActivity_);
    gitActivity_ = new QToolButton(activityBar);
    gitActivity_->setObjectName("gitActivity");
    gitActivity_->setIcon(Codicons::icon("source-control"));
    gitActivity_->setIconSize(QSize(24, 24)); gitActivity_->setFixedSize(40, 40);
    gitActivity_->setAccessibleName(I18n::text("Git")); gitActivity_->setToolTip(I18n::text("Git"));
    gitActivity_->setCheckable(true); activityLayout->addWidget(gitActivity_);
    auto* gitCount=new QLabel(gitActivity_); gitCount->setObjectName("gitCount");
    gitCount->setAlignment(Qt::AlignCenter); gitCount->setAttribute(Qt::WA_TransparentForMouseEvents);
    gitCount->setGeometry(25,24,17,16); gitCount->hide();
    debugActivity_ = new QToolButton(activityBar);
    debugActivity_->setObjectName("debugActivity");
    debugActivity_->setIcon(Codicons::icon("debug-alt"));
    debugActivity_->setIconSize(QSize(24,24)); debugActivity_->setFixedSize(40,40);
    debugActivity_->setAccessibleName(I18n::text("Run and Debug"));
    debugActivity_->setToolTip(I18n::text("Run and Debug")); debugActivity_->setCheckable(true);
    activityLayout->addWidget(debugActivity_);
    for (auto* button : {explorerActivity_, searchActivity_, gitActivity_, debugActivity_}) {
        button->setAutoRaise(true);
        button->setFixedSize(46, 46);
    }
    activityLayout->addStretch();

    sidebarStack_ = new QStackedWidget(sidebar_);
    sidebarStack_->setObjectName("sidebarStack");
    explorerPage_ = new QWidget(sidebarStack_);
    auto* explorerLayout = new QVBoxLayout(explorerPage_);
    explorerLayout->setContentsMargins(0, 0, 0, 0);
    explorerLayout->setSpacing(0);
    auto* explorerHeading = new QFrame(explorerPage_);
    explorerHeading->setObjectName("explorerHeading");
    auto* explorerHeader = new QHBoxLayout(explorerHeading);
    explorerHeader->setContentsMargins(12, 5, 6, 5);
    explorerHeader->setSpacing(2);
    auto* explorerTitle = new QLabel(I18n::text("Explorer"), explorerHeading);
    explorerTitle->setObjectName("explorerHeadingTitle");
    explorerTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    explorerHeader->addWidget(explorerTitle, 1);
    explorerLayout->addWidget(explorerHeading);
    explorer_ = new ProjectExplorer(explorerPage_);
    const auto addExplorerButton = [&](const char* name, const char* icon, const char* label,
                                       void (ProjectExplorer::*action)()) {
        auto* button = new QToolButton(explorerHeading);
        button->setObjectName(name);
        button->setIcon(Codicons::icon(icon));
        const QString text = I18n::text(label);
        button->setToolTip(text);
        button->setAccessibleName(text);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::TabFocus);
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(24, 24);
        explorerHeader->addWidget(button);
        connect(button, &QToolButton::clicked, explorer_, action);
        return button;
    };
    auto* newFile = addExplorerButton("explorerNewFile", "new-file", "New File...", &ProjectExplorer::createFile);
    auto* newFolder = addExplorerButton("explorerNewFolder", "new-folder", "New Folder...", &ProjectExplorer::createFolder);
    auto* collapse = addExplorerButton("explorerCollapseAll", "collapse-all", "Collapse all", &ProjectExplorer::collapseFolders);
    const auto updateExplorerButtons = [tree = explorer_, newFile, newFolder, collapse] {
        const bool hasRoot = tree->topLevelItemCount() > 0;
        for (auto* button : {newFile, newFolder, collapse}) button->setEnabled(hasRoot);
    };
    connect(explorer_->model(), &QAbstractItemModel::modelReset, explorerHeading, updateExplorerButtons);
    connect(explorer_->model(), &QAbstractItemModel::rowsInserted, explorerHeading,
            [updateExplorerButtons](const QModelIndex& parent) {
        if (!parent.isValid()) updateExplorerButtons();
    });
    updateExplorerButtons();
    explorer_->setVimEnabled(QSettings().value("editor/vimMode", false).toBool());
    explorer_->setCompactFolders(QSettings().value("view/compactFolders", false).toBool());
    explorer_->setConfirmMoveToTrash(QSettings().value("project/confirmMoveToTrash", true).toBool());
    autoRevealActiveFile_ = QSettings().value("view/autoRevealActiveFile", false).toBool();
    connect(explorer_, &ProjectExplorer::filesChanged, workspaceIndex_, &WorkspaceIndex::invalidate);
    connect(explorer_,&ProjectExplorer::backgroundError,this,[this](const QString& error){statusBar()->showMessage(error,10000);});
    connect(explorer_, &ProjectExplorer::revealFailed, this, [this] {
        statusBar()->showMessage(I18n::text("The active file is unavailable in the project tree."), 5000);
    });
    explorerLayout->addWidget(explorer_, 1);
    searchPanel_ = new SearchPanel(sidebarStack_);
    sidebarStack_->addWidget(explorerPage_);
    sidebarStack_->addWidget(searchPanel_);
    gitPanel_ = new GitPanel(sidebarStack_); sidebarStack_->addWidget(gitPanel_);
    connect(gitPanel_, &GitPanel::discardRequested, this, [this](const QString& repository,const GitEntry& entry) {
        if(closing_ || renameInProgress_ || !replaceLockedEditors_.isEmpty() || !branchLockedEditors_.isEmpty() || gitPanel_->service()->busy()) return;
        QVector<QPointer<EditorWidget>> affected;
        const auto paths=entry.paths();
        for(int i=0;i<tabs_->count();++i) {
            auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i));
            if(!editor || !paths.contains(QDir(repository).relativeFilePath(editor->filePath()))) continue;
            if(editor->isBusy()) { statusBar()->showMessage(I18n::text("File is busy. Try again when its operation finishes."),5000); return; }
            affected.append(editor);
        }
        const auto answer=IconDialogs::MessageBox::question(this,I18n::text("Discard changes…"),
            I18n::text("Discard all unstaged changes to %1, including unsaved edits? The file will be restored from the index.").arg(entry.path),
            QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel);
        if(answer!=QMessageBox::Yes) return;
        // A background refresh may have started while the question was open.
        if(gitPanel_->service()->busy() || gitPanel_->service()->repository()!=repository) return;
        gitDiscardInProgress_=true; branchRepository_=repository; branchLockedEditors_=affected;
        explorerPage_->setEnabled(false); searchPanel_->setEnabled(false);
        for(const auto& editor:affected) if(editor) editor->setBusy(true);
        if(gitDiff_) gitDiff_->hide();
        gitPanel_->service()->discard(entry);
    });
    connect(gitPanel_, &GitPanel::openFileRequested, this, [this](const QString& path) { openFile(path); });
    connect(gitPanel_, &GitPanel::pullRequested, this, [this](const QString& repository) {
        switchGitBranch(repository, QString(), gitPanel_->operationGeneration());
    });
    connect(gitPanel_, &GitPanel::stageRequested, this, [this](const QString& repository, const QStringList& paths) {
        stageGitPaths(repository, paths, gitPanel_->operationGeneration());
    });
    connect(gitPanel_, &GitPanel::branchSwitchRequested, this, [this](const QString& repository, const QString& branch) {
        switchGitBranch(repository, branch, gitPanel_->operationGeneration());
    });
    connect(gitPanel_->service(), &GitService::branchSwitchFinished, this, [this](const QString& repository, bool success) {
        if(gitDiscardInProgress_ && repository==branchRepository_) {
            gitDiscardInProgress_=false;
            if(!success) {
                for(const auto& editor:branchLockedEditors_) if(editor) editor->setBusy(false);
                branchLockedEditors_.clear(); branchRepository_.clear();
                explorerPage_->setEnabled(true); searchPanel_->setEnabled(true); return;
            }
        }
        finishGitBranchSwitch(repository);
    });
    connect(gitPanel_, &GitPanel::countChanged, this, [this](int count) {
        gitActivity_->setToolTip(I18n::text("Git — %1 changed files").arg(count));
        if(auto* badge=gitActivity_->findChild<QLabel*>("gitCount")) {
            badge->setText(count>99?QStringLiteral("99+"):QString::number(count)); badge->setVisible(count>0);
        }
    });
    connect(gitPanel_, &GitPanel::revertRequested, this, [this](const QString& repository, const GitEntry& entry) {
        pendingGitRevert_=repository+"/"+entry.path;
        gitPanel_->service()->diff(entry,false);
    });
    connect(gitPanel_->service(), &GitService::error, this, [this] { pendingGitRevert_.clear(); });
    connect(gitPanel_->service(), &GitService::changed, this, [this] {
        if(gitPanel_->service()->repository().isEmpty()) pendingGitRevert_.clear();
    });
    const auto ensureGitDiff = [this] {
        if (!gitDiff_) {
            gitDiff_ = new GitDiffView(editorArea_->centralWidget());
            connect(gitDiff_, &GitDiffView::visibilityChanged, this, [this, restoreOutline=false](bool visible) mutable {
                if(visible) {
                    restoreOutline=outline_ && outline_->isVisible();
                    if(restoreOutline) outline_->hide();
                } else {
                    if(restoreOutline && outline_) outline_->show();
                    restoreOutline=false;
                }
            });
            connect(gitDiff_, &GitDiffView::fileReverted, this, [this](const QString& path) {
                auto* editor=qobject_cast<EditorWidget*>(gitDiff_->nativeEditor());
                if(editor && editor->filePath()==path) saveEditor(editor);
            });
            connect(gitDiff_, &GitDiffView::refreshRequested, this, [this] {
                if(!gitDiff_ || !gitDiff_->isVisible() || gitDiff_->textOnly() || gitPanel_->service()->busy()) return;
                const auto repository=gitPanel_->service()->repository();
                if(repository.isEmpty() || !gitDiff_->path().startsWith(repository+"/")) return;
                const auto relative=gitDiff_->path().mid(repository.size()+1);
                GitEntry entry; entry.path=relative;
                for(const auto& e:gitPanel_->service()->status().entries) if(e.path==relative) { entry=e; break; }
                gitPanel_->service()->diff(entry,gitDiff_->staged());
            });
            connect(gitPanel_->service(), &GitService::busyChanged, gitDiff_, [this,wasWriting=false](bool busy) mutable {
                const bool writing=gitPanel_->service()->writing();
                gitDiff_->setOperationBusy(writing);
                if(busy && writing) wasWriting=true;
                if(!busy && wasWriting) { wasWriting=false; QTimer::singleShot(0,gitDiff_,&GitDiffView::refreshRequested); }
            });
        }
    };
    const auto diffEditorHost = [this] {
        return paneWorkspace_->count() ? paneWorkspace_->activePane()->viewContainer() : static_cast<QWidget*>(tabs_);
    };
    connect(gitPanel_, &GitPanel::diffReady, this, [this, ensureGitDiff, diffEditorHost](const QString& path, bool staged, const QString& patch) {
        ensureGitDiff();
        gitDiff_->displayPatch(path, staged, patch, diffEditorHost());
    });
    connect(gitPanel_->service(), &GitService::versionsReady, this, [this, ensureGitDiff, diffEditorHost](const GitDiffSnapshot& snapshot) {
        const QString path=snapshot.repository+"/"+snapshot.entry.path;
        const bool revertFile=!snapshot.staged && pendingGitRevert_==path;
        pendingGitRevert_.clear();
        ensureGitDiff();
        if(gitDiff_->isVisible() && !gitDiff_->textOnly() && gitDiff_->path()==path && gitDiff_->staged()==snapshot.staged) {
            gitDiff_->refreshVersions(snapshot); if(revertFile) gitDiff_->revertFile(); return;
        }
        GitDiffView::LiveDocument live;
        if(!snapshot.special) {
            QPointer<EditorWidget> editor;
            for(int i=0;i<tabs_->count();++i) {
                auto* candidate=qobject_cast<EditorWidget*>(tabs_->widget(i));
                if(candidate && (candidate->filePath()==path ||
                    (candidate->isBusy() && candidate->filePath().isEmpty() && candidate->property("openingPath").toString()==path))) { editor=candidate; break; }
            }
            if(!editor && QFileInfo(path).isFile()) {
                openFile(path);
                for(int i=0;i<tabs_->count();++i) {
                    auto* candidate=qobject_cast<EditorWidget*>(tabs_->widget(i));
                    if(candidate && (candidate->filePath()==path ||
                    (candidate->isBusy() && candidate->filePath().isEmpty() && candidate->property("openingPath").toString()==path))) { editor=candidate; break; }
                }
            }
            if(!editor && !QFileInfo::exists(path) && (snapshot.entry.worktree=='D' || snapshot.entry.conflict())) {
                editor=createEditor(path); editor->loadBuffer(PieceTreeBuffer{},path);
                editor->document()->diskState=diskFileState(path); editor->setBusy(false);
                tabs_->setTabText(tabs_->indexOf(editor),QFileInfo(path).fileName());
            }
            if(editor) {
                editor = activateEditor(editor, true);
                removeVimDiffView(editor);
                live.available=[editor,path] { return editor && editor->filePath()==path && !editor->isBusy(); };
                live.revision=[editor] { return editor ? editor->document()->buffer().contentRevision() : 0; };
                live.read=[editor]() -> std::optional<QByteArray> {
                    if (!editor) return std::nullopt;
                    const auto& buffer = editor->document()->buffer();
                    const auto size = buffer.size();
                    if (size > 8 * 1024 * 1024) return std::nullopt;
                    QByteArray bytes;
                    bytes.reserve(qsizetype(size));
                    buffer.forEachChunk([&bytes](std::string_view chunk) {
                        bytes.append(chunk.data(), qsizetype(chunk.size()));
                        return true;
                    });
                    return bytes;
                };
                live.replace=[editor,path](const QByteArray& before,const QByteArray& after) {
                    if(!editor || editor->filePath()!=path || editor->isBusy()) return false;
                    if(before==after) return true;
                    int prefix=0,suffix=0;
                    while(prefix<before.size() && prefix<after.size() && before[prefix]==after[prefix]) ++prefix;
                    while(prefix>0 && ((prefix<before.size() && (uchar(before[prefix])&0xc0)==0x80) ||
                                      (prefix<after.size() && (uchar(after[prefix])&0xc0)==0x80))) --prefix;
                    while(suffix<before.size()-prefix && suffix<after.size()-prefix && before[before.size()-1-suffix]==after[after.size()-1-suffix]) ++suffix;
                    while(suffix>0 && ((uchar(before[before.size()-suffix])&0xc0)==0x80 || (uchar(after[after.size()-suffix])&0xc0)==0x80)) --suffix;
                    std::vector<EditChange> changes{{std::size_t(prefix),before.mid(prefix,before.size()-prefix-suffix).toStdString(),after.mid(prefix,after.size()-prefix-suffix).toStdString()}};
                    if(!editor->validateWorkspaceEdits(before,changes)) return false;
                    editor->applyWorkspaceEdits(std::move(changes)); return true;
                };
                live.editor=editor;
                live.align=[editor](const LineAlignment& alignment) { if(editor) editor->setDiffAlignment(alignment); };
                live.showOverview=[editor](bool visible) { if(editor) editor->setMiniMapSuppressed(visible); };
                live.tabSize=[editor] { return editor ? editor->tabSize() : 4; };
                live.highlight=[editor](const GitDiff::Model* model) {
                    if(!editor) return;
                    QVector<EditorWidget::DiffHighlight> highlights;
                    if(model) for(const auto& hunk:model->hunks) for(int i=0;i<hunk.rightCount;++i) {
                        const int line=hunk.right+i;
                        const auto value=GitDiff::displayLine(model->right[line]);
                        const auto [prefix,suffix]=i<hunk.leftCount
                            ? GitDiff::commonColumns(GitDiff::displayLine(model->left[hunk.left+i]),value)
                            : std::pair{0,0};
                        highlights.append({line,prefix,int(value.size())-prefix-suffix});
                    }
                    editor->setDiffHighlights(highlights);
                };
            }
        }
        gitDiff_->displayVersions(snapshot,std::move(live),diffEditorHost());
        if(revertFile) gitDiff_->revertFile();
    });
    debugPanel_ = new DebugPanel(sidebarStack_); sidebarStack_->addWidget(debugPanel_);
    debugPanel_->setConfigurationWriteCheck([this](const QStringList& paths) {
        for (auto* editor : findChildren<EditorWidget*>()) {
            const auto path = editor->filePath().isEmpty() ? editor->property("openingPath").toString() : editor->filePath();
            if (paths.contains(path) && (editor->isModified() || editor->isBusy())) return false;
        }
        return true;
    });
    connect(debugPanel_, &DebugPanel::startRequested, this, [this] {
        for (int i=0;i<tabs_->count();++i) if(auto* e=qobject_cast<EditorWidget*>(tabs_->widget(i)); e && (e->isModified() || e->isBusy())) {
            statusBar()->showMessage(I18n::text("Save all files and wait for file operations before debugging"),6000); return;
        }
        debugPanel_->start();
    });
    connect(debugPanel_, &DebugPanel::configurationRequested, this, [this](const QString& path){openFile(path);});
    connect(debugPanel_, &DebugPanel::currentFileRequested, this, [this] {
        if(auto* e=activeEditor()) debugPanel_->useFile(e->filePath());
    });
    connect(debugPanel_, &DebugPanel::breakpointRequested, this, [this] {
        if(auto* e=activeEditor()) debugPanel_->toggleBreakpoint(e->filePath(),e->navigationPosition().x());
    });
    connect(debugPanel_, &DebugPanel::breakpointsChanged, this, [this](const QString& path) {
        for(auto* e:findChildren<EditorWidget*>()) if(e->filePath()==path) e->setDebugBreakpoints(debugPanel_->breakpoints(path),debugPanel_->disabledBreakpoints(path));
    });
    connect(debugPanel_, &DebugPanel::sourceActivated, this, [this](const QString& path,int line) { if(QFileInfo(path).isFile()) openFile(path,line); });
    connect(debugPanel_, &DebugPanel::locationChanged, this, [this](const QString& path,int line) {
        if (!debugHoverTip_.isEmpty() && QToolTip::text() == debugHoverTip_) QToolTip::hideText();
        debugHoverTip_.clear();
        debugPath_=path.isEmpty()?QString{}:QDir::cleanPath(QFileInfo(path).absoluteFilePath()); debugLine_=line;
        if(!path.isEmpty() && line>0 && QFileInfo(path).isFile()) openFile(path,line);
        for(auto* e:findChildren<EditorWidget*>()) e->setDebugLine(e->filePath()==debugPath_?line:0);
    });
    sidebarLayout->addWidget(sidebarStack_, 1);

    const auto showSidebarPage = [this](int index) {
        if (!sidebar_->isHidden() && sidebarStack_->currentIndex()==index) {
            setSidebarVisible(false);
            return;
        }
        sidebarStack_->setCurrentIndex(index);
        setSidebarVisible(true);
        if (index == 2) gitPanel_->refresh();
        if (index == 1) searchPanel_->focusSearch();
    };
    connect(explorerActivity_, &QToolButton::clicked, this, [showSidebarPage] { showSidebarPage(0); });
    connect(searchActivity_, &QToolButton::clicked, this, [showSidebarPage] { showSidebarPage(1); });
    connect(gitActivity_, &QToolButton::clicked, this, [showSidebarPage] { showSidebarPage(2); });
    connect(debugActivity_, &QToolButton::clicked, this, [showSidebarPage] { showSidebarPage(3); });
    connect(sidebarStack_, &QStackedWidget::currentChanged, this, [this] {
        updateSidebarActivities();
        syncExplorerToActiveFile();
    });
    connect(explorer_, &ProjectExplorer::fileClicked, this, [this](const QString& path) {
        const auto mode = QSettings().value("editor/previewFiles", false).toBool()
            ? FileOpenMode::Preview : FileOpenMode::Regular;
        openFile(path, 0, 0, 0, true, mode);
    });
    connect(explorer_, &ProjectExplorer::fileActivated, this, [this](const QString& path, bool focusEditor) {
        openFile(path);
        if (auto* editor = activeEditor(); focusEditor && editor)
            editor->setFocus(Qt::ShortcutFocusReason);
    });
    connect(explorer_, &ProjectExplorer::editorFocusRequested, this, &MainWindow::focusEditor);
    connect(explorer_, &ProjectExplorer::searchInFolderRequested, this, &MainWindow::showProjectSearch,
            Qt::QueuedConnection);
    connect(explorer_, &ProjectExplorer::newTerminalRequested, this, [this](const QString& directory) {
        if (closing_) return;
        if (!terminalPanel_ || !terminalPanel_->isVisible())
            terminalPreviousFocus_ = QApplication::focusWidget();
        ensureTerminal();
        terminalPanel_->openNewSession(directory);
    }, Qt::QueuedConnection);
    connect(searchPanel_, &SearchPanel::fileActivated, this,
            [this](const QString& path, int line, int column, int length) {
        openFile(path, line, column, length);
    });
    connect(searchPanel_, &SearchPanel::replaceRequested, this,
            [this](const ProjectReplaceRequest& request) { startProjectReplace(request); });
    connect(searchPanel_, &SearchPanel::replaceFinished, this, &MainWindow::finishProjectReplace);
    connect(explorer_, &ProjectExplorer::rootsChanged, this, [this](const QStringList& roots) {
        projectRoots_ = roots;
        if (roots.isEmpty()) explicitProject_ = false;
        debugPanel_->setWorkspace(roots.value(0));
        searchPanel_->setRoots(roots);
        if (lsp_) lsp_->setRoot(roots.isEmpty() ? QDir::homePath() : roots.front());
        if (terminalPanel_) terminalPanel_->setDirectory(roots.value(0));
        gitPanel_->setRoots(roots);
        if (gitDiff_) gitDiff_->hide();
        updateWindowTitle();
    });
    explorer_->setPathBusyCheck([this](const QString& source) {
        const QString prefix = source + QDir::separator();
        for (int i = 0; i < tabs_->count(); ++i) {
            const auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
            if (!editor || !editor->isBusy()) continue;
            const QString path = editor->filePath().isEmpty()
                ? editor->property("openingPath").toString() : editor->filePath();
            if (path == source || path.startsWith(prefix)) return true;
        }
        return false;
    });
    connect(explorer_, &ProjectExplorer::pathRenamed, this,
            [this](const QString& oldPath, const QString& newPath) {
        debugPanel_->renameSource(oldPath,newPath);
        const QString oldPrefix = oldPath + QDir::separator();
        for (int i = 0; i < tabs_->count(); ++i) {
            auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
            if (!editor) continue;
            const QString currentPath = editor->filePath();
            if (currentPath == oldPath || currentPath.startsWith(oldPrefix)) {
                const QString suffix = currentPath.mid(oldPath.size());
                const QString updatedPath = newPath + suffix;
                if (lsp_) lsp_->close(currentPath);
                editor->updateFilePathAfterRename(updatedPath);
                editor->setDebugBreakpoints(debugPanel_->breakpoints(updatedPath),debugPanel_->disabledBreakpoints(updatedPath));
                if (lsp_ && lsp_->available() && editor->lspAutomaticEligible() && lsp_->supports(updatedPath)) {
                    lsp_->open(updatedPath, editor->lspText());
                    requestSemanticHighlighting(editor);
                }
                const QString tabName = QFileInfo(updatedPath).fileName()
                    + (editor->isModified() ? " •" : "");
                tabs_->setTabText(i, tabName);
                tabs_->setTabToolTip(i, updatedPath);
            }
        }
        updateWindowTitle();
    });
    connect(explorer_, &ProjectExplorer::pathDeleted, this, [this](const QString& deletedPath) {
        const QString prefix = deletedPath + QDir::separator();
        for (int i = tabs_->count() - 1; i >= 0; --i) {
            auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
            if (!editor) continue;
            const QString path = editor->filePath();
            if (path != deletedPath && !path.startsWith(prefix)) continue;
            if (lsp_) lsp_->close(path);
            if (editor->isModified()) {
                editor->document()->setProperty("detachedDeletedFile", true);
                editor->detachDeletedFile();
                tabs_->setTabText(i, I18n::text("Untitled (deleted) •"));
                tabs_->setTabToolTip(i, I18n::text("Original file was deleted. Use Save As to keep this buffer."));
            } else {
                QWidget* page = tabs_->widget(i);
                removeBufferViews(editor);
                tabs_->removeTab(i);
                page->deleteLater();
            }
        }
        updateWindowTitle();
    });
    // Keep editor docks inside the right-hand workspace, beside the full-height sidebar.
    editorArea_ = new QMainWindow(splitter);
    debugPanel_->setTerminalAvailable(true);
    connect(debugPanel_,&DebugPanel::consoleRequested,this,[this]{ ensureTerminal(); terminalPanel_->showDebugConsole(); });
    connect(debugPanel_,&DebugPanel::terminalRequested,this,[this](const QJsonObject& request){ ensureTerminal(); terminalPanel_->runDebugProgram(request); });
    connect(debugPanel_,&DebugPanel::terminalStopRequested,this,[this]{ if(terminalPanel_) terminalPanel_->stopDebugProgram(); });
    editorArea_->setWindowFlags(Qt::Widget);
    editorArea_->setObjectName("editorArea");
    auto* documentArea = new QWidget(editorArea_);
    auto* documentLayout = new QVBoxLayout(documentArea);
    documentLayout->setContentsMargins(0, 0, 0, 0);
    documentLayout->setSpacing(0);
    tabs_ = new QTabWidget(documentArea);
    tabs_->setObjectName("editorTabs");
    tabs_->tabBar()->hide();
    tabs_->setDocumentMode(true);
    singleEditorHeader_ = new EditorPaneHeader(documentArea);
    singleEditorHeader_->hide();
    connectEditorHeader(singleEditorHeader_);
    paneWorkspace_ = new EditorPaneWorkspace(documentArea);
    connect(paneWorkspace_, &EditorPaneWorkspace::paneCreated, this, [this](EditorPane* pane) {
        connectEditorHeader(pane->header(), pane);
    });
    vimPageTabs_ = new QTabBar(documentArea);
    vimPageTabs_->setObjectName("vimPageTabs");
    vimPageTabs_->setAccessibleName(I18n::text("Vim tab pages"));
    vimPageTabs_->setExpanding(false);
    vimPageTabs_->setElideMode(Qt::ElideMiddle);
    vimPageTabs_->hide();
    connect(vimPageTabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (closing_ || !paneWorkspace_->selectPage(index)) return;
        const QScopedValueRollback changing(updatingWorkspace_, true);
        updateWorkspaceFocus(true);
    });
    paneWorkspace_->hide();
    documentLayout->addWidget(vimPageTabs_);
    documentLayout->addWidget(singleEditorHeader_);
    tabSwitcher_ = new QFrame(documentArea);
    tabSwitcher_->setObjectName("openFileSwitcher");
    tabSwitcher_->setFrameShape(QFrame::StyledPanel);
    auto* switcherLayout = new QVBoxLayout(tabSwitcher_);
    switcherLayout->setContentsMargins(8, 8, 8, 8);
    tabSwitchList_ = new QListWidget(tabSwitcher_);
    tabSwitchList_->setObjectName("openFileSwitchList");
    tabSwitchList_->setAccessibleName(I18n::text("Open files"));
    tabSwitchList_->setFrameShape(QFrame::NoFrame);
    tabSwitchList_->setIconSize(QSize(18, 18));
    tabSwitchList_->setSpacing(2);
    tabSwitchList_->setUniformItemSizes(true);
    tabSwitchList_->setTextElideMode(Qt::ElideMiddle);
    tabSwitchList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    switcherLayout->addWidget(tabSwitchList_);
    tabSwitcher_->hide();
    connect(tabSwitchList_, &QListWidget::itemClicked, this, [this] { finishTabSwitch(true); });
    documentLayout->addWidget(tabs_, 1);
    documentLayout->addWidget(paneWorkspace_, 1);
    editorArea_->setCentralWidget(documentArea);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) { closeTab(index); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        updateVimIndicator();
        scheduleOutline();
        if(gitDiff_ && !gitDiff_->isHidden() && gitDiff_->nativeEditor()!=activeEditor()) gitDiff_->hide();
        if (!tabSwitcher_->isHidden()) finishTabSwitch(false);
        if(lsp_ && lspStatus_) {
            auto* editor=activeEditor();
            lspStatus_->setText(lsp_->status(editor?editor->filePath():QString{}));
        }
        if (index >= 0) {
            updateTabMru(tabs_->widget(index));
            requestSemanticHighlighting(activeEditor());
        }
        if (!paneWorkspace_->count()) {
            auto* current = activeEditor();
            if (current && current != vimLastEditor_) {
                if (!filePreviews_.contains(vimLastEditor_)) vimAlternateEditor_ = vimLastEditor_;
                if (vimAlternateEditor_ == current) vimAlternateEditor_.clear();
                vimLastEditor_ = current;
            }
            if (current) current->setProperty("vimAlternatePath", vimAlternateEditor_ ? vimAlternateEditor_->filePath() : QString{});
        }
        updateEditorPaneActions();
        if (!paneWorkspace_->count()) {
            updateEditorHeaders();
            syncExplorerToActiveFile();
            updateVimAutocmdBuffer();
        }
        scheduleFilePreviewCleanup();
    });

    splitter->addWidget(sidebar_);
    splitter->addWidget(editorArea_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({254, 980});
    workspaceLayout->addWidget(activityBar);
    workspaceLayout->addWidget(splitter,1);
    setCentralWidget(workspace);
    setProjectRoots({QDir::homePath()}, false);
}

void MainWindow::updateSidebarActivities() {
    int index=0;
    for (auto* button : {explorerActivity_,searchActivity_,gitActivity_,debugActivity_}) {
        button->setChecked(!sidebar_->isHidden() && sidebarStack_->currentIndex()==index);
        ++index;
    }
}

void MainWindow::setSidebarVisible(bool visible) {
    sidebar_->setVisible(visible);
    updateSidebarActivities();
    syncExplorerToActiveFile();
    if (!visible && tabs_ && activeEditor()) activeEditor()->setFocus();
}

QString MainWindow::activeExplorerPath() const {
    const auto* editor = activeEditor();
    if (!editor) return {};
    if (!editor->filePath().isEmpty()) return editor->filePath();
    // A newly opened file can become active before its document is loaded.
    const auto* primary = documentEditor(editor);
    return primary ? primary->property("openingPath").toString() : QString{};
}

void MainWindow::syncExplorerToActiveFile() {
    if (closing_ || !autoRevealActiveFile_ || sidebar_->isHidden() ||
        sidebarStack_->currentWidget() != explorerPage_) {
        explorer_->cancelAutomaticReveal();
        return;
    }
    explorer_->revealPath(activeExplorerPath());
}

void MainWindow::createMenus() {
    auto* fileMenu = menuBar()->addMenu(I18n::text("&File"));

    auto* openAction = fileMenu->addAction(Codicons::icon("go-to-file"), I18n::text("&Open File..."));
    ShortcutManager::instance().bind("file.open", openAction);
    connect(openAction, &QAction::triggered, this, [this]() { openFile(); });

    auto* openFolderAction = fileMenu->addAction(Codicons::icon("folder-opened"), I18n::text("Open &Folder..."));
    ShortcutManager::instance().bind("file.openFolder", openFolderAction);
    connect(openFolderAction, &QAction::triggered, this, &MainWindow::openFolder);

    auto* saveAction = fileMenu->addAction(Codicons::icon("save"), I18n::text("&Save"));
    ShortcutManager::instance().bind("file.save", saveAction);
    connect(saveAction, &QAction::triggered, this, &MainWindow::saveCurrentFile);

    auto* saveAllAction = fileMenu->addAction(Codicons::icon("save-all"), I18n::text("Save All"));
    ShortcutManager::instance().bind("file.saveAll", saveAllAction);
    connect(saveAllAction, &QAction::triggered, this, &MainWindow::saveAllFiles);

    auto* saveAsAction = fileMenu->addAction(Codicons::icon("save-as"), I18n::text("Save &As..."));
    ShortcutManager::instance().bind("file.saveAs", saveAsAction);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveCurrentFileAs);

    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(Codicons::icon("sign-out"), I18n::text("&Quit"));
    ShortcutManager::instance().bind("file.quit", quitAction);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto* searchMenu = menuBar()->addMenu(I18n::text("&Search"));
    auto* searchAction = searchMenu->addAction(Codicons::icon("search"), I18n::text("Search in &Files..."));
    ShortcutManager::instance().bind("search.project", searchAction);
    connect(searchAction, &QAction::triggered, this, [this] { showProjectSearch(); });
    auto* findAction = searchMenu->addAction(Codicons::icon("search"), I18n::text("Find in Current File..."));
    ShortcutManager::instance().bind("search.find", findAction);
    connect(findAction, &QAction::triggered, this, [this] {
        auto* editor = activeEditor();
        if (editor) editor->showFindBar();
    });
    auto* nextFindAction = searchMenu->addAction(Codicons::icon("arrow-down"), I18n::text("Find Next"));
    ShortcutManager::instance().bind("search.next", nextFindAction);
    connect(nextFindAction, &QAction::triggered, this, [this] {
        if (auto* editor = activeEditor()) editor->findNext();
    });
    auto* previousFindAction = searchMenu->addAction(Codicons::icon("arrow-up"), I18n::text("Find Previous"));
    ShortcutManager::instance().bind("search.previous", previousFindAction);
    connect(previousFindAction, &QAction::triggered, this, [this] {
        if (auto* editor = activeEditor()) editor->findNext(true);
    });
    auto* replaceAction = searchMenu->addAction(Codicons::icon("replace"), I18n::text("Replace in Current File..."));
    ShortcutManager::instance().bind("search.replace", replaceAction);
    connect(replaceAction, &QAction::triggered, this, [this] {
        auto* editor = activeEditor();
        if (!editor) return;
        editor->showFindBar(true);
    });

    auto* codeMenu = menuBar()->addMenu(I18n::text("&Code"));
    for(const bool block:{false,true}) {
        auto* action=codeMenu->addAction(Codicons::icon("comment"),I18n::text(block?"Toggle block comment":"Toggle line comment"));
        const auto id=block?"editor.toggleBlockComment":"editor.toggleLineComment";
        action->setObjectName(id);ShortcutManager::instance().bind(id,action);action->setShortcutContext(Qt::WidgetShortcut);
        connect(action,&QAction::triggered,this,[this,block] {
            if(auto* editor=activeEditor()) {
                if(block)editor->toggleBlockComment();else editor->toggleLineComment();
            }
        });
    }
    codeMenu->addSeparator();
    for(const auto& command:std::vector<std::pair<QString,std::pair<int,bool>>>{
        {"editor.copyLineUp",{-1,true}},{"editor.copyLineDown",{1,true}},
        {"editor.moveLineUp",{-1,false}},{"editor.moveLineDown",{1,false}}}) {
        const auto operation=command.second;
        const auto label=operation.second?(operation.first<0?"Copy line up":"Copy line down"):(operation.first<0?"Move line up":"Move line down");
        auto* action=codeMenu->addAction(Codicons::icon(operation.first<0?"arrow-up":"arrow-down"),I18n::text(label));
        action->setObjectName(command.first);ShortcutManager::instance().bind(command.first,action);
        // Editor-owned shortcuts handle typing focus; this binding displays the key in the menu.
        action->setShortcutContext(Qt::WidgetShortcut);
        connect(action,&QAction::triggered,this,[this,operation] {
            if(auto* editor=activeEditor()) {
                if(operation.second)editor->copyLines(operation.first);else editor->moveLines(operation.first);
            }
        });
    }
    codeMenu->addSeparator();
    auto* formatAction = codeMenu->addAction(Codicons::icon("symbol-method"), I18n::text("Format document"));
    formatAction->setObjectName("code.formatDocument");
    ShortcutManager::instance().bind("code.formatDocument", formatAction);
    connect(formatAction, &QAction::triggered, this, &MainWindow::formatDocument);
    auto* completeAction = codeMenu->addAction(Codicons::icon("symbol-method"), I18n::text("Complete"));
    ShortcutManager::instance().bind("code.complete", completeAction);
    connect(completeAction, &QAction::triggered, this, [this] {
        requestCompletion(activeEditor(), false);
    });
    auto* hoverAction = codeMenu->addAction(Codicons::icon("info"), I18n::text("Symbol information"));
    ShortcutManager::instance().bind("code.hover", hoverAction);
    connect(hoverAction, &QAction::triggered, this, [this] { showCodeHelp(false); });
    auto* signatureAction = codeMenu->addAction(Codicons::icon("symbol-method"), I18n::text("Function parameters"));
    ShortcutManager::instance().bind("code.signature", signatureAction);
    connect(signatureAction, &QAction::triggered, this, [this] { showCodeHelp(true); });
    auto* renameAction = codeMenu->addAction(Codicons::icon("edit"), I18n::text("Rename symbol..."));
    renameAction->setObjectName("code.rename");
    ShortcutManager::instance().bind("code.rename", renameAction);
    connect(renameAction, &QAction::triggered, this, &MainWindow::renameSymbol);
    auto* problemsAction = codeMenu->addAction(Codicons::icon("warning"), I18n::text("Problems..."));
    ShortcutManager::instance().bind("code.problems", problemsAction);
    connect(problemsAction, &QAction::triggered, this, &MainWindow::showLspProblems);
    for (const auto& command : std::vector<std::pair<QString, QString>>{
        {"definition", "Go to Definition"}, {"declaration", "Go to Declaration"},
        {"typeDefinition", "Go to Type Definition"}, {"references", "Find All References"}}) {
        auto* action = codeMenu->addAction(Codicons::icon("go-to-file"), I18n::text(command.second));
        action->setObjectName("code." + command.first);
        ShortcutManager::instance().bind(action->objectName(), action);
        connect(action, &QAction::triggered, this, [this, method=command.first] { navigateSymbol(method); });
    }

    auto* goMenu = menuBar()->addMenu(I18n::text("&Go"));
    auto* commandsAction = goMenu->addAction(Codicons::icon("terminal"), I18n::text("Command palette"));
    commandsAction->setObjectName("navigation.commands");
    ShortcutManager::instance().bind("navigation.commands", commandsAction);
    connect(commandsAction, &QAction::triggered, this, &MainWindow::commandPalette);
    auto* quickOpenAction = goMenu->addAction(Codicons::icon("go-to-file"), I18n::text("Quick Open..."));
    ShortcutManager::instance().bind("navigation.quickOpen", quickOpenAction);
    connect(quickOpenAction, &QAction::triggered, this, &MainWindow::quickOpen);
    for (const bool project : {false, true}) {
        const auto id = project ? "navigation.projectSymbols" : "navigation.documentSymbols";
        auto* action = goMenu->addAction(Codicons::icon("symbol-method"),
            I18n::text(project ? "Go to Symbol in Project" : "Go to Symbol in File"));
        action->setObjectName(id);
        ShortcutManager::instance().bind(id, action);
        connect(action, &QAction::triggered, this, [this, project] { searchSymbols(project); });
    }
    auto* goToLineAction = goMenu->addAction(Codicons::icon("list-ordered"), I18n::text("Go to Line..."));
    ShortcutManager::instance().bind("navigation.line", goToLineAction);
    connect(goToLineAction, &QAction::triggered, this, &MainWindow::goToLine);
    auto* previousTabShortcut = new QShortcut(this);
    ShortcutManager::instance().bind("navigation.tabNext", previousTabShortcut);
    previousTabShortcut->setContext(Qt::WindowShortcut);
    connect(previousTabShortcut, &QShortcut::activated, this, [this] { switchRecentTab(1); });
    auto* nextTabShortcut = new QShortcut(this);
    ShortcutManager::instance().bind("navigation.tabPrevious", nextTabShortcut);
    nextTabShortcut->setContext(Qt::WindowShortcut);
    connect(nextTabShortcut, &QShortcut::activated, this, [this] { switchRecentTab(-1); });
    goMenu->addSeparator();
    auto* backAction = goMenu->addAction(Codicons::icon("arrow-left"), I18n::text("Back"));
    ShortcutManager::instance().bind("navigation.back", backAction);
    connect(backAction, &QAction::triggered, this, [this] { navigateHistory(-1); });
    auto* forwardAction = goMenu->addAction(Codicons::icon("arrow-right"), I18n::text("Forward"));
    ShortcutManager::instance().bind("navigation.forward", forwardAction);
    connect(forwardAction, &QAction::triggered, this, [this] { navigateHistory(1); });

    auto* settingsMenu = menuBar()->addMenu(I18n::text("&Settings"));
    auto* openSettings = settingsMenu->addAction(Codicons::icon("settings-gear"), I18n::text("Settings…"));
    openSettings->setObjectName("settings.open");
    ShortcutManager::instance().bind("settings.open", openSettings);
    connect(openSettings, &QAction::triggered, this, [this] { showSettings(); });

    auto addWindowAction = [this](const QString& id, const QString& label, const QString& icon) {
        auto* action = new QAction(Codicons::icon(icon), label, this);
        action->setObjectName(id);
        addAction(action);
        ShortcutManager::instance().bind(id, action);
        return action;
    };
    auto addSettingsAction = [this, &addWindowAction](const QString& id, const char* label, const QString& icon, SettingsPage page) {
        auto* action = addWindowAction(id, I18n::text(label), icon);
        connect(action, &QAction::triggered, this, [this, page] { showSettings(page); });
    };
    addSettingsAction("settings.editor", "Editor settings", "settings-gear", SettingsPage::Editor);
    addSettingsAction("settings.interfaceLanguage", "Interface language", "globe", SettingsPage::Appearance);
    addSettingsAction("settings.theme", "Theme", "color-mode", SettingsPage::Theme);
    addSettingsAction("settings.shortcuts", "Keyboard shortcuts", "keyboard", SettingsPage::Shortcuts);
    addSettingsAction("settings.lsp", "Language servers", "server", SettingsPage::LanguageServers);
    addSettingsAction("settings.languages", "Syntax highlighting", "code", SettingsPage::Highlighting);

    auto* vimAction = addWindowAction("settings.vimMode", I18n::text("Vim mode"), "keyboard");
    vimAction->setCheckable(true);
    vimAction->setChecked(QSettings().value("editor/vimMode", false).toBool());
    connect(vimAction, &QAction::triggered, this, [this, vimAction](bool enabled) {
        QSettings settings;
        const bool previous = settings.value("editor/vimMode", false).toBool();
        settings.setValue("editor/vimMode", enabled);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            settings.setValue("editor/vimMode", previous);
            const QSignalBlocker blocker(vimAction);
            vimAction->setChecked(previous);
            statusBar()->showMessage(I18n::text("Cannot save settings."), 6000);
            return;
        }
        applyVimSettings();
    });

    auto* focusNavigation = addWindowAction(ShortcutManager::navigationPrefixId,
                                            I18n::text("Focus navigation prefix"), "keyboard");
    focusNavigation->setAutoRepeat(false);
    connect(focusNavigation, &QAction::triggered, this, &MainWindow::beginFocusNavigation);
    auto* focusEditorAction = addWindowAction("view.focusEditor", I18n::text("Focus Editor"), "file-code");
    connect(focusEditorAction, &QAction::triggered, this, &MainWindow::focusEditor);
    auto* closePaneAction = addWindowAction("editor.closePane", I18n::text("Close Editor Pane"), "close");
    connect(closePaneAction, &QAction::triggered, this, [this] { closeEditorPane(activeEditor()); });
    auto* focusTerminalAction = addWindowAction("view.focusTerminal", I18n::text("Toggle Terminal Focus"), "terminal");
    connect(focusTerminalAction, &QAction::triggered, this, [this] {
        auto* focused = QApplication::focusWidget();
        if (terminalPanel_ && focused &&
            (focused == terminalPanel_ || terminalPanel_->isAncestorOf(focused))) {
            focusEditor();
        } else {
            focusTerminal();
        }
    });
    auto* focusTreeAction = addWindowAction("view.focusProjectTree", I18n::text("Toggle Project Tree Focus"), "files");
    connect(focusTreeAction, &QAction::triggered, this, [this] {
        if (explorer_->hasFocus()) focusEditor();
        else focusProjectTree();
    });

    auto* viewMenu = menuBar()->addMenu(I18n::text("&View"));
    auto addViewAction = [this, viewMenu](const QString& id, const QString& label, const QString& icon, auto action) {
        auto* item = viewMenu->addAction(Codicons::icon(icon), I18n::text(label));
        item->setObjectName(id);
        ShortcutManager::instance().bind(id, item);
        connect(item, &QAction::triggered, this, action);
        return item;
    };
    addViewAction("editor.splitBelow", "Split Editor Below", "split-horizontal", [this] { splitEditor(Qt::Vertical); });
    addViewAction("editor.splitRight", "Split Editor Right", "split-vertical", [this] { splitEditor(Qt::Horizontal); });
    updateEditorPaneActions();
    outline_=new OutlinePanel(editorArea_);
    editorArea_->addDockWidget(Qt::RightDockWidgetArea,outline_);
    editorArea_->resizeDocks({outline_},{280},Qt::Horizontal);
    outlineTimer_=new QTimer(this); outlineTimer_->setSingleShot(true); outlineTimer_->setInterval(350);
    connect(outlineTimer_,&QTimer::timeout,this,&MainWindow::refreshOutline);
    outline_->navigate=[this](int line,int column) {
        auto* editor=activeEditor();
        if(editor && editor==outlineEditor_ && !editor->isBusy()) {
            editor->revealMatch(line,column,0,true); editor->setFocus();
        }
    };
    auto* outlineAction=outline_->toggleViewAction();
    outlineAction->setObjectName("view.outline"); outlineAction->setIcon(Codicons::icon("list-tree"));
    viewMenu->addAction(outlineAction);
    ShortcutManager::instance().bind("view.outline", outlineAction);
    const bool showOutline=QSettings().value("view/outline",true).toBool();
    outline_->setVisible(showOutline);
    QTimer::singleShot(0,this,[this] {
        if(outline_->isVisible()) editorArea_->resizeDocks({outline_},{280},Qt::Horizontal);
    });
    connect(outlineAction,&QAction::triggered,this,[](bool visible) { QSettings().setValue("view/outline",visible); });
    outline_->onClosed=[] { QSettings().setValue("view/outline",false); };
    connect(outline_,&QDockWidget::visibilityChanged,this,[this](bool) { scheduleOutline(); });
    auto* miniMapAction=viewMenu->addAction(Codicons::icon("map"),I18n::text("Minimap"));
    miniMapAction->setObjectName("view.minimap");
    ShortcutManager::instance().bind("view.minimap", miniMapAction);
    miniMapAction->setCheckable(true);
    miniMapAction->setChecked(QSettings().value("editor/minimap",true).toBool());
    connect(miniMapAction,&QAction::toggled,this,[this](bool enabled) {
        QSettings().setValue("editor/minimap",enabled);
        // Also update documents temporarily hosted by the Git diff dock.
        for (auto* window:QApplication::topLevelWidgets())
            if (auto* main=qobject_cast<MainWindow*>(window)) {
                for (auto* editor:main->findChildren<EditorWidget*>()) editor->setMiniMapEnabled(enabled);
                if (auto* action=main->findChild<QAction*>("view.minimap")) {
                    const QSignalBlocker blocker(action); action->setChecked(enabled);
                }
            }
    });
    addViewAction("view.terminal", "Terminal", "terminal", [this] {
        if (terminalPanel_ && terminalPanel_->isVisible()) terminalPanel_->hide();
        else showTerminal();
    });
    auto addFileFilterAction = [this, &addWindowAction](const QString& id, const char* label,
                                                        const QString& icon, const QString& key) {
        auto* action = addWindowAction(id, I18n::text(label), icon);
        action->setCheckable(true);
        action->setChecked(QSettings().value(key, false).toBool());
        connect(action, &QAction::triggered, this, [this, key](bool enabled) {
            QSettings().setValue(key, enabled);
            applyFileVisibilitySettings();
            if (key == "view/showHidden")
                statusBar()->showMessage(I18n::text(enabled ? "Hidden files are visible" : "Hidden files are hidden"), 2000);
        });
    };
    addFileFilterAction("view.hidden", "Show Hidden Files", "eye", "view/showHidden");
    addFileFilterAction("view.ignored", "Include ignored files", "filter", "project/includeIgnored");
    explorer_->setShowHidden(QSettings().value("view/showHidden", false).toBool());
    explorer_->setIncludeIgnored(QSettings().value("project/includeIgnored", false).toBool());
    searchPanel_->setShowHidden(QSettings().value("view/showHidden", false).toBool());
    searchPanel_->setIncludeIgnored(QSettings().value("project/includeIgnored", false).toBool());
    auto* explorerAction = viewMenu->addAction(Codicons::icon("files"), I18n::text("Toggle Side Bar"));
    explorerAction->setObjectName("view.explorer");
    ShortcutManager::instance().bind("view.explorer", explorerAction);
    connect(explorerAction, &QAction::triggered, this, [this] {
        setSidebarVisible(sidebar_->isHidden());
    });
    auto* revealAction = viewMenu->addAction(Codicons::icon("target"), I18n::text("Reveal Active File in Project Tree"));
    revealAction->setObjectName("view.revealActiveFile");
    ShortcutManager::instance().bind("view.revealActiveFile", revealAction);
    connect(revealAction, &QAction::triggered, this, [this] {
        const auto path = activeExplorerPath();
        if (path.isEmpty()) {
            statusBar()->showMessage(I18n::text("No active file to show in the project tree."), 5000);
            return;
        }
        sidebarStack_->setCurrentWidget(explorerPage_);
        setSidebarVisible(true);
        explorer_->revealPath(path, true);
    });
    auto* debugAction = viewMenu->addAction(Codicons::icon("debug-alt"), I18n::text("Run and Debug"));
    ShortcutManager::instance().bind("view.debug",debugAction);
    connect(debugAction,&QAction::triggered,this,[this]{setSidebarVisible(true);sidebarStack_->setCurrentWidget(debugPanel_);});
    for(const auto& command:QStringList{"start","stop","next","stepIn","stepOut"}) {
        auto* action=new QAction(this); addAction(action);
        ShortcutManager::instance().bind("debug."+command,action);
        connect(action,&QAction::triggered,this,[this,command]{
            if(command=="start") { setSidebarVisible(true); sidebarStack_->setCurrentWidget(debugPanel_); }
            debugPanel_->perform(command);
        });
    }
    auto* breakpointAction=new QAction(this); addAction(breakpointAction);
    ShortcutManager::instance().bind("debug.breakpoint",breakpointAction);
    connect(breakpointAction,&QAction::triggered,debugPanel_,&DebugPanel::breakpointRequested);
    auto* gitAction = viewMenu->addAction(Codicons::icon("source-control"), I18n::text("Git"));
    ShortcutManager::instance().bind("view.git", gitAction);
    connect(gitAction, &QAction::triggered, this, [this] {
        setSidebarVisible(true); sidebarStack_->setCurrentWidget(gitPanel_);
        explorerActivity_->setChecked(false); searchActivity_->setChecked(false); gitActivity_->setChecked(true);
        gitPanel_->refresh();
    });
}

void MainWindow::openFolder() {
    const QString path = IconDialogs::FileDialog::getExistingDirectory(this, I18n::text("Open Folder"), projectRoots_.isEmpty() ? QDir::homePath() : projectRoots_.front());
    if (!path.isEmpty()) setProjectRoots({path});
}

void MainWindow::setProjectRoots(const QStringList& roots, bool explicitProject) {
    if (!branchRepository_.isEmpty()) {
        statusBar()->showMessage(I18n::text("Wait for the branch switch to finish."), 5000); return;
    }
    QStringList valid;
    for (const QString& path : roots) {
        const QFileInfo info(path);
        if (info.exists() && info.isDir()) {
            const QString normalized = QDir::cleanPath(info.absoluteFilePath());
            valid.push_back(normalized);
            break; // Exactly one project, including legacy sessions and CLI/IPC input.
        }
    }
    projectRoots_ = valid;
    workspaceIndex_->configure(explicitProject?valid:QStringList{}, {QSettings().value("view/showHidden",false).toBool(), QSettings().value("project/includeIgnored",false).toBool(), {}});
    debugPanel_->setWorkspace(valid.value(0));
    explicitProject_ = explicitProject && !valid.isEmpty();
    if (lsp_) lsp_->setRoot(projectRoots_.isEmpty() ? QDir::homePath() : projectRoots_.front());
    if (terminalPanel_) terminalPanel_->setDirectory(projectRoots_.value(0));
    explorer_->setRoots(projectRoots_);
    syncExplorerToActiveFile();
    searchPanel_->setRoots(projectRoots_);
    gitPanel_->setRoots(projectRoots_);
    if (gitDiff_) gitDiff_->hide();
    updateWindowTitle();
}

void MainWindow::openPaths(const QStringList& paths) {
    if(closing_)return;
    QStringList roots;
    QStringList files;
    for (const QString& input : paths) {
        const QString path = QDir::cleanPath(QFileInfo(input).absoluteFilePath());
        const QFileInfo info(path);
        if (info.isDir()) roots.push_back(path);
        else if (info.isFile()) files.push_back(path);
    }
    if (!roots.isEmpty()) setProjectRoots(roots);
    for (const QString& file : files) openFile(file);
    if (paths.isEmpty()) {
        raise();
        activateWindow();
    } else if (roots.isEmpty() && files.isEmpty()) {
        statusBar()->showMessage(I18n::text("None of the supplied paths exists."), 5000);
    }
}

void MainWindow::rememberRecentFile(const QString& path) {
    if (path.isEmpty() || !QFileInfo::exists(path)) return;
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("editor/recentFiles")).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 30) recent.removeLast();
    settings.setValue(QStringLiteral("editor/recentFiles"), recent);
}

void MainWindow::commandPalette() {
    QPointer<QWidget> previousFocus = QApplication::focusWidget();
    QPointer<EditorWidget> editor = activeEditor();
    QVector<PaletteCommand> entries;
    auto& manager = ShortcutManager::instance();
    for (const auto& command : manager.commands()) {
        if (command.id == "navigation.commands") continue;
        QPointer<QObject> target = manager.target(command.id, this, editor);
        if (!target && !command.id.startsWith("editor.")) continue;
        PaletteCommand entry;
        entry.id = command.id;
        entry.title = command.category + ": " + command.name;
        entry.searchText = command.englishName + " " + command.id;
        entry.shortcut = manager.sequence(command.id).toString(QKeySequence::NativeText);
        entry.icon = Codicons::icon("chevron-right");
        if (auto* action = qobject_cast<QAction*>(target)) {
            if (!action->icon().isNull()) entry.icon = action->icon();
            entry.enabled = action->isEnabled();
            entry.checked = action->isCheckable() ? (action->isChecked() ? 1 : 0) : -1;
        } else if (auto* shortcut = qobject_cast<QShortcut*>(target)) entry.enabled = shortcut->isEnabled();
        const bool needsEditor = command.id.startsWith("editor.") || command.id.startsWith("code.") ||
            command.id == "file.save" || command.id == "file.saveAs" || command.id == "navigation.line" ||
            command.id == "debug.breakpoint" || (command.id.startsWith("search.") && command.id != "search.project");
        if (needsEditor && (!editor || editor->isBusy())) {
            entry.enabled = false;
            entry.reason = I18n::text(editor ? "Document is busy" : "No open document");
        }
        if (command.id == "code.problems") entry.enabled = true;
        else if (command.id.startsWith("code.") && editor && !editor->isBusy() && !lsp_->supports(editor->filePath())) {
            entry.enabled = false; entry.reason = I18n::text("Language server not configured");
        }
        if (command.id.startsWith("debug.") && !debugPanel_->commandAvailable(command.id.mid(6))) {
            entry.enabled = false; entry.reason = I18n::text("Debug command is unavailable in the current state");
        }
        if (!entry.enabled && entry.reason.isEmpty()) entry.reason = I18n::text("Command is unavailable");
        entry.execute = [this, target, editor, needsEditor, id=command.id] {
            if (needsEditor && id != "code.problems" && (!editor || editor->isBusy() || activeEditor() != editor)) return;
            if (id.startsWith("debug.") && !debugPanel_->commandAvailable(id.mid(6))) return;
            if (needsEditor && editor) editor->setFocus(Qt::OtherFocusReason);
            if (auto* action = qobject_cast<QAction*>(target)) { if (action->isEnabled()) action->trigger(); }
            else if (auto* shortcut = qobject_cast<QShortcut*>(target); shortcut && shortcut->isEnabled())
                QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection);
        };
        entries.push_back(std::move(entry));
    }
    CommandPalette palette(std::move(entries), this);
    const int result = palette.exec();
    activateWindow();
    if (previousFocus) previousFocus->setFocus(Qt::OtherFocusReason);
    else if (editor) editor->setFocus(Qt::OtherFocusReason);
    if (result == QDialog::Accepted) palette.executeSelected();
}

void MainWindow::quickOpen() {
    workspaceIndex_->invalidate();
    QDialog dialog(this); dialog.setWindowTitle(I18n::text("Quick Open"));dialog.resize(680,420);
    auto* layout=new QVBoxLayout(&dialog);
    auto* input=new QLineEdit(&dialog);input->setPlaceholderText(I18n::text("Search files by name or path..."));
    auto* results=new QListWidget(&dialog);results->setUniformItemSizes(true);
    auto* status=new QLabel(&dialog);
    layout->addWidget(input);layout->addWidget(results,1);layout->addWidget(status);
    QTimer debounce;debounce.setSingleShot(true);debounce.setInterval(60);
    const auto refresh=[&] {
        QStringList open;
        for(int i=0;i<tabs_->count();++i)if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i)))if(!editor->filePath().isEmpty())open<<editor->filePath();
        status->setText(I18n::text("Searching…"));
        workspaceIndex_->query(input->text().trimmed(),QSettings().value("editor/recentFiles").toStringList(),open,&dialog,
            [results,status,input](FileQueryResult value) {
                results->setProperty("indexing",value.indexing);
                input->setPlaceholderText(I18n::text("Search files by name or path (%1 files)...").arg(value.indexedFiles));
                results->clear();
                for(const auto& file:value.files){auto* item=new QListWidgetItem(FileIcons::icon(file.path),file.relative,results);item->setToolTip(file.path);item->setData(Qt::UserRole,file.path);}
                if(results->count())results->setCurrentRow(0);
                status->setText(!value.error.isEmpty()?value.error:value.indexing?I18n::text("Indexing project…"):I18n::text("Enter to open · Escape to close"));
            });
    };
    connect(&debounce,&QTimer::timeout,&dialog,refresh);
    connect(input,&QLineEdit::textChanged,&dialog,[&]{workspaceIndex_->cancelQuery();results->clear();debounce.start();});
    connect(workspaceIndex_,&WorkspaceIndex::updated,&dialog,[&]{debounce.start();});
    connect(input,&QLineEdit::returnPressed,&dialog,[&]{if(results->currentItem())dialog.accept();});
    connect(results,&QListWidget::itemActivated,&dialog,[&](QListWidgetItem*){dialog.accept();});
    for(const auto key:{Qt::Key_Down,Qt::Key_Up}) {
        auto* shortcut=new QShortcut(QKeySequence(key),&dialog);shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut,&QShortcut::activated,&dialog,[results,key]{if(results->count())results->setCurrentRow(std::clamp(results->currentRow()+(key==Qt::Key_Down?1:-1),0,results->count()-1));});
    }
    input->setFocus();refresh();
    const auto accepted=dialog.exec()==QDialog::Accepted;
    workspaceIndex_->cancelQuery();
    if(accepted && results->currentItem())openFile(results->currentItem()->data(Qt::UserRole).toString());
}

void MainWindow::updateTabMru(QWidget* current) {
    tabMru_.erase(std::remove_if(tabMru_.begin(), tabMru_.end(), [this, current](const QPointer<QWidget>& tab) {
        return tab.isNull() || tab == current || tabs_->indexOf(tab.data()) < 0;
    }), tabMru_.end());
    if (current && !filePreviews_.contains(qobject_cast<EditorWidget*>(current))) tabMru_.prepend(current);
}

void MainWindow::pinFilePreview(EditorWidget* editor) {
    if (!filePreviews_.remove(editor)) return;
    if (!editor->isBusy()) rememberRecentFile(editor->filePath());
    if (documentEditor(activeEditor()) == editor) updateTabMru(editor);
    else tabMru_.append(editor);
    updateEditorHeaders();
}

void MainWindow::scheduleFilePreviewCleanup() {
    if (filePreviews_.isEmpty() || filePreviewCleanupScheduled_ || closing_) return;
    filePreviewCleanupScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        filePreviewCleanupScheduled_ = false;
        closeInactiveFilePreviews();
    });
}

void MainWindow::closeInactiveFilePreviews() {
    if (closing_ || recoveryWriting_ || renameInProgress_ || !replaceLockedEditors_.isEmpty()) return;
    auto inactive = filePreviews_;
    inactive.remove(documentEditor(activeEditor()));
    // A preview can stay on screen in another split or on another Vim tab page.
    for (auto* pane : paneWorkspace_->panes(true)) inactive.remove(documentEditor(pane->editor()));
    for (auto* editor : inactive) {
        if (tabs_->indexOf(editor) < 0) continue;
        if (editor->isBusy() && !editor->property("openingPath").isValid()) continue;
        if (editor->isModified()) {
            if (!editor->isBusy()) {
                requestClose({editor}, false, false, true);
                return;
            }
        } else {
            removeClosedEditor(editor);
        }
    }
}

bool MainWindow::closeTab(int index) {
    auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(index));
    if(!editor)return false;
    requestClose({QPointer<EditorWidget>(editor)},false);
    return tabs_->indexOf(editor)<0;
}

void MainWindow::closeAllTabs() {
    QVector<QPointer<EditorWidget>> editors;
    for(int i=0;i<tabs_->count();++i)
        if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i)))editors.append(editor);
    requestClose(editors,false);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (gitPanel_ && gitPanel_->service()->writing()) {
        statusBar()->showMessage(I18n::text("Wait for the Git operation to finish before closing."), 5000);
        event->ignore(); return;
    }
    if(allowWindowClose_) { if (terminalPanel_) terminalPanel_->shutdown(); event->accept();return; }
    event->ignore();
    QVector<QPointer<EditorWidget>> editors;
    for(int i=0;i<tabs_->count();++i)
        if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i)))editors.append(editor);
    requestClose(editors,true);
}

void MainWindow::requestClose(QVector<QPointer<EditorWidget>> editors,bool window,bool discard,bool inactivePreview) {
    if(closing_)return;
    if(renameInProgress_ || !replaceLockedEditors_.isEmpty()) {
        statusBar()->showMessage(I18n::text("Wait for the current operation to finish before closing."),6000);return;
    }
    for(const auto& editor:editors)if(editor && editor->isBusy()) {
        statusBar()->showMessage(I18n::text("Wait for the current operation to finish before closing."),6000);return;
    }
    closing_=true;closingWindow_=window;closingIndex_=0;
    closingInactivePreview_=inactivePreview;
    menuBar()->setEnabled(false);sidebar_->setEnabled(false);paneWorkspace_->setEnabled(false);
    vimPageTabs_->setEnabled(false);
    findChild<QWidget*>("activityBar")->setEnabled(false);
    closingEditors_=std::move(editors);discardedRevisions_.clear();
    if(discard)for(const auto& editor:closingEditors_)if(editor)discardedRevisions_[editor]=editor->lspRevision();
    for(const auto& editor:closingEditors_)if(editor)editor->setEnabled(false);
    advanceClose();
}

void MainWindow::advanceClose() {
    while(closingIndex_<closingEditors_.size()) {
        auto editor=closingEditors_[closingIndex_++];
        if(!editor || !editor->isModified())continue;
        if(discardedRevisions_.contains(editor) && discardedRevisions_.value(editor)==editor->lspRevision())continue;
        if (!closingInactivePreview_) activateEditor(editor);
        const auto answer=IconDialogs::MessageBox::warning(this,I18n::text("Unsaved changes"),
            I18n::text("Save changes to “%1” before closing?").arg(editor->filePath().isEmpty()?I18n::text("Untitled"):QFileInfo(editor->filePath()).fileName()),
            QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Save);
        if(!editor || answer==QMessageBox::Cancel){finishClose(false);return;}
        if(answer==QMessageBox::Discard){discardedRevisions_[editor]=editor->lspRevision();continue;}
        saveEditor(editor,[this](bool ok){if(ok)advanceClose();else finishClose(false);});return;
    }
    for(const auto& editor:closingEditors_)if(editor && (editor->isBusy() ||
       (editor->isModified() && (!discardedRevisions_.contains(editor) || discardedRevisions_.value(editor)!=editor->lspRevision())))) {
        finishClose(false);return;
    }
    finishClose(true);
}

void MainWindow::finishClose(bool accepted) {
    if(accepted && recoveryWriting_) {
        statusBar()->showMessage(I18n::text("Finishing recovery checkpoint…"));
        QTimer::singleShot(50,this,[this]{finishClose(true);});return;
    }
    menuBar()->setEnabled(true);sidebar_->setEnabled(true);paneWorkspace_->setEnabled(true);
    vimPageTabs_->setEnabled(true);
    findChild<QWidget*>("activityBar")->setEnabled(true);
    for(const auto& editor:closingEditors_)if(editor)editor->setEnabled(true);
    const bool inactivePreview = std::exchange(closingInactivePreview_, false);
    if (!accepted) {
        const auto preview = inactivePreview ? closingEditors_.value(0) : QPointer<EditorWidget>{};
        closing_=false;closingEditors_.clear();closingVimBuffer_.clear();
        if (preview) {
            auto* view = preview.data();
            for (auto* pane : paneWorkspace_->panes(true)) {
                if (auto* existing = pane->viewFor(preview->document().get())) {
                    view = existing;
                    break;
                }
            }
            activateEditor(view, true);
        }
        updateEditorPaneActions();updateEditorHeaders();scheduleFilePreviewCleanup();return;
    }
    finishTabSwitch(false);
    if(closingWindow_) {
        // Modified documents explicitly discarded by the user are reopened from disk.
        // Unsaved untitled documents are absent from the clean session.
        if(recovery_) {
            const auto error=recovery_->writeSession(sessionState(true));
            if(!error.isEmpty()) {
                IconDialogs::MessageBox::warning(this,I18n::text("Recovery failed"),error);
                closing_=false;return;
            }
        }
        for(const auto& editor:closingEditors_)if(editor) {
            const auto source=editor->property("recoveryOrigin").toString();
            if(!source.isEmpty()) {
                const auto error=RecoveryStore::retireDocument(source,editor->property("recoveryOriginId").toString());
                if(!error.isEmpty()){IconDialogs::MessageBox::warning(this,I18n::text("Recovery failed"),error);closing_=false;return;}
            }
            if(recovery_)recovery_->forgetDocument(editor->property("recoveryId").toString());
        }
        recoveryStopped_=true;allowWindowClose_=true;closing_=false;QTimer::singleShot(0,this,[this]{close();});return;
    }
    for(const auto& editor:closingEditors_)if(editor) {
        const auto source=editor->property("recoveryOrigin").toString();
        if(!source.isEmpty()) {
            const auto error=RecoveryStore::retireDocument(source,editor->property("recoveryOriginId").toString());
            if(!error.isEmpty()){IconDialogs::MessageBox::warning(this,I18n::text("Recovery failed"),error);closing_=false;return;}
        }
        if (closingVimBuffer_ == editor) {
            if (wipeVimBuffer_) editor->discardVimBufferMarks();
            else unloadedVimBuffers_.append({editor->property("vimBufferId").toULongLong(), editor->filePath()});
            closingVimBuffer_.clear();
        }
        removeClosedEditor(editor);
    }
    closingEditors_.clear();closing_=false;
    if (!inactivePreview) if(auto* current=activeEditor())current->setFocus();
    updateEditorPaneActions();
    updateWindowTitle();checkpointRecovery();scheduleFilePreviewCleanup();
}

void MainWindow::removeClosedEditor(EditorWidget* editor) {
    if(recovery_)recovery_->forgetDocument(editor->property("recoveryId").toString());
    recoveredRevisions_.remove(editor);
    externalFileStates_.remove(editor);
    externallyReloading_.remove(editor);
    removeBufferViews(editor);
    tabs_->removeTab(tabs_->indexOf(editor));
    filePreviews_.remove(editor);
    editor->deleteLater();
}

void MainWindow::positionTabSwitcher() {
    const QWidget* surface = paneWorkspace_->isVisible() ? static_cast<QWidget*>(paneWorkspace_) :
        (gitDiff_ && gitDiff_->isVisible() ? static_cast<QWidget*>(gitDiff_.data()) : tabs_);
    const QRect area(surface->mapTo(tabSwitcher_->parentWidget(), QPoint{}), surface->size());
    const auto margins = tabSwitcher_->layout()->contentsMargins();
    const auto metrics = tabSwitchList_->fontMetrics();
    const int rowHeight = std::max(26, metrics.height() + 10);
    int textWidth = 0;
    for (int row = 0; row < tabSwitchList_->count(); ++row) {
        auto* item = tabSwitchList_->item(row);
        textWidth = std::max(textWidth, metrics.horizontalAdvance(item->text()));
        item->setSizeHint(QSize(0, rowHeight));
    }
    const int rowSpacing = 2 * tabSwitchList_->spacing();
    const int verticalChrome = margins.top() + margins.bottom() + 2 * tabSwitcher_->frameWidth();
    const int visibleRows = std::min(tabSwitchList_->count(),
        std::clamp((area.height() / 2 - verticalChrome) / (rowHeight + rowSpacing), 1, 6));
    const int preferredWidth = textWidth + tabSwitchList_->iconSize().width() + 58;
    const int panelWidth = std::min(std::clamp(preferredWidth, 280, 500), std::max(1, area.width() - 16));
    const int panelHeight = std::min(std::max(1, area.height()), verticalChrome + visibleRows * (rowHeight + rowSpacing));
    const int topOffset = std::clamp(area.height() - panelHeight, 0, 12);
    tabSwitcher_->setGeometry(area.x() + (area.width() - panelWidth) / 2, area.y() + topOffset,
                              panelWidth, panelHeight);
}

void MainWindow::switchRecentTab(int direction) {
    const bool opening = tabSwitcher_->isHidden();
    if (opening) {
        auto* currentEditor = documentEditor(activeEditor());
        if (!currentEditor) return;
        updateTabMru(currentEditor);
        tabSwitchOrder_ = tabMru_;
        if (tabSwitchOrder_.isEmpty()) return;
        tabSwitchList_->clear();
        for (const auto& page : tabSwitchOrder_) {
            const int index = tabs_->indexOf(page);
            new QListWidgetItem(FileIcons::icon(tabs_->tabToolTip(index)), tabs_->tabText(index), tabSwitchList_);
        }
        tabSwitchFocus_ = QApplication::focusWidget();
        tabSwitcher_->ensurePolished();
        positionTabSwitcher();
        tabSwitcher_->show();
        tabSwitcher_->raise();
        qApp->installEventFilter(this);
        tabSwitchList_->setFocus();
    }
    const bool fromPreview = opening && filePreviews_.contains(documentEditor(activeEditor()));
    const int current = fromPreview && direction > 0 ? -1 : std::max(0, tabSwitchList_->currentRow());
    const auto count = tabSwitchOrder_.size();
    tabSwitchList_->setCurrentRow((current + direction + count) % count);
    tabSwitchList_->scrollToItem(tabSwitchList_->currentItem());
}

void MainWindow::finishTabSwitch(bool accept) {
    if (tabSwitcher_->isHidden()) return;
    const int row=tabSwitchList_->currentRow();
    QPointer<QWidget> target=accept && row>=0 && row<tabSwitchOrder_.size()?tabSwitchOrder_.at(row):QPointer<QWidget>{};
    qApp->removeEventFilter(this);
    tabSwitcher_->hide();
    tabSwitchOrder_.clear();
    if(target && tabs_->indexOf(target)>=0) {
        activateEditor(qobject_cast<EditorWidget*>(target.data()), true);
    } else if(tabSwitchFocus_ && isActiveWindow()) tabSwitchFocus_->setFocus();
    tabSwitchFocus_.clear();
}

void MainWindow::beginFocusNavigation() {
    if (closing_) return;
    finishTabSwitch(false);
    auto* target = QApplication::focusWidget();
    if (!target || target->window() != this) return;
    focusNavigationTarget_ = target;
    target->installEventFilter(this);
    focusNavigationTimer_->start(2000);
    statusBar()->showMessage(I18n::text(focusNavigationHint));
}

void MainWindow::finishFocusNavigation() {
    if (focusNavigationTarget_) focusNavigationTarget_->removeEventFilter(this);
    focusNavigationTarget_.clear();
    focusNavigationTimer_->stop();
    if (statusBar()->currentMessage() == I18n::text(focusNavigationHint)) statusBar()->clearMessage();
}

bool MainWindow::filterFocusNavigationEvent(QEvent* event) {
    switch (event->type()) {
    case QEvent::FocusOut: case QEvent::Hide: case QEvent::MouseButtonPress:
        finishFocusNavigation();
        return false;
    case QEvent::ShortcutOverride: case QEvent::KeyPress:
        break;
    default:
        return false;
    }

    const auto* key = static_cast<QKeyEvent*>(event);
    switch (key->key()) {
    case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt: case Qt::Key_Meta: case Qt::Key_AltGr:
        return false;
    default:
        break;
    }
    const int unmodified = key->modifiers() == Qt::NoModifier ? key->key() : Qt::Key_unknown;
    switch (unmodified) {
    case Qt::Key_H: case Qt::Key_J: case Qt::Key_K: case Qt::Key_L: case Qt::Key_Escape:
        break;
    default:
        finishFocusNavigation();
        return false;
    }

    event->accept();
    if (event->type() == QEvent::KeyPress) {
        finishFocusNavigation();
        switch (unmodified) {
        case Qt::Key_H: focusProjectTree(); break;
        case Qt::Key_J: focusTerminal(); break;
        case Qt::Key_K: case Qt::Key_L: focusEditor(); break;
        default: break;
        }
    }
    return true;
}

bool MainWindow::eventFilter(QObject* watched,QEvent* event) {
    if (watched == focusNavigationTarget_ && filterFocusNavigationEvent(event)) return true;
    if (!tabSwitcher_ || tabSwitcher_->isHidden()) return QMainWindow::eventFilter(watched, event);
    if ((watched == tabs_ || watched == paneWorkspace_)
        && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
        positionTabSwitcher();
    if(watched==this && (event->type()==QEvent::WindowDeactivate || event->type()==QEvent::Hide || event->type()==QEvent::Close)) finishTabSwitch(false);
    if(event->type()==QEvent::MouseButtonPress) {
        auto* widget=qobject_cast<QWidget*>(watched);
        if(widget && widget!=tabSwitcher_ && !tabSwitcher_->isAncestorOf(widget)) finishTabSwitch(false);
    }
    if(event->type()==QEvent::ShortcutOverride || event->type()==QEvent::KeyPress) {
        auto* key=static_cast<QKeyEvent*>(event);
        const bool cycle=(key->key()==Qt::Key_Tab || key->key()==Qt::Key_Backtab) && (key->modifiers()&Qt::ControlModifier);
        const bool finish=key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter || key->key()==Qt::Key_Escape;
        if(cycle || finish) {
            if(event->type()==QEvent::ShortcutOverride) {event->accept();return true;}
            if(cycle) switchRecentTab((key->modifiers()&Qt::ShiftModifier)?-1:1);
            else finishTabSwitch(key->key()!=Qt::Key_Escape);
            return true;
        }
    }
    if(event->type()==QEvent::KeyRelease && static_cast<QKeyEvent*>(event)->key()==Qt::Key_Control) {
        finishTabSwitch(true);return true;
    }
    return QMainWindow::eventFilter(watched,event);
}

void MainWindow::goToLine() {
    auto* editor = activeEditor();
    if (!editor) return;
    bool accepted = false;
    const int line = IconDialogs::InputDialog::getInt(this, I18n::text("Go to Line"),
        I18n::text("Line number:"), 1, 1, std::numeric_limits<int>::max(), 1, &accepted);
    if (accepted) editor->goToLine(line);
}

void MainWindow::commitPendingNavigationLocation() {
    if (restoringNavigation_ || pendingNavigationLocation_.path.isEmpty()) return;
    if (!navigationHistory_.isEmpty() && navigationHistory_.back() == pendingNavigationLocation_)
        return;
    while (navigationHistory_.size() - 1 > navigationHistoryIndex_)
        navigationHistory_.removeLast();
    navigationHistory_.push_back(pendingNavigationLocation_);
    constexpr int maxHistoryEntries = 500;
    if (navigationHistory_.size() > maxHistoryEntries)
        navigationHistory_.removeFirst();
    navigationHistoryIndex_ = navigationHistory_.size() - 1;
}

void MainWindow::recordNavigationLocation() {
    if (navigationCommitTimer_) navigationCommitTimer_->stop();
    auto* editor = activeEditor();
    if (!editor || editor->isBusy() || editor->filePath().isEmpty()) {
        commitPendingNavigationLocation();
        return;
    }
    const QPoint position = editor->navigationPosition();
    const NavigationLocation current{editor->filePath(), position.x(), position.y()};
    const bool alreadyAtHistoryEntry = navigationHistoryIndex_ >= 0
        && navigationHistoryIndex_ < navigationHistory_.size()
        && navigationHistory_.at(navigationHistoryIndex_) == current;
    if (!alreadyAtHistoryEntry) {
        pendingNavigationLocation_ = current;
        commitPendingNavigationLocation();
    }
    lastObservedLocation_ = current;
    hasObservedNavigationLocation_ = true;
}

void MainWindow::restoreNavigationLocation(const QString& path, int line, int column) {
    restoringNavigation_ = true;
    navigationCommitTimer_->stop();
    lastObservedLocation_ = {QDir::cleanPath(QFileInfo(path).absoluteFilePath()), line, column};
    hasObservedNavigationLocation_ = true;
    openFile(path, line, column, 0, true);
    restoringNavigation_ = false;
}

void MainWindow::navigateHistory(int direction) {
    if (direction == 0) return;
    if (direction < 0) {
        recordNavigationLocation();
        if (navigationHistoryIndex_ <= 0) {
            statusBar()->showMessage(I18n::text("No earlier navigation location"), 1800);
            return;
        }
        --navigationHistoryIndex_;
    } else {
        commitPendingNavigationLocation();
        if (navigationHistoryIndex_ < 0 || navigationHistoryIndex_ + 1 >= navigationHistory_.size()) {
            statusBar()->showMessage(I18n::text("No later navigation location"), 1800);
            return;
        }
        ++navigationHistoryIndex_;
    }
    const NavigationLocation target = navigationHistory_.at(navigationHistoryIndex_);
    if (!QFileInfo::exists(target.path)) {
        statusBar()->showMessage(I18n::text("Navigation target no longer exists"), 2500);
        return;
    }
    restoreNavigationLocation(target.path, target.line, target.column);
}

void MainWindow::openFile() {
    const auto path = IconDialogs::FileDialog::getOpenFileName(this, I18n::text("Open file"));
    if (!path.isEmpty())
        openFile(path);
}

void MainWindow::openFile(const QString& inputPath, int line, int column, int length, bool centerTarget, FileOpenMode mode) {
    if(closing_)return;
    const QString path = QDir::cleanPath(QFileInfo(inputPath).absoluteFilePath());
    if(!property("vimRestoringLocation").toBool())if(auto* current=activeEditor();current && (line>0 || current->filePath()!=path))current->recordVimJump();
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
        if (editor && (editor->filePath()==path ||
            (editor->isBusy() && editor->filePath().isEmpty() && editor->property("openingPath").toString()==path))) {
            if (mode == FileOpenMode::Regular) pinFilePreview(editor);
            auto* view = activateEditor(editor);
            if (!filePreviews_.contains(editor)) rememberRecentFile(path);
            if (line > 0 && view) view->revealMatch(line, column, length, centerTarget);
            return;
        }
    }

    auto* editor = createEditor(path, mode);

    QPointer<EditorWidget> editorGuard(editor), viewGuard(activeEditor());
    auto* watcher = new QFutureWatcher<LoadedFile>(this);
    connect(watcher, &QFutureWatcher<LoadedFile>::finished, this,
            [this, watcher, editorGuard, viewGuard, path, line, column, length, centerTarget] {
        if(watcher->future().resultCount()==0) { watcher->deleteLater();return; }
        const LoadedFile result = watcher->future().resultAt(watcher->future().resultCount()-1);
        watcher->deleteLater();
        if (!editorGuard) return;
        const int tabIndex = tabs_->indexOf(editorGuard);
        if (tabIndex < 0) return;
        if (!result.buffer) {
            removeBufferViews(editorGuard);
            tabs_->removeTab(tabIndex);
            editorGuard->deleteLater();
            if(result.error!="Loading cancelled")IconDialogs::MessageBox::critical(this, I18n::text("Open failed"), result.error + "\n" + path);
            return;
        }
        if(editorGuard->property("loadingPreview").toBool())editorGuard->reloadBuffer(std::move(*result.buffer),path);
        else editorGuard->loadBuffer(std::move(*result.buffer), path);
        editorGuard->document()->diskState=result.diskState;
        editorGuard->setProperty("openingPath",QVariant{});
        editorGuard->setProperty("loadingPreview",false);editorGuard->setBusy(false);editorGuard->applyHistorySettings();
        if (!filePreviews_.contains(editorGuard)) rememberRecentFile(path);
        if (lsp_ && lsp_->available() && editorGuard->lspAutomaticEligible()) {
            if (lsp_->supports(path)) {
                lsp_->open(path, editorGuard->lspText());
                requestSemanticHighlighting(editorGuard);
            }
        }
        if(lsp_ && lsp_->available() && lsp_->supports(path) && !editorGuard->lspAutomaticEligible())
            statusBar()->showMessage(I18n::text("Automatic language-server loading deferred; use a manual language command to start it."),8000);
        if (lsp_ && documentEditor(activeEditor()) == editorGuard) {
            lspStatus_->setText(lsp_->status(path));
            requestSemanticHighlighting(activeEditor());
        }
        if (line > 0 && viewGuard) viewGuard->revealMatch(line, column, length, centerTarget);
        tabs_->setTabText(tabIndex, QFileInfo(path).fileName());
        if(editorGuard->property("restoreVertical").isValid())editorGuard->verticalScrollBar()->setValue(editorGuard->property("restoreVertical").toInt());
        if(editorGuard->property("restoreHorizontal").isValid())editorGuard->horizontalScrollBar()->setValue(editorGuard->property("restoreHorizontal").toInt());
        queueVimBufferAutocmd(editorGuard, "BufReadPost");
        if(editorGuard->property("restoreActive").toBool())activateEditor(editorGuard);
        updateVimAutocmdBuffer();
        updateWindowTitle();
    });
    auto loadCancelled=std::make_shared<std::atomic<bool>>(false);
    connect(editor,&QObject::destroyed,[loadCancelled]{loadCancelled->store(true);});
    auto* progress=new IconDialogs::ProgressDialog(I18n::text("Opening file and indexing lines…"),I18n::text("Cancel"),0,100,this);
    progress->setWindowModality(Qt::NonModal);progress->setMinimumDuration(400);progress->setAutoClose(true);
    connect(progress,&QProgressDialog::canceled,this,[loadCancelled]{loadCancelled->store(true);});
    connect(watcher,&QFutureWatcher<LoadedFile>::progressValueChanged,progress,&QProgressDialog::setValue);
    connect(watcher,&QFutureWatcher<LoadedFile>::finished,progress,&QProgressDialog::deleteLater);
    connect(watcher,&QFutureWatcher<LoadedFile>::resultReadyAt,this,[this,watcher,editorGuard,path](int index) {
        if(!editorGuard)return;
        const auto result=watcher->future().resultAt(index);
        if(!result.preview || !result.buffer)return;
        const auto tab=tabs_->indexOf(editorGuard);if(tab<0)return;
        editorGuard->loadBuffer(std::move(*result.buffer),path);editorGuard->setProperty("loadingPreview",true);
        tabs_->setTabText(tab,QFileInfo(path).fileName()+I18n::text(" (indexing…)"));
    });
    watcher->setFuture(QtConcurrent::run([path,loadCancelled](QPromise<LoadedFile>& promise) {
        promise.setProgressRange(0,100);LoadedFile result;
        result.buffer=std::make_shared<PieceTreeBuffer>();std::string error;
        const auto report=[&](std::size_t done,std::size_t total,std::string_view prefix) {
            if(!prefix.empty()) {
                const auto newline=prefix.rfind('\n');
                if(newline!=std::string_view::npos)prefix=prefix.substr(0,newline+1);
                else prefix=prefix.substr(0,4096);
                LoadedFile preview;preview.preview=true;preview.buffer=std::make_shared<PieceTreeBuffer>(prefix);promise.addResult(preview);
            }
            promise.setProgressValue(static_cast<int>(total?done*100/total:100));
        };
        const auto initialState=diskFileState(path);
        if(!result.buffer->loadFile(QFile::encodeName(path).toStdString(),&error,[loadCancelled]{return loadCancelled->load();},report)) {
            result.error=QString::fromStdString(error);result.buffer.reset();
        }
        if(result.buffer && diskFileState(path)!=initialState) {
            result.error=I18n::text("File changed while it was being loaded");result.buffer.reset();
        }
        result.diskState=initialState;
        promise.addResult(result);
    }));


    updateWindowTitle();
}

void MainWindow::switchGitBranch(const QString& repository, const QString& branch, quint64 generation) {
    if (generation != gitPanel_->operationGeneration() || gitPanel_->service()->repository() != repository) return;
    if (closing_ || renameInProgress_ || !replaceLockedEditors_.isEmpty() || !branchLockedEditors_.isEmpty()) {
        gitPanel_->cancelPreparation();
        statusBar()->showMessage(I18n::text("Wait for the current operation before switching branches."), 5000); return;
    }
    QVector<QPointer<EditorWidget>> affected;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
        if (!editor || editor->filePath().isEmpty()) continue;
        const auto canonical = QFileInfo(editor->filePath()).canonicalFilePath();
        const auto path = canonical.isEmpty() ? QDir::cleanPath(editor->filePath()) : canonical;
        if (!path.startsWith(repository + QLatin1Char('/'))) continue;
        if (editor->isBusy()) {
            gitPanel_->cancelPreparation();
            statusBar()->showMessage(I18n::text("Wait for the current operation before switching branches."), 5000); return;
        }
        if (editor->isModified()) {
            const auto answer = IconDialogs::MessageBox::question(this, I18n::text("Unsaved changes"),
                (branch.isEmpty()?I18n::text("Save %1 before pulling changes?"):I18n::text("Save %1 before switching branches?")).arg(editor->filePath()),
                QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Save) { gitPanel_->cancelPreparation(); return; }
            const auto pathBeforeSave = editor->filePath(); QPointer<EditorWidget> guard(editor);
            saveEditor(editor, [this, repository, branch, generation, guard, pathBeforeSave](bool ok) {
                if (generation != gitPanel_->operationGeneration()) return;
                if (ok && guard && guard->filePath() == pathBeforeSave) switchGitBranch(repository, branch, generation);
                else gitPanel_->cancelPreparation();
            });
            return;
        }
        affected.append(editor);
    }
    branchRepository_ = repository; branchLockedEditors_ = affected;
    explorerPage_->setEnabled(false); searchPanel_->setEnabled(false);
    for (const auto& editor : affected) if (editor) editor->setBusy(true);
    if (gitDiff_) gitDiff_->hide();
    if(branch.isEmpty()) gitPanel_->pullRepository(repository);
    else gitPanel_->switchBranch(repository, branch);
}

void MainWindow::finishGitBranchSwitch(const QString& repository) {
    if (repository != branchRepository_) return;
    const auto editors = branchLockedEditors_; branchLockedEditors_.clear(); branchRepository_.clear();
    explorerPage_->setEnabled(true); searchPanel_->setEnabled(true);
    QVector<QPointer<EditorWidget>> removed;
    for (const auto& editor : editors) {
        if (!editor) continue;
        const auto path = editor->filePath();
        const auto state = diskFileState(path);
        if (state == editor->document()->diskState && !editor->isModified()) { editor->setBusy(false); continue; }
        if (state == "missing") { editor->setBusy(false); removed.append(editor); continue; }
        auto* watcher = new QFutureWatcher<LoadedFile>(this);
        connect(watcher, &QFutureWatcher<LoadedFile>::finished, this, [this, watcher, editor, path, state] {
            auto loaded = watcher->result(); watcher->deleteLater();
            if (!editor) return;
            if (editor->filePath() == path && loaded.buffer && diskFileState(path) == state) {
                editor->reloadBuffer(std::move(*loaded.buffer), path);
                editor->document()->diskState = state;
                checkpointRecovery();
            } else statusBar()->showMessage(I18n::text("Could not reload %1 after switching branches.").arg(path), 7000);
            editor->setBusy(false);
        });
        watcher->setFuture(QtConcurrent::run([path] {
            LoadedFile loaded; loaded.buffer = std::make_shared<PieceTreeBuffer>(); std::string error;
            if (!loaded.buffer->loadFile(QFile::encodeName(path).toStdString(), &error)) {
                loaded.error = QString::fromStdString(error); loaded.buffer.reset();
            }
            return loaded;
        }));
    }
    // These tabs were saved before the switch and no longer exist on this branch.
    if (!removed.isEmpty()) requestClose(removed, false);
}

void MainWindow::stageGitPaths(const QString& repository, const QStringList& paths, quint64 generation) {
    if (generation != gitPanel_->operationGeneration() || gitPanel_->service()->repository() != repository) return;
    QStringList absolute;
    for (const auto& path : paths) absolute.append(QDir::cleanPath(repository + QLatin1Char('/') + path));
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
        if (!editor || !absolute.contains(QDir::cleanPath(editor->filePath()))) continue;
        if (editor->isBusy()) {
            statusBar()->showMessage(I18n::text("File is busy. Retry Stage when its operation finishes."), 5000); gitPanel_->cancelPreparation(); return;
        }
        if (!editor->isModified()) continue;
        const auto answer = IconDialogs::MessageBox::question(this, I18n::text("Unsaved changes"),
            I18n::text("Save %1 before staging? Git uses the saved file.").arg(editor->filePath()),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save) { gitPanel_->cancelPreparation(); return; }
        const QString previousPath = editor->filePath();
        QPointer<EditorWidget> guard(editor);
        saveEditor(editor, [this, guard, previousPath, repository, paths, generation](bool ok) {
            if (generation != gitPanel_->operationGeneration()) return;
            if (ok && guard && guard->filePath() == previousPath) stageGitPaths(repository, paths, generation);
            else gitPanel_->cancelPreparation();
        });
        return;
    }
    gitPanel_->stagePaths(repository, paths);
}

void MainWindow::initializeFileMonitoring() {
    documentWatcher_ = new QFileSystemWatcher(this);
    documentChangeTimer_ = new QTimer(this);
    documentChangeTimer_->setObjectName("externalFileDebounce");
    documentChangeTimer_->setSingleShot(true);
    documentChangeTimer_->setInterval(250);
    const auto queue = [this](const QString&) { documentChangeTimer_->start(); };
    connect(documentWatcher_, &QFileSystemWatcher::fileChanged, this, queue);
    connect(documentWatcher_, &QFileSystemWatcher::directoryChanged, this, queue);
    connect(documentChangeTimer_, &QTimer::timeout, this, &MainWindow::checkExternalFileChanges);
    // Also retries busy documents, failed watches, and paths recreated after deletion.
    auto* poll = new QTimer(this);
    poll->setInterval(2000);
    connect(poll, &QTimer::timeout, this, &MainWindow::checkExternalFileChanges);
    poll->start();
    documentChangeTimer_->start();
}

void MainWindow::checkExternalFileChanges() {
    if(closing_ || !tabs_ || fileCheckRunning_)return;
    struct Observation {QHash<QString,QByteArray> states;QSet<QString> files,directories;};
    QStringList paths;QHash<QString,quint64> revisions;QHash<QString,QByteArray> expected;
    for(int i=0;i<tabs_->count();++i)if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i))) {
        const auto path=editor->filePath();if(path.isEmpty())continue;
        paths<<path;revisions[path]=editor->lspRevision();expected[path]=editor->document()->diskState;
    }
    fileCheckRunning_=true;
    auto* worker=new QFutureWatcher<Observation>(this);
    connect(worker,&QFutureWatcher<Observation>::finished,this,[this,worker,revisions,expected] {
        const auto observations=worker->result();worker->deleteLater();fileCheckRunning_=false;
        if(closing_)return;
        const auto& files=observations.files;const auto& directories=observations.directories;
    const auto reconcile = [this](const QStringList& watched, const QSet<QString>& wanted) {
        for (const auto& path : watched)
            if (!wanted.contains(path)) documentWatcher_->removePath(path);
        for (const auto& path : wanted)
            if (!watched.contains(path)) documentWatcher_->addPath(path);
    };
    reconcile(documentWatcher_->files(), files);
    reconcile(documentWatcher_->directories(), directories);

    for (int i = 0; i < tabs_->count(); ++i) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
        if (!editor || editor->isBusy() || editor->filePath().isEmpty()
            || externallyReloading_.contains(editor)) continue;
        const QString path = editor->filePath();
        if(!observations.states.contains(path) || revisions.value(path)!=editor->lspRevision() || expected.value(path)!=editor->document()->diskState){documentChangeTimer_->start();continue;}
        const auto state = observations.states.value(path);
        auto& tracked = externalFileStates_[editor];
        if (tracked.path != path) { tracked = {}; tracked.path = path; }
        if (state == editor->document()->diskState) {
            tracked.observed.clear(); tracked.acknowledged.clear();
            continue;
        }
        if (tracked.acknowledged == state) continue;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (tracked.observed != state) {
            tracked.observed = state; tracked.observedAt = now;
            documentChangeTimer_->start();
            continue;
        }
        if (now - tracked.observedAt < 250) { documentChangeTimer_->start(); continue; }
        if (state == "missing") {
            tracked.acknowledged = state;
            editor->markFileMissing();
            statusBar()->showMessage(I18n::text("File deleted outside Turtle Code: %1. The open copy has been kept; save it to restore the file.").arg(path), 10000);
            continue;
        }
        if (!editor->isModified()) { reloadExternalFile(editor, path, state); continue; }
        // Do not stack dialogs over Save, Close, or other editor operations.
        if (externalChangeDialog_ || QApplication::activeModalWidget()) continue;
        auto* message = new IconDialogs::MessageBox(QMessageBox::Warning,
            I18n::text("File changed on disk"),
            I18n::text("%1 was changed outside Turtle Code and has unsaved edits. Reloading will discard your edits.").arg(path),
            QMessageBox::NoButton, this);
        message->setObjectName("externalFileConflict");
        message->setAttribute(Qt::WA_DeleteOnClose);
        auto* reload = message->addButton(I18n::text("Reload from Disk"), QMessageBox::DestructiveRole);
        reload->setIcon(Codicons::icon("refresh"));
        auto* keep = message->addButton(I18n::text("Keep My Changes"), QMessageBox::RejectRole);
        keep->setIcon(Codicons::icon("edit"));
        message->setDefaultButton(keep);
        message->setEscapeButton(keep);
        externalChangeDialog_ = message;
        QPointer<EditorWidget> guard(editor);
        const auto revision = editor->lspRevision();
        connect(message, &QDialog::finished, this, [this, message, reload, guard, path, state, revision] {
            externalChangeDialog_.clear();
            if (!guard || tabs_->indexOf(guard) < 0 || guard->filePath() != path || closing_) return;
            if (diskFileState(path) != state || guard->lspRevision() != revision || guard->isBusy()) {
                documentChangeTimer_->start(); return;
            }
            if (message->clickedButton() == reload) reloadExternalFile(guard, path, state);
            else externalFileStates_[guard].acknowledged = state;
            // Keeping edits does not advance Document::diskState: Save still
            // asks before overwriting the external version.
        });
        message->open();
    }
    });
    worker->setFuture(QtConcurrent::run([paths] {
        Observation result;
        for(const auto& path:paths) {
            const QFileInfo info(path);result.states[path]=diskFileState(path);
            if(info.isFile())result.files.insert(path);
            auto parent=info.absoluteDir();
            while(!parent.exists() && !parent.isRoot())parent.setPath(QFileInfo(parent.absolutePath()).absolutePath());
            result.directories.insert(parent.absolutePath());
        }
        return result;
    }));
}

void MainWindow::reloadExternalFile(EditorWidget* editor, const QString& path, const QByteArray& state) {
    externallyReloading_.insert(editor);
    QPointer<EditorWidget> guard(editor);
    const auto revision = editor->lspRevision();
    auto* watcher = new QFutureWatcher<LoadedFile>(this);
    connect(watcher, &QFutureWatcher<LoadedFile>::finished, this, [this, watcher, guard, path, state, revision] {
        auto loaded = watcher->result(); watcher->deleteLater();
        if (!guard) return;
        externallyReloading_.remove(guard);
        // Reading never locks the editor. Discard stale results if the user
        // edits, saves, renames, closes, or starts another operation meanwhile.
        if (closing_ || tabs_->indexOf(guard) < 0 || guard->isBusy() || guard->filePath() != path || guard->lspRevision() != revision
            || diskFileState(path) != state || loaded.diskState != state) {
            documentChangeTimer_->start(); return;
        }
        if (!loaded.buffer) {
            statusBar()->showMessage(I18n::text("Could not reload %1: %2").arg(path, loaded.error), 7000);
            return;
        }
        guard->reloadBuffer(std::move(*loaded.buffer), path);
        guard->document()->diskState = state;
        externalFileStates_.remove(guard);
        checkpointRecovery();
        statusBar()->showMessage(I18n::text("Reloaded from disk: %1").arg(path), 3000);
    });
    watcher->setFuture(QtConcurrent::run([path, state] {
        LoadedFile loaded;
        if (diskFileState(path) != state) return loaded;
        loaded.buffer = std::make_shared<PieceTreeBuffer>();
        std::string error;
        if (!loaded.buffer->loadFile(QFile::encodeName(path).toStdString(), &error)) {
            loaded.error = QString::fromStdString(error); loaded.buffer.reset();
        }
        loaded.diskState = diskFileState(path);
        return loaded;
    }));
}

void MainWindow::saveCurrentFile() {
    saveEditor(activeEditor());
}

void MainWindow::saveAllFiles() {
    QList<QPointer<EditorWidget>> modified;
    int busy = 0;
    for (int i = 0; i < tabs_->count(); ++i) if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i))) {
        if (!editor->isModified()) continue;
        if (editor->isBusy()) { ++busy; continue; }
        modified.append(editor);
    }
    for (const auto& editor : modified) if (editor) saveEditor(editor);
    if (busy) statusBar()->showMessage(QString(I18n::text("Saving %1 files. %2 busy files were skipped; retry after their current operation finishes.")).arg(modified.size()).arg(busy), 7000);
    else if (modified.isEmpty()) statusBar()->showMessage(I18n::text("All files are already saved."), 3000);
}

void MainWindow::saveEditor(EditorWidget* editor,std::function<void(bool)> completion,bool saveAs) {
    const auto fail=[&]{if(completion)completion(false);};
    if(!editor || editor->isBusy()){fail();return;}
    QString path=editor->filePath();
    bool choosePath=saveAs || path.isEmpty();
    if(!choosePath && diskFileState(path)!=editor->document()->diskState) {
        IconDialogs::MessageBox conflict(QMessageBox::Warning,I18n::text("File changed on disk"),
            I18n::text("The file was changed or deleted outside Turtle Code. Choose how to save your changes."),QMessageBox::NoButton,this);
        auto* overwrite=conflict.addButton(I18n::text("Overwrite"),QMessageBox::DestructiveRole); overwrite->setIcon(Codicons::icon("save"));
        auto* other=conflict.addButton(I18n::text("Save As..."),QMessageBox::ActionRole); other->setIcon(Codicons::icon("save-as"));
        auto* cancel=conflict.addButton(QMessageBox::Cancel);conflict.setDefaultButton(cancel);conflict.exec();
        if(conflict.clickedButton()==other)choosePath=true;
        else if(conflict.clickedButton()!=overwrite){fail();return;}
    }
    if(choosePath) {
        path=IconDialogs::FileDialog::getSaveFileName(this,I18n::text("Save file"),path);
        if(path.isEmpty()){fail();return;}
    }
    const auto expected=diskFileState(path);
    QPointer<EditorWidget> guard(editor);
    editor->prepareEditorConfigSave(path,[this,guard,path,expected,completion](bool ok) {
        if(!ok || !guard){if(completion)completion(false);return;}
        writeEditor(guard,path,expected,completion);
    });
}

void MainWindow::writeEditor(EditorWidget* editor,const QString& path,const QByteArray& expected,std::function<void(bool)> completion,bool reportDialog) {
    const auto previousPath=editor->filePath();
    editor->setBusy(true);
    QPointer<EditorWidget> guard(editor);
    statusBar()->showMessage(I18n::text("Saving…"));
    auto* watcher=new QFutureWatcher<SavedFile>(this);
    connect(watcher,&QFutureWatcher<SavedFile>::finished,this,[this,watcher,guard,path,previousPath,completion,reportDialog] {
        const auto result=watcher->result();watcher->deleteLater();
        if(!guard){if(completion)completion(false);return;}
        guard->setBusy(false);guard->applyHistorySettings();
        if(!result.ok) {
            if(reportDialog && result.error!="Saving cancelled")IconDialogs::MessageBox::critical(this,I18n::text("Save failed"),result.error);
            if(completion)completion(false);
            return;
        }
        if(lsp_ && previousPath!=path)lsp_->close(previousPath);
        if(previousPath!=path) debugPanel_->renameSource(previousPath,path);
        guard->completeAsyncSave(path);
        if(QFileInfo(path).fileName()==".editorconfig")
            for(auto* window:QApplication::topLevelWidgets())
                if(auto* main=qobject_cast<MainWindow*>(window))
                    for(auto* open:main->findChildren<EditorWidget*>())open->refreshEditorConfig();
        guard->setDebugBreakpoints(debugPanel_->breakpoints(path),debugPanel_->disabledBreakpoints(path));guard->document()->diskState=result.diskState;
        if(lsp_ && lsp_->available() && lsp_->supports(path) && guard->lspEligible() &&
           (guard->lspAutomaticEligible() || lsp_->documentVersion(path)>0)) {
            if(previousPath!=path)lsp_->open(path,guard->lspText());
            else lsp_->change(path,guard->lspText());
            lsp_->saved(path);
        }
        const auto index=tabs_->indexOf(documentEditor(guard));
        if(index>=0){tabs_->setTabText(index,QFileInfo(path).fileName());tabs_->setTabToolTip(index,path);}
        updateWindowTitle();statusBar()->showMessage(I18n::text("Saved"),2000);
        checkpointRecovery();
        gitPanel_->refresh();
        queueVimBufferAutocmd(guard, "BufWritePost");
        if(completion)completion(true);
    });
    const auto document=editor->document();
    auto cancelled=std::make_shared<std::atomic<bool>>(false);
    auto percent=std::make_shared<std::atomic<int>>(0);
    auto* progress=new IconDialogs::ProgressDialog(I18n::text("Saving…"),I18n::text("Cancel"),0,100,this);
    progress->setWindowModality(Qt::NonModal);progress->setMinimumDuration(300);
    connect(progress,&QProgressDialog::canceled,progress,[cancelled]{cancelled->store(true);});
    connect(editor,&QObject::destroyed,[cancelled]{cancelled->store(true);});
    connect(editor,&EditorWidget::vimExInterruptRequested,progress,&QProgressDialog::cancel);
    connect(editor,&EditorWidget::vimExInterruptRequested,progress,[cancelled]{cancelled->store(true);});
    auto* timer=new QTimer(progress);timer->setInterval(50);
    connect(timer,&QTimer::timeout,progress,[percent,progress,cancelled]{if(progress->wasCanceled())cancelled->store(true);else progress->setValue(percent->load());});timer->start();
    connect(watcher,&QFutureWatcher<SavedFile>::finished,progress,&QProgressDialog::deleteLater);
    watcher->setFuture(QtConcurrent::run([document,path,expected,cancelled,percent] {
        SavedFile result;
        try {
            if(diskFileState(path)!=expected){result.error=I18n::text("The file changed while saving. Save again or choose another path.");return result;}
            const auto snapshot=document->buffer().snapshot();
            QSaveFile file(path);
            if(!file.open(QIODevice::WriteOnly)){result.error=file.errorString();return result;}
            std::size_t written=0;
            result.ok=snapshot.forEachChunk([&](std::string_view chunk) {
                if(cancelled->load())return false;
                const bool ok=file.write(chunk.data(),qint64(chunk.size()))==qint64(chunk.size());
                written+=chunk.size();percent->store(int(snapshot.size()?written*99/snapshot.size():99));return ok;
            });
            if(cancelled->load()){file.cancelWriting();result.ok=false;result.error="Saving cancelled";return result;}
            if(diskFileState(path)!=expected) {
                file.cancelWriting();result.ok=false;result.error=I18n::text("The file changed while saving. Save again or choose another path.");return result;
            }
            if(result.ok)result.ok=file.commit();
            if(!result.ok)result.error=file.errorString();
            else result.diskState=diskFileState(path);
        } catch(const std::exception& error){result.ok=false;result.error=QString::fromUtf8(error.what());}
        return result;
    }));
}

void MainWindow::saveCurrentFileAs() {
    saveEditor(activeEditor(),{},true);
}

void MainWindow::updateWindowTitle() {
    updateEditorHeaders();
    auto* editor = activeEditor();
    QString projectName;
    if (!projectRoots_.isEmpty()) {
        projectName = QFileInfo(projectRoots_.front()).fileName();
    }
    const QString fileName = editor ? QFileInfo(editor->filePath()).fileName()
                                    + (editor->isModified() ? " •" : "") : QString();
    QStringList parts;
    if (!fileName.isEmpty()) parts.push_back(fileName);
    if (!projectName.isEmpty()) parts.push_back(projectName);
    parts.push_back("Turtle Code");
    setWindowTitle(parts.join(" — "));
}

void MainWindow::showProjectSearch(const QString& directory) {
    searchPanel_->setSearchDirectory(directory);
    sidebarStack_->setCurrentWidget(searchPanel_);
    setSidebarVisible(true);
    searchPanel_->focusSearch();
}

void MainWindow::startProjectReplace(ProjectReplaceRequest request) {
    QSet<QString> selectedPaths(request.selectedPaths.cbegin(), request.selectedPaths.cend());
    replaceLockedEditors_.clear();
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (!editor) continue;
        const QString path = editor->filePath().isEmpty()
            ? tabs_->tabToolTip(index) : editor->filePath();
        if (!selectedPaths.contains(path) && !request.matches.containsPath(path)) continue;
        if (editor->isModified() || editor->isBusy()) {
            request.protectedPaths.push_back(path);
        } else {
            editor->setBusy(true);
            replaceLockedEditors_.push_back(editor);
        }
    }
    explorerPage_->setEnabled(false);
    searchPanel_->startReplace(std::move(request));
}

void MainWindow::finishProjectReplace(const ProjectReplaceResult& result) {
    explorerPage_->setEnabled(true);
    const QSet<QString> changed(result.changedPaths.cbegin(), result.changedPaths.cend());
    for (const auto& widget : replaceLockedEditors_) {
        auto* editor = qobject_cast<EditorWidget*>(widget.data());
        if (!editor) continue;
        const QString path = editor->filePath();
        if (!changed.contains(path)) {
            editor->setBusy(false);
            continue;
        }
        QPointer<EditorWidget> guard(editor);
        auto* watcher = new QFutureWatcher<LoadedFile>(this);
        connect(watcher, &QFutureWatcher<LoadedFile>::finished, this,
                [this, watcher, guard, path] {
            LoadedFile loaded = watcher->result();
            watcher->deleteLater();
            if (!guard) return;
            if (guard->filePath() == path && loaded.buffer)
                guard->reloadBuffer(std::move(*loaded.buffer), path);
            guard->setBusy(false);
            if (!loaded.buffer)
                statusBar()->showMessage(tr("Could not reload %1: %2")
                    .arg(path, loaded.error), 8000);
        });
        watcher->setFuture(QtConcurrent::run([path] {
            LoadedFile loaded;
            loaded.buffer=std::make_shared<PieceTreeBuffer>(); std::string error;
            if(!loaded.buffer->loadFile(QFile::encodeName(path).toStdString(),&error)) {
                loaded.error=QString::fromStdString(error);loaded.buffer.reset();
            }
            return loaded;
        }));
    }
    replaceLockedEditors_.clear();
    scheduleFilePreviewCleanup();
}

void MainWindow::configureLsp() {
    for(int i=0;i<tabs_->count();++i) if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i))) {
        editor->clearSemanticHighlighting(); editor->setDiagnostics({}); editor->lspPolicy().reset();
        editor->setProperty("semanticPending",false);
    }
    lspProblems_.clear();
    if(!projectRoots_.isEmpty()) lsp_->setRoot(projectRoots_.front());
    lsp_->configure(LspConfiguration::load());
    for(int i=0;i<tabs_->count();++i)
        if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i));editor && !editor->isBusy() && editor->lspAutomaticEligible() && lsp_->supports(editor->filePath()))
            lsp_->open(editor->filePath(),editor->lspText());
    requestSemanticHighlighting(activeEditor(),true);
    scheduleOutline();
}
bool MainWindow::ensureManualLsp(EditorWidget* editor,std::function<void()> retry) {
    if(!editor || editor->isBusy() || !lsp_ || !lsp_->supports(editor->filePath())) return false;
    if(!editor->lspEligible()) {
        statusBar()->showMessage(I18n::text("This document exceeds the language server transfer budget (32 MiB)."),8000); return false;
    }
    if(!lsp_->available()) { statusBar()->showMessage(lsp_->status(editor->filePath()),5000); return false; }
    if(!lsp_->documentVersion(editor->filePath())) {
        lsp_->open(editor->filePath(),editor->lspText());
    }
    if(lsp_->ready(editor->filePath())) return true;
    statusBar()->showMessage(lsp_->status(editor->filePath()),5000);
    const auto serial=editor->property("manualLspWait").toULongLong()+1; editor->setProperty("manualLspWait",serial);
    QPointer<EditorWidget> guard(editor); const auto path=editor->filePath(); const auto revision=editor->lspRevision();
    auto connection=std::make_shared<QMetaObject::Connection>();
    *connection=connect(lsp_,&LspManager::statusChanged,this,[this,guard,path,revision,serial,retry,connection](const QString&) {
        if(!lsp_->ready(path)) return;
        disconnect(*connection);
        if(guard && activeEditor()==guard && guard->filePath()==path && guard->lspRevision()==revision
            && guard->property("manualLspWait").toULongLong()==serial) retry();
    });
    QTimer::singleShot(15000,this,[connection]{QObject::disconnect(*connection);});
    return false;
}

void MainWindow::formatDocument() {
    if(closing_ || renameInProgress_ || !replaceLockedEditors_.isEmpty()) return;
    auto* editor=activeEditor();
    if(!editor || editor->isBusy()) return;
    if(editor->filePath().isEmpty() || !lsp_->supports(editor->filePath())) {
        statusBar()->showMessage(I18n::text("Formatting is unavailable for this document. Save it with a supported extension first."),6000);return;
    }
    if(!ensureManualLsp(editor,[this]{formatDocument();})) return;
    if(!lsp_->canFormat(editor->filePath())) {
        statusBar()->showMessage(I18n::text("The language server does not support document formatting."),6000);return;
    }
    const auto path=editor->filePath();const auto revision=editor->lspRevision();
    const auto generation=editor->property("formatGeneration").toULongLong()+1;
    editor->setProperty("formatGeneration",generation);
    const auto before=QByteArray::fromStdString(editor->snapshotBuffer().text());
    lsp_->change(path,editor->lspText());
    QPointer<EditorWidget> guard(editor);
    statusBar()->showMessage(I18n::text("Formatting document…"),10000);
    editor->refreshEditorConfig();
    lsp_->formatDocument(path,editor->tabSize(),editor->insertsSpaces(),[this,guard,path,revision,generation,before](QJsonValue result) {
        if(closing_ || renameInProgress_ || !replaceLockedEditors_.isEmpty() || !guard || activeEditor()!=guard || guard->isBusy() || guard->filePath()!=path
            || guard->lspRevision()!=revision || guard->property("formatGeneration").toULongLong()!=generation) return;
        if(result.isUndefined()) return; // Preserve the protocol error shown by LspClient.
        if(result.isNull()) {statusBar()->showMessage(I18n::text("No formatting changes needed."),4000);return;}
        QString error;std::vector<EditChange> changes;
        if(!result.isArray() || !WorkspaceEdits::convertFormatting(before,result.toArray(),&changes,&error)
            || !guard->validateWorkspaceEdits(before,changes)) {
            statusBar()->showMessage(I18n::text("Formatting not applied: invalid edits from the language server.")+" "+error,8000);return;
        }
        if(changes.empty()) {statusBar()->showMessage(I18n::text("No formatting changes needed."),4000);return;}
        guard->applyWorkspaceEdits(std::move(changes));
        statusBar()->showMessage(I18n::text("Document formatted. Changes are not saved yet."),5000);
    });
}

void MainWindow::requestSemanticHighlighting(EditorWidget* editor, bool force) {
    if (!lsp_ || !editor || activeEditor()!=editor || editor->isBusy()
        || !editor->lspAutomaticEligible() || !lsp_->supports(editor->filePath())
        || !QSettings().value("lsp/semanticHighlighting",true).toBool()
        || !lsp_->ready(editor->filePath()) || !lsp_->semanticTokensAvailable(editor->filePath())) return;
    if(force) editor->setProperty("semanticForcePending",true);
    const bool effectiveForce=editor->property("semanticForcePending").toBool();
    const auto now=QDateTime::currentMSecsSinceEpoch();
    const auto mode=editor->lspPolicy().mode(editor->byteSize(),lsp_->semanticTokensRangeAvailable(editor->filePath()),lsp_->semanticTokensFullAvailable(editor->filePath()),now);
    auto* timer=editor->findChild<QTimer*>("semanticSync");
    if(mode==LspAdaptivePolicy::Mode::None) {
        lsp_->cancelSemanticTokens(editor->filePath()); editor->clearSemanticHighlighting();
        if(!editor->property("semanticLimitedNotice").toBool()) {
            statusBar()->showMessage(I18n::text("Automatic LSP highlighting limited; manual language commands remain available."),8000);
            editor->setProperty("semanticLimitedNotice",true);
        }
        const auto remaining=editor->lspPolicy().remainingPause(now);
        if(timer && remaining>0) timer->start(static_cast<int>(remaining+1));
        return;
    }
    editor->setProperty("semanticLimitedNotice",false);
    const bool ranged=mode==LspAdaptivePolicy::Mode::Range;
    const auto range=ranged ? editor->visibleLspRange() : QJsonObject{};
    const auto window=QString::fromUtf8(QJsonDocument(range).toJson(QJsonDocument::Compact));
    const auto revision=editor->lspRevision(), generation=editor->semanticGeneration();
    if (editor->property("semanticPending").toBool()
        && editor->property("semanticPendingRevision").toULongLong()==revision
        && editor->property("semanticPendingGeneration").toULongLong()==generation
        && editor->property("semanticPendingWindow").toString()==window) return;
    if (!effectiveForce && editor->property("semanticAppliedRevision").isValid()
        && editor->property("semanticAppliedRevision").toULongLong()==revision
        && editor->property("semanticAppliedGeneration").toULongLong()==generation
        && editor->property("semanticAppliedWindow").toString()==window) return;
    const auto last=editor->property("semanticLastRequest").toLongLong();
    const auto wait=editor->lspPolicy().delay(editor->byteSize())-(now-last);
    if(last && wait>0 && timer) { timer->start(static_cast<int>(wait)); return; }
    const auto serial=editor->property("semanticSerial").toULongLong()+1;
    editor->setProperty("semanticForcePending",false);
    editor->setProperty("semanticSerial",serial); editor->setProperty("semanticLastRequest",now);
    editor->setProperty("semanticPending",true); editor->setProperty("semanticPendingWindow",window);
    editor->setProperty("semanticPendingRevision",revision); editor->setProperty("semanticPendingGeneration",generation);
    const QString path=editor->filePath();
    const int first=ranged?range.value("start").toObject().value("line").toInt():0;
    const int lastLine=ranged?range.value("end").toObject().value("line").toInt():0;
    // Sync the complete document only when it changed; viewport requests decode
    // a bounded text slice rather than splitting a large document into lines.
    if(editor->property("semanticSyncedRevision").toULongLong()!=revision || lsp_->documentVersion(path)==0) {
        lsp_->change(path,editor->lspText()); editor->setProperty("semanticSyncedRevision",revision);
    }
    const QString text=ranged ? editor->lspRangeText(first,lastLine) : editor->lspText();
    auto clock=std::make_shared<QElapsedTimer>(); clock->start();
    QPointer<EditorWidget> guard(editor);
    lsp_->semanticTokens(path,[this,guard,path,text,first,ranged,revision,generation,serial,window,clock](QJsonValue response) {
        if (!guard || guard->filePath()!=path) return;
        const auto object=response.toObject(); const auto data=object.value("data").toArray();
        const auto count=static_cast<std::size_t>(data.size()/5); const std::size_t budget=ranged?20000:50000;
        if(!response.isObject() || !object.value("data").isArray() || count>budget) {
            guard->lspPolicy().observe(clock->elapsed(),count,false,QDateTime::currentMSecsSinceEpoch());
            if(guard->property("semanticSerial").toULongLong()==serial) {
                guard->setProperty("semanticPending",false); guard->setProperty("semanticForcePending",true);
                if(auto* retry=guard->findChild<QTimer*>("semanticSync")) retry->start(std::max(1000,guard->lspPolicy().delay(guard->byteSize())));
            }
            return;
        }
        if(guard->lspRevision()!=revision || guard->semanticGeneration()!=generation || guard->property("semanticSerial").toULongLong()!=serial) return;
        auto* watcher=new QFutureWatcher<SemanticHighlightResult>(guard);
        connect(watcher,&QFutureWatcher<SemanticHighlightResult>::finished,this,[watcher,guard,path,revision,generation,serial,window,clock,count] {
            const auto result=watcher->result(); watcher->deleteLater();
            if(!guard || guard->filePath()!=path) return;
            if(guard->lspRevision()!=revision || guard->semanticGeneration()!=generation || guard->property("semanticSerial").toULongLong()!=serial) return;
            guard->setProperty("semanticPending",false);
            guard->setProperty("semanticLatencyMs",clock->elapsed());
            guard->lspPolicy().observe(clock->elapsed(),count,result.valid,QDateTime::currentMSecsSinceEpoch());
            if(!result.valid) {
                guard->setProperty("semanticForcePending",true);
                if(auto* retry=guard->findChild<QTimer*>("semanticSync")) retry->start(std::max(1000,guard->lspPolicy().delay(guard->byteSize())));
                return;
            }
            if(!QSettings().value("lsp/semanticHighlighting",true).toBool()) return;
            guard->setSemanticHighlighting(result,revision,generation);
            guard->setProperty("semanticAppliedRevision",revision); guard->setProperty("semanticAppliedGeneration",generation);
            guard->setProperty("semanticAppliedWindow",window);
            if(guard->property("semanticForcePending").toBool())
                if(auto* retry=guard->findChild<QTimer*>("semanticSync")) retry->start(guard->lspPolicy().delay(guard->byteSize()));
        });
        watcher->setFuture(QtConcurrent::run([object,text,first,budget] {
            return SemanticHighlighting::decode(object.value("data").toArray(),object.value("legend").toObject(),text,first,budget);
        }));
    },range);
}

void MainWindow::requestCompletion(EditorWidget* editor, bool automatic) {
    if(!automatic && !ensureManualLsp(editor,[this,guard=QPointer<EditorWidget>(editor)]{if(guard) requestCompletion(guard,false);})) return;
    if(automatic && editor && !editor->lspPolicy().automaticFeatures(editor->byteSize(),QDateTime::currentMSecsSinceEpoch())) return;
    if (!editor || editor->isBusy() || !editor->lspEligible() || !lsp_->supports(editor->filePath()) || (automatic && !editor->hasFocus())) return;
    if (automatic && (!QSettings().value("lsp/autoCompletion", true).toBool() || editor->applyingCompletion())) return;
    if (!lsp_->available() || !lsp_->ready(editor->filePath())) { if (!automatic) statusBar()->showMessage(lsp_->status(editor->filePath()), 5000); return; }
    const QString path = editor->filePath(); const auto revision = editor->lspRevision(); const auto position = editor->lspPosition();
    if (automatic && !editor->shouldAutoComplete()) return;
    if (!automatic) editor->setFocus();
    lsp_->change(path, editor->lspText());
    QPointer<EditorWidget> guard(editor);
    lsp_->completion(path, position.x(), position.y(), [this, guard, path, revision, position, automatic](QJsonValue result) {
        if (!guard || guard->filePath() != path || activeEditor() != guard || guard->lspRevision() != revision || guard->lspPosition() != position || !guard->hasFocus()) return;
        if (automatic && !QSettings().value("lsp/autoCompletion", true).toBool()) return;
        auto items = result.isArray() ? result.toArray() : result.toObject().value("items").toArray();
        const auto defaults = result.toObject().value("itemDefaults").toObject();
        if (!defaults.isEmpty()) for (int i=0; i<items.size(); ++i) {
            auto item = items[i].toObject();
            if (!item.contains("insertTextFormat") && defaults.contains("insertTextFormat")) item["insertTextFormat"] = defaults.value("insertTextFormat");
            if (!item.contains("textEdit") && defaults.value("editRange").isObject()) {
                auto edit = defaults.value("editRange").toObject();
                if (edit.contains("start")) edit = QJsonObject{{"range", edit}};
                edit["newText"] = item.value("textEditText").toString(item.value("insertText").toString(item.value("label").toString()));
                item["textEdit"] = edit;
            }
            items[i] = item;
        }
        QVector<QJsonValue> sorted;
        for (const auto& item : items) sorted.push_back(item);
        std::stable_sort(sorted.begin(), sorted.end(), [](const QJsonValue& a, const QJsonValue& b) {
            const auto x=a.toObject(), y=b.toObject();
            return x.value("sortText").toString(x.value("label").toString()) < y.value("sortText").toString(y.value("label").toString());
        });
        items = {}; for (const auto& item : sorted) items.append(item);
        guard->showCompletions(items);
        if (items.isEmpty() && !automatic) statusBar()->showMessage(I18n::text("No completions"), 2000);
    });
}
void MainWindow::requestEditorHover(EditorWidget* editor, int line, int column, const QPoint& global, bool hasDiagnostic) {
    const auto generation = ++editorHoverGeneration_;
    const auto path = editor->filePath();
    const auto revision = editor->lspRevision();
    const QPointer<EditorWidget> guard(editor);
    const auto isCurrent = [this, guard, path, revision, generation] {
        return guard && guard->isVisible() && !guard->isBusy() && activeEditor() == guard &&
               guard->filePath() == path && guard->lspRevision() == revision && generation == editorHoverGeneration_;
    };
    const auto showLanguageHover = [this, guard, path, line, column, global, hasDiagnostic, isCurrent] {
        if (hasDiagnostic || !isCurrent() || !lsp_ || !lsp_->ready(path) || !guard->lspEligible() || !lsp_->supports(path) ||
            !guard->lspPolicy().automaticFeatures(guard->byteSize(), QDateTime::currentMSecsSinceEpoch())) return;
        lsp_->change(path, guard->lspText());
        lsp_->hover(path, line, column, [guard, global, isCurrent](QJsonValue value) {
            if (!isCurrent() || (QCursor::pos() - global).manhattanLength() > 12) return;
            const auto contents = value.toObject().value("contents");
            const auto plain = [](QJsonValue item) { return item.isString() ? item.toString() : item.toObject().value("value").toString(); };
            QString text;
            if (contents.isArray()) for (const auto& item : contents.toArray()) text += plain(item) + '\n';
            else text = plain(contents);
            if (!text.trimmed().isEmpty()) QToolTip::showText(global, "<pre>" + text.left(8000).toHtmlEscaped() + "</pre>", guard);
        });
    };

    if (!path.isEmpty() && path == debugPath_ && isCurrent()) {
        const auto target = editor->debugHoverExpression(editor->viewport()->mapFromGlobal(global));
        if (target && debugPanel_->evaluateHover(target->text, [this, guard, target=*target, isCurrent, showLanguageHover](const QString& value) {
            if (!isCurrent()) return;
            auto* viewport = guard->viewport();
            const auto cursor = QCursor::pos();
            if (!target.area.contains(viewport->mapFromGlobal(cursor))) return;
            const auto hovered = guard->debugHoverExpression(viewport->mapFromGlobal(cursor));
            if (!hovered || hovered->text != target.text || hovered->area != target.area) return;
            if (value.isEmpty()) {
                showLanguageHover();
                return;
            }
            debugHoverTip_ = "<pre>" + target.text.toHtmlEscaped() + " = " + value.left(2000).toHtmlEscaped() + "</pre>";
            QToolTip::showText(cursor, debugHoverTip_, viewport, target.area);
        })) return;
    }
    showLanguageHover();
}

void MainWindow::showCodeHelp(bool signature) {
    auto* editor = activeEditor();
    if (!editor) return;
    if(!ensureManualLsp(editor,[this,signature]{showCodeHelp(signature);})) return;
    const auto path=editor->filePath(); const auto revision=editor->lspRevision(); const auto pos=editor->lspPosition();
    lsp_->change(path, editor->lspText()); QPointer<EditorWidget> guard(editor);
    auto callback = [this, guard, path, revision, pos, signature](QJsonValue value) {
        if (!guard || activeEditor()!=guard || guard->filePath()!=path || guard->lspRevision()!=revision || guard->lspPosition()!=pos) return;
        QString text;
        auto documentation = [](QJsonValue v) { return v.isString() ? v.toString() : v.toObject().value("value").toString(); };
        if (signature) {
            const auto object=value.toObject(); const auto signatures=object.value("signatures").toArray();
            const int active=std::clamp(object.value("activeSignature").toInt(), 0, std::max(0, static_cast<int>(signatures.size())-1));
            if (!signatures.isEmpty()) { const auto item=signatures[active].toObject(); text=item.value("label").toString()+"\n"+documentation(item.value("documentation")); }
        } else {
            const auto contents=value.toObject().value("contents");
            if (contents.isArray()) for (const auto& part:contents.toArray()) text+=documentation(part)+"\n";
            else text=documentation(contents);
        }
        if (text.trimmed().isEmpty()) { statusBar()->showMessage(I18n::text("No language information"), 2000); return; }
        QToolTip::showText(guard->cursorScreenPosition(), "<pre>"+text.left(8000).toHtmlEscaped()+"</pre>", guard);
    };
    if (signature) lsp_->signatureHelp(path,pos.x(),pos.y(),callback); else lsp_->hover(path,pos.x(),pos.y(),callback);
}

void MainWindow::showLspProblems() {
    QDialog dialog(this); dialog.setWindowTitle(I18n::text("Problems")); dialog.resize(950, 500);
    auto* layout=new QVBoxLayout(&dialog); auto* table=new QTableWidget(&dialog);
    table->setColumnCount(4); table->setHorizontalHeaderLabels({I18n::text("File"), I18n::text("Line"), I18n::text("Severity"), I18n::text("Message")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setSelectionBehavior(QAbstractItemView::SelectRows); layout->addWidget(table);
    auto refresh=[&] {
        table->setRowCount(0); auto paths=lspProblems_.keys(); std::sort(paths.begin(),paths.end());
        for (const auto& path:paths) for (const auto& value:lspProblems_.value(path)) {
            const auto item=value.toObject(), start=item.value("range").toObject().value("start").toObject();
            const int row=table->rowCount(); table->insertRow(row);
            auto* file=new QTableWidgetItem(QDir(projectRoots_.value(0)).relativeFilePath(path)); file->setData(Qt::UserRole,path);
            file->setData(Qt::UserRole+1,QPoint(start.value("line").toInt()+1,start.value("character").toInt()+1)); table->setItem(row,0,file);
            table->setItem(row,1,new QTableWidgetItem(QString::number(start.value("line").toInt()+1)));
            const int severity=item.value("severity").toInt(1);
            table->setItem(row,2,new QTableWidgetItem(Codicons::icon(severity==1?"error":severity==2?"warning":"info"),severity==1?I18n::text("Error"):severity==2?I18n::text("Warning"):I18n::text("Information")));
            table->setItem(row,3,new QTableWidgetItem(item.value("message").toString()));
        }
    }; refresh();
    connect(lsp_,&LspManager::diagnostics,&dialog,[&](const QString&,const QJsonArray&) { refresh(); });
    connect(table,&QTableWidget::cellDoubleClicked,&dialog,[&](int row,int) {
        const auto* item=table->item(row,0); if (!item) return;
        const auto point=item->data(Qt::UserRole+1).toPoint(); openFile(item->data(Qt::UserRole).toString(),point.x(),point.y(),0,true); dialog.accept();
    });
    auto* close=new IconDialogs::ButtonBox(QDialogButtonBox::Close,&dialog); layout->addWidget(close);
    connect(close,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); dialog.exec();
}

EditorWidget* MainWindow::createEditor(const QString& path, FileOpenMode mode) {
    auto* editor = new EditorWidget(this, std::make_shared<Document>());
    auto id = vimNextBufferId_++;
    if (!path.isEmpty()) {
        const auto entry = std::find_if(unloadedVimBuffers_.begin(), unloadedVimBuffers_.end(),
                                       [&](const auto& buffer) { return buffer.path == path; });
        if (entry != unloadedVimBuffers_.end()) {
            id = entry->id;
            unloadedVimBuffers_.erase(entry);
        }
    }
    editor->setProperty("vimBufferId", qulonglong(id));
    editor->setProperty("openingPath", path);
    editor->setBusy(true);
    connectEditor(editor, path, true);
    if (mode == FileOpenMode::Preview) filePreviews_.insert(editor);
    const int index = tabs_->addTab(editor, QFileInfo(path).fileName() + I18n::text(" (loading…)"));
    tabs_->setTabToolTip(index, path);
    activateEditor(editor);
    return editor;
}

EditorWidget* MainWindow::createUntitledEditor() {
    auto* editor = createEditor({});
    editor->loadBuffer(PieceTreeBuffer(), {});
    editor->setProperty("openingPath", QVariant{});
    editor->setBusy(false);
    tabs_->setTabText(tabs_->indexOf(editor), I18n::text("Untitled"));
    updateVimAutocmdBuffer();
    return editor;
}

void MainWindow::connectEditor(EditorWidget* editor, const QString& path, bool primary) {
    editor->setVimCommandHandlers(
        [](const QString& id) { return ShortcutManager::instance().hasCommand(id); },
        [this, editor](const QString& id) {
            if (closing_ || activeEditor() != editor || editor->isBusy()) return false;
            if (id.startsWith("debug.") && !debugPanel_->commandAvailable(id.mid(6))) return false;
            QObject* target = ShortcutManager::instance().target(id, this, editor);
            if (auto* action = qobject_cast<QAction*>(target); action && action->isEnabled()) {
                action->trigger();
                return true;
            }
            if (auto* shortcut = qobject_cast<QShortcut*>(target); shortcut && shortcut->isEnabled())
                return QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection);
            return false;
        });
    if (primary) connect(editor->document().get(), &Document::busyChanged, editor, [this, editor](bool busy) {
        if (documentEditor(activeEditor()) == editor) updateEditorPaneActions();
        updateEditorHeaders();
        if (!busy && filePreviews_.contains(editor)) scheduleFilePreviewCleanup();
    });
    else connect(editor, &EditorWidget::modifiedChanged, this, &MainWindow::updateEditorHeaders);
    connect(editor, &EditorWidget::viewFocused, this, [this, editor] { viewFocused(editor); });
    connect(editor, &EditorWidget::vimWorkspaceRequested, this, [this, editor](const QString& key, int count, bool explicitCount) {
        executeVimWorkspaceKey(editor, key, count, explicitCount);
    });
    connect(editor, &EditorWidget::vimBufferNavigateRequested, this, [this, editor](Document* document, int line, int column) {
        if (auto* target = activateEditor(documentEditor(document), true)) {
            target->resetVimMode();
            target->setNavigationPosition(line, column);
            editor->transferVimReplay(target);
        }
    });
    connect(editor,&EditorWidget::vimExRequested,this,[this,editor](const VimEx::Invocation& command,std::function<void(QString)> completed){executeVimEx(editor,command,std::move(completed));});
    connect(editor,&EditorWidget::vimNavigateRequested,this,[this,editor](const QString& file,int line,int column){
        setProperty("vimRestoringLocation",true);openFile(file,line,column);setProperty("vimRestoringLocation",false);
        if(auto* target=activeEditor()){target->resetVimMode();target->setFocus();editor->transferVimReplay(target);}
    });
    connect(editor,&EditorWidget::vimModeChanged,this,&MainWindow::updateVimIndicator);
    editor->setDebugBreakpoints(debugPanel_->breakpoints(path),debugPanel_->disabledBreakpoints(path));
    connect(editor, &EditorWidget::contentChanged, this, [this, editor, observedPath = path]() mutable {
        if (observedPath == editor->filePath()) return;
        observedPath = editor->filePath();
        editor->setDebugBreakpoints(debugPanel_->breakpoints(observedPath), debugPanel_->disabledBreakpoints(observedPath));
        editor->setDebugLine(observedPath == debugPath_ ? debugLine_ : 0);
    });
    connect(editor,&EditorWidget::debugAnchorsMoved,this,[this,editor](const QMap<int,int>& anchors){debugPanel_->remapBreakpoints(editor->filePath(),anchors);});
    connect(editor,&EditorWidget::debugLinesEdited,this,[this,editor](int row,int column,int removed,int inserted){debugPanel_->moveBreakpoints(editor->filePath(),row,column,removed,inserted);});
    editor->setDebugLine(path==debugPath_?debugLine_:0);
    connect(editor,&EditorWidget::breakpointToggled,this,[this,editor](int line) {
        debugPanel_->toggleBreakpoint(editor->filePath(),line);
    });
    connect(editor,&EditorWidget::vimCompletionRequested,this,[this,editor]{requestCompletion(editor,false);});
    connect(editor, &EditorWidget::codeContextMenuRequested, this, [this, editor](const QPoint& global) {
        activateEditor(editor);
        auto* menu = new QMenu(editor);
        menu->setObjectName("editorCodeContextMenu");
        menu->setAttribute(Qt::WA_DeleteOnClose);
        for (const auto& id : {"code.definition", "code.declaration", "code.typeDefinition", "code.references", "code.rename"})
            if (auto* action = findChild<QAction*>(id)) menu->addAction(action);
        menu->popup(global);
    });
    connect(editor, &EditorWidget::hoverRequested, this, [this, editor](int line, int column, const QPoint& global, bool hasDiagnostic) {
        requestEditorHover(editor, line, column, global, hasDiagnostic);
    });
    connect(editor,&EditorWidget::highlightingStatusChanged,this,[this,editor](const QString& message) {
        if(activeEditor()==editor) statusBar()->showMessage(message,8000);
    });
    auto* semanticSync=new QTimer(editor); semanticSync->setObjectName("semanticSync"); semanticSync->setSingleShot(true);
    connect(semanticSync,&QTimer::timeout,this,[this,editor]{requestSemanticHighlighting(editor);});
    connect(editor,&EditorWidget::semanticViewportChanged,semanticSync,[this,editor,semanticSync] {
        if(!lsp_ || !lsp_->semanticTokensRangeAvailable(editor->filePath())) return;
        const auto mode=editor->lspPolicy().mode(editor->byteSize(),true,lsp_->semanticTokensFullAvailable(editor->filePath()),QDateTime::currentMSecsSinceEpoch());
        if(mode!=LspAdaptivePolicy::Mode::Range) return;
        lsp_->cancelSemanticTokens(editor->filePath());
        editor->setProperty("semanticPending",false);
        semanticSync->start(editor->lspPolicy().delay(editor->byteSize()));
    });
    auto* lspSync = new QTimer(editor);
    lspSync->setSingleShot(true);
    lspSync->setInterval(200);
    connect(editor, &EditorWidget::contentChanged, lspSync, [this, editor, lspSync, primary] {
        if (activeEditor() == editor) scheduleOutline();
        if (editor->filePath().isEmpty()) return;
        editor->setDiagnostics({});
        if (!primary) return;
        if(lsp_) lsp_->cancelSemanticTokens(editor->filePath());
        editor->setProperty("semanticPending",false);
        lspSync->start(editor->lspPolicy().syncDelay(editor->byteSize()));
    });
    connect(lspSync, &QTimer::timeout, this, [this, editor] {
        if (!lsp_ || !lsp_->available()) return;
        const QString path = editor->filePath();
        if (!editor->lspEligible()) { lsp_->close(path); editor->clearSemanticHighlighting(); return; }
        if (!lsp_->supports(path) || (!editor->lspAutomaticEligible() && lsp_->documentVersion(path)==0)) return;
        lsp_->change(path, editor->lspText());
        editor->setProperty("semanticSyncedRevision",editor->lspRevision());
        auto* active = activeEditor();
        if (active && active->document() == editor->document()) requestSemanticHighlighting(active);
        else requestSemanticHighlighting(editor);
    });
    auto* completionTimer = new QTimer(editor);
    completionTimer->setSingleShot(true);
    connect(editor, &EditorWidget::contentChanged, completionTimer, [editor, completionTimer] {
        completionTimer->stop();
        if (!editor->applyingCompletion() && QSettings().value("lsp/autoCompletion", true).toBool())
            completionTimer->start(std::clamp(QSettings().value("lsp/completionDelayMs", 150).toInt(), 50, 2000));
    });
    connect(completionTimer, &QTimer::timeout, this, [this, editor] {
        if (activeEditor() == editor) requestCompletion(editor, true);
    });
    connect(editor, &EditorWidget::contentChanged, this, [this, editor] {
        if (gitDiff_ && gitDiff_->nativeEditor() == editor) gitDiff_->documentChanged();
    });
    connect(editor, &EditorWidget::diffRowsInserted, this, [this, editor](int row, int count) {
        if (gitDiff_ && gitDiff_->nativeEditor() == editor) gitDiff_->insertPadding(row, count);
    });
    if (!primary) return;
    editor->setProperty("recoveryId",QUuid::createUuid().toString(QUuid::WithoutBraces));
    connect(editor,&EditorWidget::contentChanged,this,[this,editor] {
        if(documentChangeTimer_) documentChangeTimer_->start();
        if(!QSettings().value("editor/recoveryEnabled",true).toBool())return;
        if(recoveryTimer_)recoveryTimer_->start();
        if(recoveryDeadline_ && !recoveryDeadline_->isActive())recoveryDeadline_->start();
    });
    const auto document = editor->document();
    connect(editor, &QObject::destroyed, this, [this, document, editor] {
        filePreviews_.remove(editor);
        recoveredRevisions_.remove(editor);
        externalFileStates_.remove(editor);
        externallyReloading_.remove(editor);
        if (lsp_) lsp_->close(document->filePath());
    });

    connect(editor, &EditorWidget::modifiedChanged, this, [this, editor] {
        if (editor->isModified() && filePreviews_.contains(editor) &&
            QSettings().value("editor/pinPreviewOnEdit", true).toBool()) pinFilePreview(editor);
        const int index = tabs_->indexOf(editor);
        if (index >= 0) {
            const QString name = editor->filePath().isEmpty()
                ? I18n::text("Untitled (deleted)")
                : QFileInfo(editor->filePath()).fileName();
            tabs_->setTabText(index, name + (editor->isModified() ? " •" : ""));
        }
        updateWindowTitle();
    });
}

void MainWindow::navigateSymbol(const QString& method) {
    auto* editor = activeEditor();
    if (!ensureManualLsp(editor, [this, method] { navigateSymbol(method); })) return;
    const auto path = editor->filePath(); const auto revision = editor->lspRevision();
    const auto position = editor->lspPosition(); QPointer<EditorWidget> guard(editor);
    lsp_->change(path, editor->lspText());
    statusBar()->showMessage(I18n::text("Searching for symbol locations…"), 10000);
    auto callback = [this, guard, path, revision, position, method](QJsonValue result) {
        if (!guard || activeEditor()!=guard || guard->filePath()!=path || guard->lspRevision()!=revision || guard->lspPosition()!=position) return;
        const auto values = result.isArray() ? result.toArray() : QJsonArray{result};
        struct Location { QString path; QPoint position; };
        QVector<Location> locations;
        for (const auto& value : values) {
            const auto object = value.toObject();
            const auto target = QUrl(object.value("targetUri").toString(object.value("uri").toString())).toLocalFile();
            const auto range = object.contains("targetUri") ? object.value("targetSelectionRange").toObject() : object.value("range").toObject();
            const auto start = range.value("start").toObject();
            if (target.isEmpty() || !start.value("line").isDouble() || !start.value("character").isDouble()) continue;
            const QPoint point(start.value("line").toInt()+1, start.value("character").toInt()+1);
            if (point.x()<1 || point.y()<1) continue;
            const auto duplicate = std::any_of(locations.cbegin(), locations.cend(), [&](const Location& item) { return item.path==target && item.position==point; });
            if (!duplicate) locations.append({target, point});
        }
        if (locations.isEmpty()) { statusBar()->showMessage(I18n::text("No symbol locations found. The server may not support this command."), 5000); return; }
        statusBar()->clearMessage();
        auto open = [this](const Location& location) {
            recordNavigationLocation();
            openFile(location.path, location.position.x(), location.position.y(), 0, true);
        };
        if (method!="references" && locations.size()==1) { open(locations.first()); return; }
        auto* dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName("symbolLocations");
        dialog->setWindowTitle(method=="references" ? I18n::text("Find All References") : I18n::text("Symbol locations"));
        dialog->resize(800, 420);
        auto* layout = new QVBoxLayout(dialog); auto* table = new QTableWidget(dialog);
        table->setColumnCount(3); table->setHorizontalHeaderLabels({I18n::text("File"), I18n::text("Line"), I18n::text("Column")});
        table->setRowCount(locations.size()); table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection); table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int i=0; i<locations.size(); ++i) {
            const auto& item=locations[i];
            table->setItem(i,0,new QTableWidgetItem(item.path));
            table->setItem(i,1,new QTableWidgetItem(QString::number(item.position.x())));
            table->setItem(i,2,new QTableWidgetItem(QString::number(item.position.y())));
        }
        layout->addWidget(table); table->selectRow(0);
        auto* buttons = new IconDialogs::ButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Close, dialog);
        buttons->button(QDialogButtonBox::Open)->setIcon(Codicons::icon("go-to-file"));
        buttons->button(QDialogButtonBox::Close)->setIcon(Codicons::icon("close"));
        layout->addWidget(buttons);
        auto activate = [table, locations, open] { const int row=table->currentRow(); if (row>=0 && row<locations.size()) open(locations[row]); };
        connect(table, &QTableWidget::cellDoubleClicked, dialog, [activate](int,int) { activate(); });
        connect(buttons, &QDialogButtonBox::accepted, dialog, activate);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        dialog->show();
    };
    if (method=="definition") lsp_->definition(path,position.x(),position.y(),callback);
    else if (method=="declaration") lsp_->declaration(path,position.x(),position.y(),callback);
    else if (method=="typeDefinition") lsp_->typeDefinition(path,position.x(),position.y(),callback);
    else lsp_->references(path,position.x(),position.y(),callback);
}

void MainWindow::renameSymbol() {
    if (renameInProgress_) return;
    auto* editor=activeEditor();
    if (!editor || editor->isBusy() || editor->cursorCount()!=1) return;
    if(!ensureManualLsp(editor,[this]{renameSymbol();})) return;
    if (!lsp_->canRename(editor->filePath())) { statusBar()->showMessage(I18n::text("The language server is not ready or does not support rename."),6000); return; }
    renameInProgress_=true;
    QPointer<MainWindow> owner(this);
    const auto operation=std::shared_ptr<int>(new int(0),[owner](int* value) {
        delete value;
        if (owner) {
            owner->renameInProgress_ = false;
            owner->scheduleFilePreviewCleanup();
        }
    });
    for (int i=0;i<tabs_->count();++i) if (auto* item=qobject_cast<EditorWidget*>(tabs_->widget(i)))
        if (!item->isBusy() && item->lspEligible() && lsp_->supports(item->filePath())) lsp_->change(item->filePath(),item->lspText());
    const auto path=editor->filePath(); const auto revision=editor->lspRevision(); const auto position=editor->lspPosition();
    const QByteArray source=QByteArray::fromStdString(editor->snapshotBuffer().text());
    QPointer<EditorWidget> guard(editor); editor->hideCompletions();
    statusBar()->showMessage(I18n::text("Checking whether the symbol can be renamed…"),10000);
    lsp_->prepareRename(path,position.x(),position.y(),[this,guard,path,revision,position,source,operation](QJsonValue prepared) {
        if (!guard || activeEditor()!=guard || guard->filePath()!=path || guard->lspRevision()!=revision || guard->lspPosition()!=position) return;
        if (!prepared.isObject()) { IconDialogs::MessageBox::information(this,I18n::text("Rename symbol"),I18n::text("The language server cannot rename this symbol.")); return; }
        QString oldName,error; const auto object=prepared.toObject();
        if (object.value("defaultBehavior").toBool()) {
            const auto lines=QString::fromUtf8(source).split('\n');
            if (position.x()>=lines.size()) return;
            const auto& line=lines[position.x()]; int a=position.y(),b=a;
            auto identifier=[](QChar ch) { return ch.isLetterOrNumber() || ch=='_' || ch.isMark(); };
            while (a>0 && identifier(line[a-1])) --a;
            while (b<line.size() && identifier(line[b])) ++b;
            oldName=line.mid(a,b-a);
        } else {
            const auto range=object.contains("range")?object.value("range").toObject():object;
            if (!WorkspaceEdits::rangeText(source,range,&oldName,&error)) { IconDialogs::MessageBox::warning(this,I18n::text("Rename symbol"),error); return; }
        }
        if (oldName.isEmpty()) return;
        bool accepted=false;
        const auto newName=IconDialogs::InputDialog::getText(this,I18n::text("Rename symbol"),I18n::text("New name:"),QLineEdit::Normal,oldName,&accepted).trimmed();
        if (!accepted || newName.isEmpty() || newName==oldName) { statusBar()->clearMessage(); return; }
        if (!guard || guard->filePath()!=path || guard->lspRevision()!=revision || !lsp_->canRename(path)) return;
        struct Snapshot { QPointer<EditorWidget> editor; QByteArray text; quint64 revision{}; int version{}; };
        QHash<QString,Snapshot> snapshots;
        for (int i=0;i<tabs_->count();++i) if (auto* item=qobject_cast<EditorWidget*>(tabs_->widget(i))) {
            if (item->filePath().isEmpty()) continue;
            const auto key=WorkspaceEdits::pathKey(item->filePath());
            if (snapshots.contains(key)) { IconDialogs::MessageBox::warning(this,I18n::text("Rename symbol"),I18n::text("The same file is open through multiple paths. Close the duplicate tab and retry.")); return; }
            if (item->isBusy()) { IconDialogs::MessageBox::information(this,I18n::text("Rename symbol"),I18n::text("Wait for file loading/saving to finish and retry.")); return; }
            if (item->lspEligible() && lsp_->supports(item->filePath())) lsp_->change(item->filePath(),item->lspText());
            snapshots.insert(key,{item,QByteArray::fromStdString(item->snapshotBuffer().text()),item->lspRevision(),lsp_->documentVersionForServer(path,item->filePath())});
        }
        const auto requestedAt=QDateTime::currentDateTimeUtc();
        statusBar()->showMessage(I18n::text("Finding symbol references for rename…"),30000);
        lsp_->renameSymbol(path,position.x(),position.y(),newName,[this,guard,path,revision,snapshots,requestedAt,oldName,newName,operation](QJsonValue result) {
            if (!guard || guard->filePath()!=path || guard->lspRevision()!=revision) return;
            if (!result.isObject()) {
                // Keep a more specific protocol error if LspClient just displayed one.
                if (statusBar()->currentMessage().startsWith(I18n::text("Finding symbol references"))) statusBar()->showMessage(I18n::text("No rename changes were returned."),4000);
                return;
            }
            QVector<WorkspaceDocumentEdit> documents; QString error;
            if (!WorkspaceEdits::decode(result.toObject(),&documents,&error)) { IconDialogs::MessageBox::warning(this,I18n::text("Cannot rename"),error); return; }
            struct PlannedFile { QString path; QByteArray before; std::vector<EditChange> changes; QJsonArray edits; QPointer<EditorWidget> editor; quint64 revision{}; };
            QVector<PlannedFile> plan;
            auto fail=[this](const QString& message) { IconDialogs::MessageBox::warning(this,I18n::text("Rename not applied"),message+I18n::text("\nNo text changes were applied.")); };
            for (const auto& document:documents) {
                if (document.edits.isEmpty()) continue;
                if (!QFileInfo(document.path).isWritable()) { fail(I18n::text("A target file is read-only or unavailable:\n")+document.path); return; }
                PlannedFile file; file.path=document.path; file.edits=document.edits;
                if (snapshots.contains(document.path)) {
                    const auto snapshot=snapshots.value(document.path);
                    if (!snapshot.editor || snapshot.editor->isBusy() || snapshot.editor->lspRevision()!=snapshot.revision ||
                        WorkspaceEdits::pathKey(snapshot.editor->filePath())!=document.path) { fail(I18n::text("An open file changed while rename was being calculated. Retry rename.")); return; }
                    if (document.version && *document.version!=snapshot.version) { fail(I18n::text("The server returned edits for an older document version.")); return; }
                    file.editor=snapshot.editor; file.before=snapshot.text; file.revision=snapshot.revision;
                } else {
                    for (int i=0;i<tabs_->count();++i) if (auto* item=qobject_cast<EditorWidget*>(tabs_->widget(i)))
                        if (!item->filePath().isEmpty() && WorkspaceEdits::pathKey(item->filePath())==document.path) { fail(I18n::text("A target file was opened while rename was being calculated. Retry rename.")); return; }
                    if (document.version) { fail(I18n::text("The server returned a versioned edit for a document that was not open.")); return; }
                    const QFileInfo info(document.path);
                    if (!info.isFile() || !info.isWritable() || info.lastModified().toUTC()>requestedAt) { fail(I18n::text("A target file is unavailable, read-only or changed during the request:\n")+document.path); return; }
                    QFile input(document.path);
                    if (!input.open(QIODevice::ReadOnly)) { fail(input.errorString()+"\n"+document.path); return; }
                    file.before=input.readAll();
                    if (input.error()!=QFileDevice::NoError) { fail(input.errorString()); return; }
                }
                if (!WorkspaceEdits::convert(file.before,document.edits,&file.changes,&error)) { fail(document.path+"\n"+error); return; }
                if (!file.editor) for (const auto& change:file.changes)
                    if (QString::fromUtf8(change.removed.data(),change.removed.size())!=oldName) { fail(I18n::text("An unopened file no longer matches the symbol at the server's edit ranges:\n")+file.path); return; }
                plan.append(std::move(file));
            }
            if (plan.isEmpty()) { statusBar()->showMessage(I18n::text("No symbol references to rename."),4000); return; }
            QDialog preview(this); preview.setWindowTitle(I18n::text("Rename ")+oldName+" → "+newName); preview.resize(850,550);
            auto* layout=new QVBoxLayout(&preview);
            auto* note=new QLabel(I18n::text("Review all changes. Affected files will remain open with unsaved changes. Save them when ready; Ctrl+Z undoes rename separately in each file."),&preview);
            note->setWordWrap(true); layout->addWidget(note);
            auto* tree=new QTreeWidget(&preview); tree->setHeaderLabels({I18n::text("File / line"),I18n::text("Before"),I18n::text("After")}); layout->addWidget(tree);
            int edits=0;
            for (const auto& file:plan) {
                auto* parent=new QTreeWidgetItem(tree,{QDir(projectRoots_.value(0)).relativeFilePath(file.path)});
                parent->setIcon(0, FileIcons::icon(file.path));
                parent->setToolTip(0,file.path);
                for (int i=0;i<file.edits.size();++i) {
                    const auto edit=file.edits[i].toObject(); const int line=edit.value("range").toObject().value("start").toObject().value("line").toInt();
                    QString before; WorkspaceEdits::rangeText(file.before,edit.value("range").toObject(),&before,&error);
                    new QTreeWidgetItem(parent,{QString::number(line+1),before,edit.value("newText").toString()}); ++edits;
                }
                parent->setExpanded(true);
            }
            tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
            auto* buttons=new IconDialogs::ButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel,&preview); layout->addWidget(buttons);
            connect(buttons->button(QDialogButtonBox::Apply),&QPushButton::clicked,&preview,&QDialog::accept);
            connect(buttons,&QDialogButtonBox::rejected,&preview,&QDialog::reject);
            if (preview.exec()!=QDialog::Accepted) { statusBar()->clearMessage(); return; }
            // Revalidate every file after the modal preview, before editing any buffer.
            for (const auto& file:plan) {
                if (file.editor) {
                    if (file.editor->lspRevision()!=file.revision || WorkspaceEdits::pathKey(file.editor->filePath())!=file.path || !file.editor->validateWorkspaceEdits(file.before,file.changes)) { fail(I18n::text("An open file changed during preview. Retry rename.")); return; }
                } else {
                    for (int i=0;i<tabs_->count();++i) if (auto* item=qobject_cast<EditorWidget*>(tabs_->widget(i)))
                        if (!item->filePath().isEmpty() && WorkspaceEdits::pathKey(item->filePath())==file.path) { fail(I18n::text("A target file was opened during preview. Retry rename.")); return; }
                    QFile input(file.path);
                    if (!QFileInfo(file.path).isWritable() || !input.open(QIODevice::ReadOnly) || input.readAll()!=file.before || input.error()!=QFileDevice::NoError) { fail(I18n::text("A file changed on disk during preview:\n")+file.path); return; }
                }
            }
            for (auto& file:plan) if (!file.editor) {
                auto* target=createEditor(file.path); target->loadBuffer(PieceTreeBuffer(file.before.toStdString()),file.path); target->setBusy(false); file.editor=target;
            }
            // No event loop is entered while applying this prevalidated transaction.
            for (auto& file:plan) {
                file.editor->applyWorkspaceEdits(std::move(file.changes));
                if (file.editor->lspEligible() && lsp_->supports(file.editor->filePath())) lsp_->change(file.editor->filePath(),file.editor->lspText());
            }
            if (guard) activateEditor(guard, true);
            statusBar()->showMessage(QString(I18n::text("Renamed %1 references in %2 files. Changes are not saved yet.")).arg(edits).arg(plan.size()),8000);
        });
    });
}

void MainWindow::applyVimSettings() {
    const bool enabled=QSettings().value("editor/vimMode",false).toBool();
    for(auto* widget:QApplication::allWidgets()) {
        if(auto* editor=qobject_cast<EditorWidget*>(widget))editor->setVimEnabled(enabled);
        if(auto* window=qobject_cast<MainWindow*>(widget)) {
            window->explorer_->setVimEnabled(enabled);
            if(auto* action=window->findChild<QAction*>("settings.vimMode")){const QSignalBlocker blocker(action);action->setChecked(enabled);}
            window->updateVimIndicator();
        }
    }
}
void MainWindow::updateVimIndicator() {
    if(!vimStatus_ || !tabs_)return;
    auto* editor=activeEditor();
    for(auto* widget=QApplication::focusWidget();widget;widget=widget->parentWidget())
        if(auto* focused=qobject_cast<EditorWidget*>(widget);focused && isAncestorOf(focused)){editor=focused;break;}
    const bool visible=editor && editor->vimEnabled();vimStatus_->setVisible(visible);
    if(visible){vimStatus_->setText(editor->vimModeName());vimStatus_->setEnabled(editor->hasFocus() || editor->vimCommandLineActive());}
}

void MainWindow::initializeRecovery() {
    try { recovery_=std::make_shared<RecoveryStore>(); }
    catch(const std::exception& error) {
        auto* warning=new QLabel(I18n::text("Recovery is unavailable"),this);
        warning->setToolTip(QString::fromUtf8(error.what()));statusBar()->addPermanentWidget(warning);return;
    }
    recoveryTimer_=new QTimer(this);recoveryTimer_->setObjectName("recoveryDebounce");recoveryTimer_->setSingleShot(true);recoveryTimer_->setInterval(2000);
    recoveryDeadline_=new QTimer(this);recoveryDeadline_->setObjectName("recoveryDeadline");recoveryDeadline_->setSingleShot(true);recoveryDeadline_->setInterval(15000);
    connect(recoveryTimer_,&QTimer::timeout,this,&MainWindow::checkpointRecovery);
    connect(recoveryDeadline_,&QTimer::timeout,this,&MainWindow::checkpointRecovery);
    auto* poll=new QTimer(this);poll->setInterval(10000);
    connect(poll,&QTimer::timeout,this,&MainWindow::checkpointRecovery);poll->start();
}

void MainWindow::applyRecoverySettings() {
    if(!QSettings().value("editor/recoveryEnabled",true).toBool()) {
        if(recoveryTimer_)recoveryTimer_->stop();
        if(recoveryDeadline_)recoveryDeadline_->stop();
        if(auto* warning=findChild<QLabel*>("recoveryWarning"))warning->hide();
    } else if(recoveryTimer_)recoveryTimer_->start();
}

QJsonObject MainWindow::sessionState(bool clean) const {
    QJsonArray documents;
    const bool backupsEnabled=QSettings().value("editor/recoveryEnabled",true).toBool();
    QString active;
    for(int i=0;i<tabs_->count();++i)if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i))) {
        if (editor->isBusy()) continue;
        const auto path=editor->filePath();
        if(clean && path.isEmpty())continue;
        const auto id=editor->property("recoveryId").toString();
        const bool hasCopy=recovery_ && QFileInfo::exists(recovery_->directory()+"/"+id+".json");
        if (filePreviews_.contains(editor) && (clean || !editor->isModified() || (!backupsEnabled && !hasCopy))) continue;
        if(!clean && !backupsEnabled && path.isEmpty() && !hasCopy)continue;
        const auto* view = activeEditor();
        if (!view || view->document() != editor->document()) view = editor;
        const auto position=view->navigationPosition();
        QJsonObject entry{{"id",id},{"path",path},{"line",position.x()},{"column",position.y()},
            {"vertical",view->verticalScrollBar()->value()},{"horizontal",view->horizontalScrollBar()->value()},
            {"modified",!clean && editor->isModified() && (backupsEnabled || hasCopy)}};
        documents.append(entry);
        if(view==activeEditor())active=id;
    }
    return {{"documents",documents},{"roots",QJsonArray::fromStringList(explicitProject_?projectRoots_:QStringList{})},
            {"active",active},{"clean",clean},{"time",QString::number(QDateTime::currentMSecsSinceEpoch())}};
}

void MainWindow::checkpointRecovery() {
    if(!recovery_ || recoveryStopped_ || closing_)return;
    if(recoveryWriting_) { if(recoveryDeadline_ && !recoveryDeadline_->isActive())recoveryDeadline_->start();return; }
    // Never publish a session omitting documents which are temporarily busy.
    for(int i=0;i<tabs_->count();++i)
        if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i));editor && editor->isBusy())return;
    recoveryTimer_->stop();recoveryDeadline_->stop();
    const bool backupsEnabled=QSettings().value("editor/recoveryEnabled",true).toBool();
    struct Job {QPointer<EditorWidget> editor;quint64 revision;QString id;std::shared_ptr<PieceTreeBuffer> snapshot;QJsonObject metadata;};
    auto jobs=std::make_shared<std::vector<Job>>();
    for(int i=0;i<tabs_->count();++i)if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i))) {
        if(!backupsEnabled || !editor->isModified() || (recoveredRevisions_.contains(editor) && recoveredRevisions_.value(editor)==editor->lspRevision()))continue;
        // Capture between UI edits; clone piece metadata and bounded resident arenas,
        // sharing disk storage. Disk reads and all recovery writes run in the worker.
        auto snapshot=std::make_shared<PieceTreeBuffer>(editor->document()->buffer().snapshot(true));
        const auto position=editor->navigationPosition();
        jobs->push_back({editor,editor->lspRevision(),editor->property("recoveryId").toString(),snapshot,
            {{"path",editor->filePath()},{"diskState",QString::fromLatin1(editor->document()->diskState)},
             {"line",position.x()},{"column",position.y()}}});
    }
    recoveryWriting_=true;
    const auto store=recovery_;
    auto* watcher=new QFutureWatcher<QString>(this);
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,jobs] {
        auto error=watcher->result();watcher->deleteLater();recoveryWriting_=false;
        if(recoveryStopped_)return;
        scheduleFilePreviewCleanup();
        if(error.isEmpty()) {
            for(const auto& job:*jobs)if(job.editor)recoveredRevisions_[job.editor]=job.revision;
            auto state=sessionState();
            // If an edit arrived while writing, keep the document in the session:
            // its manifest still points to the last successfully recorded text.
            // A brand-new dirty document without a checkpoint must wait for the next pass.
            QJsonArray entries;
            for(const auto value:state["documents"].toArray()) {
                const auto entry=value.toObject();
                if(entry["modified"].toBool() && !QFileInfo::exists(recovery_->directory()+"/"+entry["id"].toString()+".json"))continue;
                entries.append(entry);
            }
            state["documents"]=entries;
            bool busy=false;
            for(int i=0;i<tabs_->count();++i)if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i));editor && editor->isBusy())busy=true;
            if(!closing_ && !busy) {
                error=recovery_->writeSession(state);
                if(error.isEmpty())for(int i=0;i<tabs_->count();++i)
                    if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i));editor && !editor->isModified()) {
                        recovery_->forgetDocument(editor->property("recoveryId").toString());
                        recoveredRevisions_.remove(editor);
                    }
            }
        }
        auto* warning=findChild<QLabel*>("recoveryWarning");
        const bool backupsEnabled=QSettings().value("editor/recoveryEnabled",true).toBool();
        if(!error.isEmpty() && backupsEnabled) {
            if(!warning){warning=new QLabel(this);warning->setObjectName("recoveryWarning");statusBar()->addPermanentWidget(warning);}
            warning->setText(I18n::text("Recovery copy is out of date"));warning->setToolTip(error);warning->show();
            recoveryDeadline_->start();
        } else {
            if(warning)warning->hide();
            bool complete=true;
            for(int i=0;i<tabs_->count();++i)if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i))) {
                if(editor->isBusy() || (editor->isModified() && (!recoveredRevisions_.contains(editor) || recoveredRevisions_.value(editor)!=editor->lspRevision())))complete=false;
            }
            if(complete && !closing_ && !recoveryMigrationFailed_) {
                for(const auto& directory:recoveredSourceDirectories_)QDir(directory).removeRecursively();
                recoveredSourceDirectories_.clear();restorationLocks_.clear();
            }
            if(!complete && backupsEnabled)recoveryTimer_->start();
        }
    });
    watcher->setFuture(QtConcurrent::run([store,jobs] {
        try {
            for(const auto& job:*jobs) {
                const auto error=store->writeDocument(job.id,*job.snapshot,job.metadata);
                if(!error.isEmpty())return error;
            }
        } catch(const std::exception& error){return QString::fromUtf8(error.what());}
        return QString();
    }));
}

void MainWindow::restorePreviousSession(bool restoreClean) {
    if(!recovery_)return;
    struct Candidate {QString directory;QJsonObject state;std::shared_ptr<QLockFile> lock;};
    std::vector<Candidate> candidates;
    for(const auto& name:QDir(RecoveryStore::rootDirectory()).entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        const auto directory=RecoveryStore::rootDirectory()+"/"+name;
        if(directory==recovery_->directory())continue;
        auto lock=std::make_shared<QLockFile>(directory+"/lock");lock->setStaleLockTime(0);
        if(!lock->tryLock())continue;
        auto state=RecoveryStore::readObject(directory+"/session.json");
        if(state["format"].toInt()!=1 || !state["documents"].isArray()) {
            // A crash may happen after the first document commit but before the
            // session manifest is published. Recover these complete checkpoints too.
            QJsonArray documents;
            for(const auto& file:QDir(directory).entryList({"*.json"},QDir::Files)) {
                if(file=="session.json")continue;
                const auto id=QFileInfo(file).completeBaseName();
                const auto metadata=RecoveryStore::readObject(directory+"/"+file);
                if(!RecoveryStore::validId(id) || metadata["format"].toInt()!=1 || !metadata["pieces"].isArray())continue;
                documents.append(QJsonObject{{"id",id},{"path",metadata["path"]},{"modified",true},
                    {"line",metadata["line"]},{"column",metadata["column"]}});
            }
            if(!documents.isEmpty())state={{"format",1},{"clean",false},{"documents",documents}};
        }
        if(state["format"].toInt()==1)candidates.push_back({directory,state,lock});
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.state["time"].toString().toLongLong()>b.state["time"].toString().toLongLong();});
    bool restoredClean=false;
    for(const auto& candidate:candidates) {
        const auto documents=candidate.state["documents"].toArray();
        bool dirty=false;for(const auto entry:documents)dirty|=entry.toObject()["modified"].toBool();
        const bool clean=candidate.state["clean"].toBool();
        if(clean && (!restoreClean || restoredClean))continue;
        if(dirty) {
            IconDialogs::MessageBox prompt(QMessageBox::Warning,I18n::text("Recover unsaved documents"),
                I18n::text("Unsaved changes from a previous session were found. Restore them now?"),QMessageBox::NoButton,this);
            auto* restore=prompt.addButton(I18n::text("Restore"),QMessageBox::AcceptRole); restore->setIcon(Codicons::icon("history"));
            prompt.addButton(I18n::text("Later"),QMessageBox::RejectRole)->setIcon(Codicons::icon("clock"));prompt.setDefaultButton(restore);prompt.exec();
            if(prompt.clickedButton()!=restore)continue;
        }
        if(clean)restoredClean=true;
        if(explicitProject_==false) {
            QStringList roots;for(const auto root:candidate.state["roots"].toArray())if(QFileInfo(root.toString()).isDir())roots.append(root.toString());
            if(!roots.isEmpty())setProjectRoots(roots);
        }
        recoveredSourceDirectories_.insert(candidate.directory);restorationLocks_.push_back(candidate.lock);
        for(const auto value:documents) {
            const auto entry=value.toObject();const auto path=entry["path"].toString();
            const auto id=entry["id"].toString();
            if(!entry["modified"].toBool()) {
                if(!QFileInfo::exists(path)){recoveryMigrationFailed_=true;continue;}
                openFile(path,entry["line"].toInt(1),entry["column"].toInt(1));
                auto* editor=activeEditor();
                if(editor){editor->setProperty("recoveryOrigin",candidate.directory);editor->setProperty("recoveryOriginId",id);editor->setProperty("restoreVertical",entry["vertical"].toInt());editor->setProperty("restoreHorizontal",entry["horizontal"].toInt());}
                if(id==candidate.state["active"].toString() && editor)editor->setProperty("restoreActive",true);
                continue;
            }
            // Recover to an independent buffer. If another tab already owns the path,
            // detach this one to avoid having two conflicting writable documents.
            QString targetPath=path;
            for(int i=0;i<tabs_->count();++i)if(auto* existing=qobject_cast<EditorWidget*>(tabs_->widget(i));existing && existing->filePath()==path)targetPath.clear();
            auto* editor=createEditor(targetPath);QPointer<EditorWidget> guard(editor);
            editor->setProperty("recoveryOrigin",candidate.directory);editor->setProperty("recoveryOriginId",id);
            auto* watcher=new QFutureWatcher<LoadedFile>(this);
            const auto directory=candidate.directory;
            connect(watcher,&QFutureWatcher<LoadedFile>::finished,this,[this,watcher,guard,targetPath,directory,id,entry,candidate] {
                const auto result=watcher->result();watcher->deleteLater();
                if(!guard)return;
                if(!result.buffer) {
                    recoveryMigrationFailed_=true;
                    tabs_->removeTab(tabs_->indexOf(guard));guard->deleteLater();
                    IconDialogs::MessageBox::warning(this,I18n::text("Recovery failed"),result.error);return;
                }
                guard->restoreBuffer(std::move(*result.buffer),targetPath);
                guard->document()->diskState=result.diskState;
                guard->setBusy(false);
                guard->setNavigationPosition(entry["line"].toInt(1),entry["column"].toInt(1));
                guard->verticalScrollBar()->setValue(entry["vertical"].toInt());
                guard->horizontalScrollBar()->setValue(entry["horizontal"].toInt());
                if(id==candidate.state["active"].toString())activateEditor(guard);
                recoveryTimer_->start();
            });
            watcher->setFuture(QtConcurrent::run([directory,id] {
                LoadedFile result;result.buffer=std::make_shared<PieceTreeBuffer>();QJsonObject metadata;
                try {
                    if(!RecoveryStore::restoreDocument(directory,id,*result.buffer,metadata,result.error))result.buffer.reset();
                    result.diskState=metadata["diskState"].toString().toLatin1();
                } catch(const std::exception& error){result.buffer.reset();result.error=QString::fromUtf8(error.what());}
                return result;
            }));
        }
    }
    recoveryTimer_->start();
}

void MainWindow::focusEditor() {
    if (auto* editor = activeEditor()) editor->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::focusProjectTree() {
    sidebarStack_->setCurrentWidget(explorerPage_);
    if (sidebar_->isHidden()) setSidebarVisible(true);
    explorer_->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::focusTerminal() {
    if (terminalPanel_ && terminalPanel_->isVisible()) terminalPanel_->focusCurrentTab();
    else showTerminal();
}

void MainWindow::showTerminal() {
    terminalPreviousFocus_ = QApplication::focusWidget();
    const bool firstOpen = !terminalPanel_;
    ensureTerminal();
    QString directory = projectRoots_.value(0);
    if (!explicitProject_) {
        if (auto* editor = activeEditor(); editor && !editor->filePath().isEmpty())
            directory = QFileInfo(editor->filePath()).absolutePath();
    }
    terminalPanel_->open(directory.isEmpty() ? QDir::homePath() : directory);
    if (firstOpen) editorArea_->resizeDocks({terminalPanel_}, {qMax(180, editorArea_->height() / 3)}, Qt::Vertical);
}

void MainWindow::ensureTerminal() {
    if(terminalPanel_) return;
    terminalPanel_=new TerminalPanel(editorArea_);
    terminalPanel_->setDirectory(projectRoots_.value(0));
    editorArea_->addDockWidget(Qt::BottomDockWidgetArea,terminalPanel_);
    terminalPanel_->setDebugConsole(debugPanel_->console());
    connect(terminalPanel_,&TerminalPanel::debugTerminalReply,debugPanel_,&DebugPanel::terminalReply);
    connect(terminalPanel_,&QDockWidget::visibilityChanged,this,[this](bool visible){
        if(!visible) {
            if(terminalPreviousFocus_ && terminalPreviousFocus_->isVisible()) terminalPreviousFocus_->setFocus();
            else if(activeEditor()) activeEditor()->setFocus();
        }
    });
    editorArea_->resizeDocks({terminalPanel_},{qMax(180,editorArea_->height()/3)},Qt::Vertical);
}

void MainWindow::scheduleOutline() {
    if(!outline_ || !outlineTimer_) return;
    ++outlineGeneration_; outlineTimer_->stop();
    if(!outline_->isVisible()) return;
    auto* editor=activeEditor();
    const bool changed=outlineEditor_!=editor; outlineEditor_=editor;
    if(!editor) { outline_->showMessage(I18n::text("Open a file to see its outline.")); return; }
    outline_->pending(changed);
    outlineTimer_->start();
}

void MainWindow::refreshOutline() {
    if(!outline_ || !outline_->isVisible() || !lsp_) return;
    auto* editor=activeEditor();
    if(!editor) { outline_->showMessage(I18n::text("Open a file to see its outline.")); return; }
    if(editor->isBusy()) { outlineTimer_->start(); return; }
    const auto path=editor->filePath();
    if(!editor->lspAutomaticEligible()) {
        outline_->showMessage(I18n::text("Outline is unavailable for this large file.")); return;
    }
    if(!lsp_->supports(path) || !lsp_->available()) {
        outline_->showMessage(I18n::text("Enable a language server for this file to see its outline.")); return;
    }
    if(!lsp_->ready(path)) {
        lsp_->open(path,editor->lspText());
        outline_->showMessage(I18n::text("Waiting for the language server…")); return;
    }
    if(!lsp_->documentSymbolsAvailable(path)) {
        outline_->showMessage(I18n::text("This language server does not provide an outline.")); return;
    }
    lsp_->change(path,editor->lspText());
    const auto generation=outlineGeneration_,revision=editor->lspRevision();
    QPointer<EditorWidget> guard(editor); QPointer<MainWindow> owner(this);
    lsp_->documentSymbols(path,[owner,guard,path,generation,revision](QJsonValue result) {
        if(!owner || !owner->tabs_ || !guard || generation!=owner->outlineGeneration_ || guard->lspRevision()!=revision
            || guard->filePath()!=path || owner->activeEditor()!=guard || !owner->outline_->isVisible()) return;
        if(result.isUndefined() || (!result.isNull() && !result.isArray())) {
            owner->outline_->showMessage(I18n::text("Could not load outline.")); return;
        }
        owner->outline_->setSymbols(result.toArray(),path);
    });
}
