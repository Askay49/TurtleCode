#include "app/IconDialogs.hpp"
#include "document/RecoveryStore.hpp"
#include "document/FileState.hpp"
#include <QCloseEvent>
#include <QUuid>
#include <QLockFile>
#include <QScrollBar>
#include <QScrollArea>
#include <QScreen>
#include "app/FileIcons.hpp"
#include "app/Codicons.hpp"
#include "app/I18n.hpp"
#include "FontSettings.hpp"
#include "terminal/TerminalPanel.hpp"
#include "terminal/TerminalWidget.hpp"
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include "ShortcutManager.hpp"
#include "ThemeManager.hpp"
#include "WorkspaceBackground.hpp"
#include <QPainter>
#include <QImageReader>
#include "MainWindow.hpp"

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QTreeWidget>
#include "language/WorkspaceEdits.hpp"
#include "app/SettingsSpinBox.hpp"
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
#include <QScreen>
#include <QPushButton>
#include <QHBoxLayout>
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
#include "editor/EditorTabs.hpp"
#include "project/ProjectExplorer.hpp"
#include "search/SearchPanel.hpp"
#include "git/GitPanel.hpp"
#include "debug/DebugPanel.hpp"
#include "git/GitDiffView.hpp"
#include "language/LanguageRegistry.hpp"
#include "language/GrammarManager.hpp"
#include "language/LspManager.hpp"
#include "language/LspSettingsDialog.hpp"

namespace {

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

int fuzzyMatchScore(const QString& candidate, const QString& query) {
    const QString text = candidate.toLower();
    const QString needle = query.toLower();
    int score = 0;
    int previous = -2;
    int position = 0;
    for (const QChar character : needle) {
        const int found = text.indexOf(character, position);
        if (found < 0) return -1;
        score += 10;
        if (found == previous + 1) score += 12;
        if (found == 0 || text.at(found - 1) == QLatin1Char('/')
            || text.at(found - 1) == QLatin1Char('_')
            || text.at(found - 1) == QLatin1Char('-')) score += 8;
        score -= found - position;
        previous = found;
        position = found + 1;
    }
    return score - (text.size() - needle.size()) / 20;
}

QStringList scanWorkspaceFiles(const QStringList& roots) {
    QStringList files;
    QSet<QString> visited;
    const QSet<QString> excluded{QStringLiteral(".git"), QStringLiteral("build"),
        QStringLiteral("node_modules"), QStringLiteral(".cache"), QStringLiteral(".venv"),
        QStringLiteral("venv"), QStringLiteral(".idea"), QStringLiteral(".vscode")};
    QVector<QString> pending;
    for (const QString& root : roots) pending.push_back(root);
    constexpr qsizetype maxFiles = 150000;
    while (!pending.isEmpty() && files.size() < maxFiles) {
        const QString directory = pending.takeLast();
        const QString canonical = QFileInfo(directory).canonicalFilePath();
        const QString identity = canonical.isEmpty() ? QDir::cleanPath(directory) : canonical;
        if (visited.contains(identity)) continue;
        visited.insert(identity);
        const QFileInfoList entries = QDir(directory).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        for (const QFileInfo& entry : entries) {
            if (entry.isDir()) {
                if (!entry.isSymLink() && !entry.fileName().startsWith(QLatin1Char('.'))
                    && !excluded.contains(entry.fileName())) pending.push_back(entry.absoluteFilePath());
            } else if (entry.isFile() && !entry.fileName().startsWith(QLatin1Char('.'))) {
                files.push_back(entry.absoluteFilePath());
                if (files.size() >= maxFiles) break;
            }
        }
    }
    return files;
}
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setObjectName("workspaceBackground");
    createUi();
    lsp_ = new LspManager(this);
    lspStatus_ = new QLabel(this);
    statusBar()->addPermanentWidget(lspStatus_);
    connect(lsp_, &LspManager::statusChanged, this, [this](const QString&) {
        auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget());
        lspStatus_->setText(lsp_->status(editor?editor->filePath():QString{}));
    });
    connect(lsp_, &LspManager::clientStateChanged, this, [this](LspClient* client) {
        scheduleOutline();
        for(int i=0;i<tabs_->count();++i) if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i))) {
            if(!lsp_->serves(client,editor->filePath())) continue;
            if(!client->ready()) {editor->clearSemanticHighlighting();editor->lspPolicy().reset();editor->setProperty("semanticPending",false);}
            else if(tabs_->currentWidget()==editor) requestSemanticHighlighting(editor);
        }
    });
    connect(lsp_, &LspManager::semanticTokensRefresh, this, [this](LspClient* client) {
        auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget());
        if(editor && lsp_->serves(client,editor->filePath())) requestSemanticHighlighting(editor,true);
    });
    configureLsp();
    lspStatus_->setText(lsp_->status());
    connect(lsp_, &LspManager::diagnostics, this, [this](const QString& path, const QJsonArray& items) {
        if (items.isEmpty()) lspProblems_.remove(path); else lspProblems_[path] = items;
        for (int i = 0; i < tabs_->count(); ++i) {
            auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
            if (editor && editor->filePath() == path) editor->setDiagnostics(items);
        }
    });
    connect(lsp_, &LspManager::error, this, [this](const QString& message) {
        statusBar()->showMessage(message, 6000);
    });
    createMenus();
    navigationPollTimer_ = new QTimer(this);
    navigationPollTimer_->setInterval(250);
    connect(navigationPollTimer_, &QTimer::timeout, this, [this] {
        if (restoringNavigation_) return;
        auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget());
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
        for (int i = 0; i < tabs_->count(); ++i) {
            if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i)))
                editor->refreshSyntaxHighlighting();
        }
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
    auto* explorerPage = new QWidget(sidebarStack_);
    auto* explorerLayout = new QVBoxLayout(explorerPage);
    explorerLayout->setContentsMargins(0, 0, 0, 0);
    explorerLayout->setSpacing(0);
    auto* explorerHeading = new QLabel(I18n::text("Explorer"), explorerPage);
    explorerHeading->setObjectName("explorerHeading");
    explorerLayout->addWidget(explorerHeading);
    explorer_ = new ProjectExplorer(explorerPage);
    explorerLayout->addWidget(explorer_, 1);
    searchPanel_ = new SearchPanel(sidebarStack_);
    sidebarStack_->addWidget(explorerPage);
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
        explorer_->setEnabled(false); searchPanel_->setEnabled(false);
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
                explorer_->setEnabled(true); searchPanel_->setEnabled(true); return;
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
    connect(gitPanel_->service(), &GitService::versionsReady, this, [this](const GitDiffSnapshot& snapshot) {
        const QString path=snapshot.repository+"/"+snapshot.entry.path;
        const bool revertFile=!snapshot.staged && pendingGitRevert_==path;
        pendingGitRevert_.clear();
        if (!gitDiff_) {
            gitDiff_ = new GitDiffView(editorArea_); editorArea_->addDockWidget(Qt::LeftDockWidgetArea, gitDiff_);
            connect(gitDiff_, &QDockWidget::visibilityChanged, this, [this, restoreOutline=false](bool visible) mutable {
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
            editorArea_->resizeDocks({gitDiff_}, {qMax(320, (width()-sidebar_->width())/2)}, Qt::Horizontal);
            connect(gitDiff_, &GitDiffView::refreshRequested, this, [this] {
                if(!gitDiff_ || !gitDiff_->isVisible() || gitPanel_->service()->busy()) return;
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
        if(gitDiff_->isVisible() && gitDiff_->path()==path && gitDiff_->staged()==snapshot.staged) {
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
                live.available=[editor,path] { return editor && editor->filePath()==path && !editor->isBusy() && editor->byteSize()<=8*1024*1024; };
                live.read=[editor] { if(!editor || editor->byteSize()>8*1024*1024) return QByteArray{}; return QByteArray::fromStdString(editor->snapshotBuffer().text()); };
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
                live.align=[editor](const QVector<int>& rows) { if(editor) editor->setDiffAlignment(rows); };
                live.highlight=[editor](const GitDiff::Model* model) {
                    if(!editor) return;
                    QVector<EditorWidget::DiffHighlight> highlights;
                    if(model) for(const auto& row:model->rows) if(row.changed && row.right>=0) {
                        auto value=GitDiff::displayText(model->right[row.right]); if(value.endsWith('\n')) value.chop(1);
                        int prefix=0,suffix=0;
                        if(row.left>=0) {
                            auto old=GitDiff::displayText(model->left[row.left]); if(old.endsWith('\n')) old.chop(1);
                            while(prefix<old.size() && prefix<value.size() && old[prefix]==value[prefix]) ++prefix;
                            while(suffix<old.size()-prefix && suffix<value.size()-prefix && old[old.size()-1-suffix]==value[value.size()-1-suffix]) ++suffix;
                        }
                        highlights.append({row.right,prefix,int(value.size())-prefix-suffix});
                    }
                    editor->setDiffHighlights(highlights);
                };
                tabs_->setCurrentWidget(editor); editor->setFocus();
            }
        }
        gitDiff_->displayVersions(snapshot,std::move(live));
        if(revertFile) gitDiff_->revertFile();
    });
    debugPanel_ = new DebugPanel(sidebarStack_); sidebarStack_->addWidget(debugPanel_);
    connect(debugPanel_, &DebugPanel::startRequested, this, [this] {
        for (int i=0;i<tabs_->count();++i) if(auto* e=qobject_cast<EditorWidget*>(tabs_->widget(i)); e && (e->isModified() || e->isBusy())) {
            statusBar()->showMessage(I18n::text("Save all files and wait for file operations before debugging"),6000); return;
        }
        debugPanel_->start();
    });
    connect(debugPanel_, &DebugPanel::configurationRequested, this, [this](const QString& path){openFile(path);});
    connect(debugPanel_, &DebugPanel::currentFileRequested, this, [this] {
        if(auto* e=qobject_cast<EditorWidget*>(tabs_->currentWidget())) debugPanel_->useFile(e->filePath());
    });
    connect(debugPanel_, &DebugPanel::breakpointRequested, this, [this] {
        if(auto* e=qobject_cast<EditorWidget*>(tabs_->currentWidget())) debugPanel_->toggleBreakpoint(e->filePath(),e->navigationPosition().x());
    });
    connect(debugPanel_, &DebugPanel::breakpointsChanged, this, [this](const QString& path) {
        for(auto* e:findChildren<EditorWidget*>()) if(e->filePath()==path) e->setDebugBreakpoints(debugPanel_->breakpoints(path),debugPanel_->disabledBreakpoints(path));
    });
    connect(debugPanel_, &DebugPanel::sourceActivated, this, [this](const QString& path,int line) { if(QFileInfo(path).isFile()) openFile(path,line); });
    connect(debugPanel_, &DebugPanel::locationChanged, this, [this](const QString& path,int line) {
        debugPath_=path; debugLine_=line;
        if(!path.isEmpty() && line>0 && QFileInfo(path).isFile()) openFile(path,line);
        for(auto* e:findChildren<EditorWidget*>()) e->setDebugLine(e->filePath()==path?line:0);
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
    connect(sidebarStack_, &QStackedWidget::currentChanged, this, [this] { updateSidebarActivities(); });
    connect(explorer_, &ProjectExplorer::fileActivated, this, [this](const QString& path) {
        openFile(path);
    });
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
                editor->detachDeletedFile();
                tabs_->setTabText(i, I18n::text("Untitled (deleted) •"));
                tabs_->setTabToolTip(i, I18n::text("Original file was deleted. Use Save As to keep this buffer."));
            } else {
                QWidget* page = tabs_->widget(i);
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
    tabs_ = new EditorTabs(editorArea_);
    editorArea_->setCentralWidget(tabs_);
    tabs_->setTabsClosable(false);
    tabs_->setMovable(false);
    tabs_->setDocumentMode(true);
    auto* tabBar=tabs_->tabBar();
    tabBar->setExpanding(false);
    tabBar->setDrawBase(false);
    tabBar->setElideMode(Qt::ElideMiddle);
    tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabBar,&QWidget::customContextMenuRequested,this,[this,tabBar](const QPoint& position) {
        const int index=tabBar->tabAt(position);
        if(index<0 || index!=tabs_->currentIndex()) return;
        QPointer<QWidget> page=tabs_->widget(index);
        QMenu menu(this);menu.setObjectName("currentTabMenu");
        auto* close=menu.addAction(Codicons::icon("close"), I18n::text("Close"));close->setObjectName("closeCurrentTab");
        auto* closeAll=menu.addAction(Codicons::icon("close-all"), I18n::text("Close All"));closeAll->setObjectName("closeAllTabs");
        auto* selected=menu.exec(tabBar->mapToGlobal(position));
        if(selected==close && page) closeTab(tabs_->indexOf(page));
        else if(selected==closeAll) closeAllTabs();
    });
    connect(tabs_,&QTabWidget::tabCloseRequested,this,[this](int index){closeTab(index);});
    connect(tabs_, &QTabWidget::currentChanged, this, [this,tabBar](int index) {
        scheduleOutline();
        if(gitDiff_ && gitDiff_->isVisible() && gitDiff_->nativeEditor() && gitDiff_->nativeEditor()!=tabs_->widget(index)) gitDiff_->hide();
        if(tabSwitcher_ && tabSwitcher_->isVisible()) finishTabSwitch(false);
        if(index<0 && tabs_->count()>0) {
            tabs_->setTabVisible(0,true);tabs_->setCurrentIndex(0);return;
        }
        // Reveal the new current tab before hiding the others, so hiding a tab
        // never asks QTabWidget to select an unrelated document.
        if(index>=0 && !tabBar->isTabVisible(index)) tabs_->setTabVisible(index,true);
        for(int i=0;i<tabs_->count();++i)
            if(i!=index && tabBar->isTabVisible(i)) tabs_->setTabVisible(i,false);
        if(lsp_ && lspStatus_) {
            auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(index));
            lspStatus_->setText(lsp_->status(editor?editor->filePath():QString{}));
        }
        if (index >= 0) {
            alignCurrentTabTitle();
            QTimer::singleShot(0,this,&MainWindow::alignCurrentTabTitle);
            updateTabMru(tabs_->widget(index));
            requestSemanticHighlighting(qobject_cast<EditorWidget*>(tabs_->widget(index)));
        } else tabs_->findChild<QWidget*>("tabGutter")->hide();
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
    if (!visible && tabs_ && tabs_->currentWidget()) tabs_->currentWidget()->setFocus();
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
    connect(searchAction, &QAction::triggered, this, &MainWindow::searchInProject);
    auto* findAction = searchMenu->addAction(Codicons::icon("search"), I18n::text("Find in Current File..."));
    ShortcutManager::instance().bind("search.find", findAction);
    connect(findAction, &QAction::triggered, this, [this] {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget());
        if (editor) editor->showFindBar();
    });
    auto* nextFindAction = searchMenu->addAction(Codicons::icon("arrow-down"), I18n::text("Find Next"));
    ShortcutManager::instance().bind("search.next", nextFindAction);
    connect(nextFindAction, &QAction::triggered, this, [this] {
        if (auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget())) editor->findNext();
    });
    auto* previousFindAction = searchMenu->addAction(Codicons::icon("arrow-up"), I18n::text("Find Previous"));
    ShortcutManager::instance().bind("search.previous", previousFindAction);
    connect(previousFindAction, &QAction::triggered, this, [this] {
        if (auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget())) editor->findNext(true);
    });
    auto* replaceAction = searchMenu->addAction(Codicons::icon("replace"), I18n::text("Replace in Current File..."));
    ShortcutManager::instance().bind("search.replace", replaceAction);
    connect(replaceAction, &QAction::triggered, this, [this] {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget());
        if (!editor) return;
        editor->showFindBar(true);
    });

    auto* codeMenu = menuBar()->addMenu(I18n::text("&Code"));
    for(const bool block:{false,true}) {
        auto* action=codeMenu->addAction(Codicons::icon("comment"),I18n::text(block?"Toggle block comment":"Toggle line comment"));
        const auto id=block?"editor.toggleBlockComment":"editor.toggleLineComment";
        action->setObjectName(id);ShortcutManager::instance().bind(id,action);action->setShortcutContext(Qt::WidgetShortcut);
        connect(action,&QAction::triggered,this,[this,block] {
            if(auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget())) {
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
            if(auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget())) {
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
        requestCompletion(qobject_cast<EditorWidget*>(tabs_->currentWidget()), false);
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
    auto* quickOpenAction = goMenu->addAction(Codicons::icon("go-to-file"), I18n::text("Quick Open..."));
    ShortcutManager::instance().bind("navigation.quickOpen", quickOpenAction);
    connect(quickOpenAction, &QAction::triggered, this, &MainWindow::quickOpen);
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
    auto* editorSettings=settingsMenu->addAction(Codicons::icon("settings-gear"), I18n::text("Editor..."));
    connect(editorSettings,&QAction::triggered,this,&MainWindow::showEditorSettings);
    auto* interfaceLanguageAction = settingsMenu->addAction(Codicons::icon("globe"), I18n::text("Interface language..."));
    ShortcutManager::instance().bind("settings.interfaceLanguage", interfaceLanguageAction);
    connect(interfaceLanguageAction, &QAction::triggered, this, &MainWindow::showInterfaceLanguageSettings);
    auto* themeAction = settingsMenu->addAction(Codicons::icon("color-mode"), I18n::text("Theme..."));
    ShortcutManager::instance().bind("settings.theme", themeAction);
    connect(themeAction, &QAction::triggered, this, [this] { ThemeManager::instance().showSettings(this); });
    auto* shortcutsAction = settingsMenu->addAction(Codicons::icon("keyboard"), I18n::text("Keyboard shortcuts..."));
    ShortcutManager::instance().bind("settings.shortcuts", shortcutsAction);
    connect(shortcutsAction, &QAction::triggered, this, [this] { ShortcutManager::instance().showSettings(this); });
    auto* lspAction = settingsMenu->addAction(Codicons::icon("server"), I18n::text("Language server..."));
    ShortcutManager::instance().bind("settings.lsp", lspAction);
    connect(lspAction, &QAction::triggered, this, &MainWindow::showLspSettings);
    auto* languagesAction = settingsMenu->addAction(Codicons::icon("code"), I18n::text("&Languages..."));
    ShortcutManager::instance().bind("settings.languages", languagesAction);
    connect(languagesAction, &QAction::triggered, this, &MainWindow::showLanguageSettings);

    auto* viewMenu = menuBar()->addMenu(I18n::text("&View"));
    outline_=new OutlinePanel(editorArea_);
    editorArea_->addDockWidget(Qt::RightDockWidgetArea,outline_);
    editorArea_->resizeDocks({outline_},{280},Qt::Horizontal);
    outlineTimer_=new QTimer(this); outlineTimer_->setSingleShot(true); outlineTimer_->setInterval(350);
    connect(outlineTimer_,&QTimer::timeout,this,&MainWindow::refreshOutline);
    outline_->navigate=[this](int line,int column) {
        auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget());
        if(editor && editor==outlineEditor_ && !editor->isBusy()) {
            editor->revealMatch(line,column,0,true); editor->setFocus();
        }
    };
    auto* outlineAction=outline_->toggleViewAction();
    outlineAction->setObjectName("view.outline"); outlineAction->setIcon(Codicons::icon("list-tree"));
    viewMenu->addAction(outlineAction);
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
    auto* terminalAction = viewMenu->addAction(Codicons::icon("terminal"), I18n::text("Terminal"));
    terminalAction->setObjectName("view.terminal");
    ShortcutManager::instance().bind("view.terminal", terminalAction);
    connect(terminalAction, &QAction::triggered, this, [this] {
        if (terminalPanel_ && terminalPanel_->isVisible()) {
            terminalPanel_->hide();
            return;
        }
        terminalPreviousFocus_ = QApplication::focusWidget();
        const bool firstOpen = !terminalPanel_;
        ensureTerminal();
        QString directory = projectRoots_.value(0);
        if (!explicitProject_) {
            if (auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget()); editor && !editor->filePath().isEmpty())
                directory = QFileInfo(editor->filePath()).absolutePath();
        }
        terminalPanel_->open(directory.isEmpty() ? QDir::homePath() : directory);
        if (firstOpen) editorArea_->resizeDocks({terminalPanel_}, {qMax(180, editorArea_->height()/3)}, Qt::Vertical);
    });
    auto* hiddenAction = viewMenu->addAction(Codicons::icon("eye"), I18n::text("Show Hidden Files"));
    hiddenAction->setCheckable(true);
    ShortcutManager::instance().bind("view.hidden", hiddenAction);
    connect(hiddenAction, &QAction::toggled, this, [this](bool show) {
        explorer_->setShowHidden(show);
        searchPanel_->setShowHidden(show);
        statusBar()->showMessage(show ? I18n::text("Hidden files are visible") : I18n::text("Hidden files are hidden"), 2000);
    });
    auto* explorerAction = viewMenu->addAction(Codicons::icon("files"), I18n::text("Toggle Side Bar"));
    explorerAction->setObjectName("view.explorer");
    ShortcutManager::instance().bind("view.explorer", explorerAction);
    connect(explorerAction, &QAction::triggered, this, [this] {
        setSidebarVisible(sidebar_->isHidden());
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
    debugPanel_->setWorkspace(valid.value(0));
    explicitProject_ = explicitProject && !valid.isEmpty();
    if (lsp_) lsp_->setRoot(projectRoots_.isEmpty() ? QDir::homePath() : projectRoots_.front());
    if (terminalPanel_) terminalPanel_->setDirectory(projectRoots_.value(0));
    explorer_->setRoots(projectRoots_);
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

void MainWindow::quickOpen() {
    QDialog dialog(this);
    dialog.setWindowTitle(I18n::text("Quick Open"));
    dialog.setModal(true);
    dialog.resize(680, 420);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(10, 10, 10, 10);
    auto* input = new QLineEdit(&dialog);
    input->setPlaceholderText(I18n::text("Search files by name or path..."));
    auto* results = new QListWidget(&dialog);
    results->setUniformItemSizes(true);
    layout->addWidget(input);
    layout->addWidget(results, 1);

    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("editor/recentFiles")).toStringList();
    recent.erase(std::remove_if(recent.begin(), recent.end(), [](const QString& path) {
        return !QFileInfo::exists(path);
    }), recent.end());
    const QStringList roots = explicitProject_ ? projectRoots_ : QStringList{};
    QStringList openFiles;
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i)); editor && !editor->filePath().isEmpty())
            openFiles.push_back(editor->filePath());
    }
    QStringList indexedFiles;
    auto* watcher = new QFutureWatcher<QStringList>(&dialog);
    const auto refresh = [&] {
        const QString query = input->text().trimmed();
        struct Match { QString path; int score; };
        QVector<Match> matches;
        QSet<QString> seen;
        const auto consider = [&](const QString& path, int recentRank) {
            if (seen.contains(path)) return;
            QString display = path;
            bool inScope = roots.isEmpty() && openFiles.contains(path);
            for (const QString& root : roots) {
                const QString relative = QDir(root).relativeFilePath(path);
                if (!QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") &&
                    !relative.startsWith(QStringLiteral("../"))) {
                    display = relative;
                    inScope = true;
                    break;
                }
            }
            if (!inScope) return;
            seen.insert(path);
            const int score = query.isEmpty() ? 0 : fuzzyMatchScore(display, query);
            if (!query.isEmpty() && score < 0) return;
            matches.push_back({path, score + (recentRank >= 0 ? 10000 - recentRank * 100 : 0)});
        };
        for (int i = 0; i < recent.size(); ++i) consider(recent.at(i), i);
        for (const QString& path : openFiles) consider(path, -1);
        for (const QString& path : indexedFiles) consider(path, -1);
        std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
            return a.score > b.score;
        });
        results->clear();
        for (int i = 0; i < std::min(100, static_cast<int>(matches.size())); ++i) {
            const QString path = matches.at(i).path;
            QString display = path;
            for (const QString& root : roots) {
                const QString prefix = QDir(root).absolutePath() + QDir::separator();
                if (path.startsWith(prefix)) { display = path.mid(prefix.size()); break; }
            }
            auto* item = new QListWidgetItem(FileIcons::icon(path), display, results);
            item->setToolTip(path);
            item->setData(Qt::UserRole, path);
        }
        if (results->count() > 0) results->setCurrentRow(0);
    };
    connect(input, &QLineEdit::textChanged, &dialog, refresh);
    connect(watcher, &QFutureWatcher<QStringList>::finished, &dialog, [&] {
        indexedFiles = watcher->result();
        input->setPlaceholderText(I18n::text("Search files by name or path (%1 files)...")
                                      .arg(indexedFiles.size()));
        refresh();
    });
    watcher->setFuture(QtConcurrent::run([roots] { return scanWorkspaceFiles(roots); }));
    connect(input, &QLineEdit::returnPressed, &dialog, [&] {
        if (results->currentItem()) dialog.accept();
    });
    connect(results, &QListWidget::itemActivated, &dialog, [&dialog](QListWidgetItem*) {
        dialog.accept();
    });
    auto* nextResult = new QShortcut(QKeySequence(Qt::Key_Down), &dialog);
    nextResult->setContext(Qt::WidgetWithChildrenShortcut);
    connect(nextResult, &QShortcut::activated, &dialog, [results] {
        if (results->count() == 0) return;
        results->setCurrentRow(std::min(results->currentRow() + 1, results->count() - 1));
    });
    auto* previousResult = new QShortcut(QKeySequence(Qt::Key_Up), &dialog);
    previousResult->setContext(Qt::WidgetWithChildrenShortcut);
    connect(previousResult, &QShortcut::activated, &dialog, [results] {
        if (results->count() == 0) return;
        results->setCurrentRow(std::max(0, results->currentRow() - 1));
    });
    input->setFocus();
    refresh();
    if (dialog.exec() == QDialog::Accepted && results->currentItem()) {
        const QString path = results->currentItem()->data(Qt::UserRole).toString();
        if (!path.isEmpty()) openFile(path);
    }
}

void MainWindow::updateTabMru(QWidget* current) {
    if (!current) return;
    tabMru_.erase(std::remove_if(tabMru_.begin(), tabMru_.end(), [this](const QPointer<QWidget>& tab) {
        return tab.isNull() || tabs_->indexOf(tab.data()) < 0;
    }), tabMru_.end());
    tabMru_.erase(std::remove_if(tabMru_.begin(), tabMru_.end(), [current](const QPointer<QWidget>& tab) {
        return tab.data() == current;
    }), tabMru_.end());
    tabMru_.prepend(QPointer<QWidget>(current));
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

void MainWindow::requestClose(QVector<QPointer<EditorWidget>> editors,bool window) {
    if(closing_)return;
    if(renameInProgress_ || !replaceLockedEditors_.isEmpty()) {
        statusBar()->showMessage(I18n::text("Wait for the current operation to finish before closing."),6000);return;
    }
    for(const auto& editor:editors)if(editor && editor->isBusy()) {
        statusBar()->showMessage(I18n::text("Wait for the current operation to finish before closing."),6000);return;
    }
    closing_=true;closingWindow_=window;closingIndex_=0;
    menuBar()->setEnabled(false);sidebar_->setEnabled(false);
    findChild<QWidget*>("activityBar")->setEnabled(false);
    closingEditors_=std::move(editors);discardedRevisions_.clear();
    for(const auto& editor:closingEditors_)if(editor)editor->setEnabled(false);
    advanceClose();
}

void MainWindow::advanceClose() {
    while(closingIndex_<closingEditors_.size()) {
        auto editor=closingEditors_[closingIndex_++];
        if(!editor || !editor->isModified())continue;
        tabs_->setCurrentWidget(editor);
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
    menuBar()->setEnabled(true);sidebar_->setEnabled(true);
    findChild<QWidget*>("activityBar")->setEnabled(true);
    for(const auto& editor:closingEditors_)if(editor)editor->setEnabled(true);
    if(!accepted){closing_=false;closingEditors_.clear();return;}
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
        if(recovery_)recovery_->forgetDocument(editor->property("recoveryId").toString());
        recoveredRevisions_.remove(editor);
        externalFileStates_.remove(editor);
        externallyReloading_.remove(editor);
        tabs_->removeTab(tabs_->indexOf(editor));editor->deleteLater();
    }
    closingEditors_.clear();closing_=false;
    if(auto* current=tabs_->currentWidget())current->setFocus();
    updateWindowTitle();checkpointRecovery();
}

void MainWindow::alignCurrentTabTitle() {
    if(!tabs_) return;
    auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget());
    if(!editor) return;
    const auto boundary=editor->viewport()->mapTo(tabs_,QPoint(editor->lineNumberAreaWidth(),0));
    tabs_->setHeaderGutter(boundary.x(),boundary.y());
}

void MainWindow::positionTabSwitcher() {
    if(!tabSwitcher_) return;
    int textWidth=0;
    for(int row=0;row<tabSwitchList_->count();++row)
        textWidth=std::max(textWidth,tabSwitchList_->fontMetrics().horizontalAdvance(tabSwitchList_->item(row)->text()));
    const auto margins=tabSwitcher_->layout()->contentsMargins();
    const int rowHeight=std::max(28,tabSwitchList_->fontMetrics().height()+12);
    const int popupWidth=std::min(std::clamp(textWidth+40,220,480),std::max(1,width()-32));
    const int popupHeight=std::min(std::max(1,height()-80),margins.top()+margins.bottom()+2*tabSwitcher_->frameWidth()+std::min(10,tabSwitchList_->count())*rowHeight);
    const int popupTop=std::max(menuBar()->height()+12,height()/3-popupHeight/2);
    tabSwitcher_->setGeometry((width()-popupWidth)/2,popupTop,popupWidth,popupHeight);
}

void MainWindow::switchRecentTab(int direction) {
    if(!tabSwitcher_ || !tabSwitcher_->isVisible()) {
        updateTabMru(tabs_->currentWidget());
        if(tabMru_.isEmpty()) return;
        tabSwitchOrder_=tabMru_;tabSwitchIndex_=0;
        if(!tabSwitcher_) {
            tabSwitcher_=new QFrame(this);tabSwitcher_->setObjectName("openFileSwitcher");
            tabSwitcher_->setFrameShape(QFrame::StyledPanel);
            auto* layout=new QVBoxLayout(tabSwitcher_);
            layout->setContentsMargins(6,6,6,6);layout->setSpacing(0);
            tabSwitchList_=new QListWidget(tabSwitcher_);tabSwitchList_->setObjectName("openFileSwitchList");
            tabSwitchList_->setFrameShape(QFrame::NoFrame);
            tabSwitchList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            layout->addWidget(tabSwitchList_);
            connect(tabSwitchList_,&QListWidget::itemClicked,this,[this]{finishTabSwitch(true);});
        }
        tabSwitchList_->clear();
        for(const auto& page:tabSwitchOrder_) {
            const int index=tabs_->indexOf(page);
            auto* item=new QListWidgetItem(tabs_->tabText(index),tabSwitchList_);
            item->setSizeHint(QSize(0,std::max(28,tabSwitchList_->fontMetrics().height()+12)));
        }
        tabSwitchFocus_=QApplication::focusWidget();
        positionTabSwitcher();tabSwitcher_->show();tabSwitcher_->raise();
        qApp->installEventFilter(this);tabSwitchList_->setFocus();
    }
    tabSwitchIndex_=(tabSwitchList_->currentRow()<0?0:tabSwitchList_->currentRow());
    tabSwitchIndex_=(tabSwitchIndex_+direction+tabSwitchOrder_.size())%tabSwitchOrder_.size();
    tabSwitchList_->setCurrentRow(tabSwitchIndex_);
    tabSwitchList_->scrollToItem(tabSwitchList_->currentItem());
}

void MainWindow::finishTabSwitch(bool accept) {
    if(!tabSwitcher_ || !tabSwitcher_->isVisible()) return;
    const int row=tabSwitchList_->currentRow();
    QPointer<QWidget> target=accept && row>=0 && row<tabSwitchOrder_.size()?tabSwitchOrder_.at(row):QPointer<QWidget>{};
    qApp->removeEventFilter(this);tabSwitcher_->hide();
    tabSwitchOrder_.clear();tabSwitchIndex_=-1;
    if(target && tabs_->indexOf(target)>=0) {
        tabs_->setCurrentWidget(target);target->setFocus();
    } else if(tabSwitchFocus_ && isActiveWindow()) tabSwitchFocus_->setFocus();
    tabSwitchFocus_.clear();
}

bool MainWindow::eventFilter(QObject* watched,QEvent* event) {
    if(!tabSwitcher_ || !tabSwitcher_->isVisible()) return QMainWindow::eventFilter(watched,event);
    if(watched==this && event->type()==QEvent::Resize) positionTabSwitcher();
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
    auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget());
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
    auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget());
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

void MainWindow::openFile(const QString& inputPath, int line, int column, int length, bool centerTarget) {
    if(closing_)return;
    const QString path = QDir::cleanPath(QFileInfo(inputPath).absoluteFilePath());
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
        if (editor && (editor->filePath()==path ||
            (editor->isBusy() && editor->filePath().isEmpty() && editor->property("openingPath").toString()==path))) {
            tabs_->setCurrentIndex(i);
            rememberRecentFile(path);
            if (line > 0) editor->revealMatch(line, column, length, centerTarget);
            return;
        }
    }

    auto* editor = createEditor(path);

    QPointer<EditorWidget> editorGuard(editor);
    auto* watcher = new QFutureWatcher<LoadedFile>(this);
    connect(watcher, &QFutureWatcher<LoadedFile>::finished, this,
            [this, watcher, editorGuard, path, line, column, length, centerTarget] {
        if(watcher->future().resultCount()==0) { watcher->deleteLater();return; }
        const LoadedFile result = watcher->future().resultAt(watcher->future().resultCount()-1);
        watcher->deleteLater();
        if (!editorGuard) return;
        const int tabIndex = tabs_->indexOf(editorGuard);
        if (tabIndex < 0) return;
        if (!result.buffer) {
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
        rememberRecentFile(path);
        if (lsp_ && lsp_->available() && editorGuard->lspAutomaticEligible()) {
            if (lsp_->supports(path)) {
                lsp_->open(path, editorGuard->lspText());
                requestSemanticHighlighting(editorGuard);
            }
        }
        if(lsp_ && lsp_->available() && lsp_->supports(path) && !editorGuard->lspAutomaticEligible())
            statusBar()->showMessage(I18n::text("Automatic language-server loading deferred; use a manual language command to start it."),8000);
        if(lsp_ && tabs_->currentWidget()==editorGuard) lspStatus_->setText(lsp_->status(path));
        if (line > 0) editorGuard->revealMatch(line, column, length, centerTarget);
        tabs_->setTabText(tabIndex, QFileInfo(path).fileName());
        if(editorGuard->property("restoreVertical").isValid())editorGuard->verticalScrollBar()->setValue(editorGuard->property("restoreVertical").toInt());
        if(editorGuard->property("restoreHorizontal").isValid())editorGuard->horizontalScrollBar()->setValue(editorGuard->property("restoreHorizontal").toInt());
        if(editorGuard->property("restoreActive").toBool())tabs_->setCurrentWidget(editorGuard);
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
    explorer_->setEnabled(false); searchPanel_->setEnabled(false);
    for (const auto& editor : affected) if (editor) editor->setBusy(true);
    if (gitDiff_) gitDiff_->hide();
    if(branch.isEmpty()) gitPanel_->pullRepository(repository);
    else gitPanel_->switchBranch(repository, branch);
}

void MainWindow::finishGitBranchSwitch(const QString& repository) {
    if (repository != branchRepository_) return;
    const auto editors = branchLockedEditors_; branchLockedEditors_.clear(); branchRepository_.clear();
    explorer_->setEnabled(true); searchPanel_->setEnabled(true);
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
    if (closing_ || !tabs_) return;
    QSet<QString> files, directories;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
        if (!editor || editor->filePath().isEmpty()) continue;
        const QString path = editor->filePath();
        const QFileInfo info(path);
        if (info.isFile()) files.insert(path);
        // Watching the directory survives atomic replacement of the file. Watch
        // the nearest surviving ancestor when an entire directory disappears.
        QDir parent = info.absoluteDir();
        while (!parent.exists() && !parent.isRoot())
            parent.setPath(QFileInfo(parent.absolutePath()).absolutePath());
        directories.insert(parent.absolutePath());
    }
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
        const auto state = diskFileState(path);
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
    saveEditor(qobject_cast<EditorWidget*>(tabs_->currentWidget()));
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
    const auto previousPath=editor->filePath();
    editor->setBusy(true);
    QPointer<EditorWidget> guard(editor);
    statusBar()->showMessage(I18n::text("Saving…"));
    auto* watcher=new QFutureWatcher<SavedFile>(this);
    connect(watcher,&QFutureWatcher<SavedFile>::finished,this,[this,watcher,guard,path,previousPath,completion] {
        const auto result=watcher->result();watcher->deleteLater();
        if(!guard){if(completion)completion(false);return;}
        guard->setBusy(false);guard->applyHistorySettings();
        if(!result.ok) {
            if(result.error!="Saving cancelled")IconDialogs::MessageBox::critical(this,I18n::text("Save failed"),result.error);
            if(completion)completion(false);
            return;
        }
        if(lsp_ && previousPath!=path)lsp_->close(previousPath);
        if(previousPath!=path) debugPanel_->renameSource(previousPath,path);
        guard->completeAsyncSave(path);
        guard->setDebugBreakpoints(debugPanel_->breakpoints(path),debugPanel_->disabledBreakpoints(path));guard->document()->diskState=result.diskState;
        if(lsp_ && lsp_->available() && lsp_->supports(path) && guard->lspEligible() &&
           (guard->lspAutomaticEligible() || lsp_->documentVersion(path)>0)) {
            if(previousPath!=path)lsp_->open(path,guard->lspText());
            else lsp_->change(path,guard->lspText());
            lsp_->saved(path);
        }
        const auto index=tabs_->indexOf(guard);
        if(index>=0){tabs_->setTabText(index,QFileInfo(path).fileName());tabs_->setTabToolTip(index,path);}
        updateWindowTitle();statusBar()->showMessage(I18n::text("Saved"),2000);
        checkpointRecovery();
        gitPanel_->refresh();
        if(completion)completion(true);
    });
    const auto document=editor->document();
    auto cancelled=std::make_shared<std::atomic<bool>>(false);
    auto percent=std::make_shared<std::atomic<int>>(0);
    auto* progress=new IconDialogs::ProgressDialog(I18n::text("Saving…"),I18n::text("Cancel"),0,100,this);
    progress->setWindowModality(Qt::NonModal);progress->setMinimumDuration(300);
    connect(progress,&QProgressDialog::canceled,progress,[cancelled]{cancelled->store(true);});
    connect(editor,&QObject::destroyed,[cancelled]{cancelled->store(true);});
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
    saveEditor(qobject_cast<EditorWidget*>(tabs_->currentWidget()),{},true);
}

void MainWindow::updateWindowTitle() {
    auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget());
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

void MainWindow::searchInProject() {
    setSidebarVisible(true);
    sidebarStack_->setCurrentWidget(searchPanel_);
    explorerActivity_->setChecked(false);
    searchActivity_->setChecked(true);
    gitActivity_->setChecked(false);
    searchPanel_->focusSearch();
}

void MainWindow::startProjectReplace(ProjectReplaceRequest request) {
    QSet<QString> selectedPaths(request.selectedPaths.cbegin(), request.selectedPaths.cend());
    if (selectedPaths.isEmpty()) {
        for (qsizetype index = 0; index < request.matches.size(); ++index) {
            if (!request.selected.isEmpty() && !request.selected.testBit(index)) continue;
            selectedPaths.insert(request.matches[index].path);
        }
    }
    replaceLockedEditors_.clear();
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (!editor) continue;
        const QString path = editor->filePath().isEmpty()
            ? tabs_->tabToolTip(index) : editor->filePath();
        if (!selectedPaths.contains(path)) continue;
        if (editor->isModified() || editor->isBusy()) {
            request.protectedPaths.push_back(path);
        } else {
            editor->setBusy(true);
            replaceLockedEditors_.push_back(editor);
        }
    }
    explorer_->setEnabled(false);
    searchPanel_->startReplace(std::move(request));
}

void MainWindow::finishProjectReplace(const ProjectReplaceResult& result) {
    explorer_->setEnabled(true);
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
}

void MainWindow::showLanguageSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(I18n::text("Language Support"));
    dialog.setMinimumSize(600, 480);
    dialog.resize(700, 620);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(12);

    auto* heading = new QLabel(I18n::text("Language Support"), &dialog);
    QFont headingFont = heading->font();
    headingFont.setPointSize(15);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto* description = new QLabel(
        I18n::text("Enable a language to download its Tree-sitter grammar automatically. Installation happens in the background; files remain usable as plain text while a grammar is unavailable."), &dialog);
    description->setObjectName("description");
    description->setWordWrap(true);
    root->addWidget(description);

    auto* status = new QLabel(I18n::text("Select a language to install or enable its grammar."), &dialog);
    status->setObjectName("status");
    status->setWordWrap(true);
    root->addWidget(status);
    connect(grammarManager_, &GrammarManager::statusChanged, status,
            [status](const QString&, const QString& message) { status->setText(message); });

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(4, 4, 4, 4);
    contentLayout->setSpacing(8);

    struct LanguageToggle { QString id; QString name; QPointer<QCheckBox> checkbox; };
    QVector<LanguageToggle> toggles;
    QString currentCategory;
    for (const auto& language : LanguageRegistry::languages()) {
        if (language.category != currentCategory) {
            currentCategory = language.category;
            auto* categoryLabel = new QLabel(currentCategory, content);
            QFont categoryFont = categoryLabel->font();
            categoryFont.setBold(true);
            categoryFont.setPointSize(categoryFont.pointSize() + 1);
            categoryLabel->setFont(categoryFont);
            contentLayout->addWidget(categoryLabel);
        }
        const bool installed = GrammarManager::isInstalled(language.id);
        auto* checkbox = new QCheckBox(language.name + (installed ? I18n::text("  · Installed") : I18n::text("  · Not installed")), content);
        checkbox->setChecked(installed && LanguageRegistry::isEnabled(language.id));
        checkbox->setToolTip(installed ? I18n::text("Toggle syntax highlighting for this language.")
                                      : I18n::text("Enable to download and install the official Tree-sitter grammar."));
        contentLayout->addWidget(checkbox);
        toggles.push_back({language.id, language.name, checkbox});

        connect(checkbox, &QCheckBox::toggled, &dialog,
                [this, id = language.id, name = language.name, checkbox, status](bool enabled) {
            if (!enabled) {
                LanguageRegistry::setEnabled(id, false);
                for (int i = 0; i < tabs_->count(); ++i)
                    if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i))) editor->refreshSyntaxHighlighting();
                return;
            }
            if (GrammarManager::isInstalled(id)) {
                LanguageRegistry::setEnabled(id, true);
                for (int i = 0; i < tabs_->count(); ++i)
                    if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i))) editor->refreshSyntaxHighlighting();
                status->setText(I18n::text("Language enabled."));
                return;
            }
            LanguageRegistry::setEnabled(id, true);
            checkbox->setText(name + I18n::text("  · Installing…"));
            status->setText(I18n::text("Preparing grammar download…"));
            grammarManager_->install(id);
        });
    }
    contentLayout->addStretch(1);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    auto* note = new QLabel(
        I18n::text("Grammars are downloaded from their public Tree-sitter repositories, compiled locally, and stored in your user data directory. A C compiler and tar are required for installation. The IDE itself does not need to be rebuilt."), &dialog);
    note->setObjectName("note");
    note->setWordWrap(true);
    root->addWidget(note);

    connect(grammarManager_, &GrammarManager::installationFinished, &dialog,
            [this, toggles, status](const QString& id, bool success) {
        for (const auto& toggle : toggles) {
            if (toggle.id != id || !toggle.checkbox) continue;
            toggle.checkbox->setEnabled(true);
            if (success) {
                toggle.checkbox->setText(toggle.name + I18n::text("  · Installed"));
                const bool enabled = LanguageRegistry::isEnabled(id);
                {
                    const QSignalBlocker blocker(toggle.checkbox);
                    toggle.checkbox->setChecked(enabled);
                }
                status->setText(enabled ? I18n::text("Grammar installed and language enabled.")
                                        : I18n::text("Grammar installed; language remains disabled."));
            } else {
                toggle.checkbox->setText(toggle.name + I18n::text("  · Not installed"));
                const QSignalBlocker blocker(toggle.checkbox);
                toggle.checkbox->setChecked(false);
            }
        }
        if (success) {
            for (int i = 0; i < tabs_->count(); ++i)
                if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i))) editor->refreshSyntaxHighlighting();
        }
    });

    auto* buttons = new IconDialogs::ButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    root->addWidget(buttons);
    dialog.exec();
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
    requestSemanticHighlighting(qobject_cast<EditorWidget*>(tabs_->currentWidget()),true);
    scheduleOutline();
}
void MainWindow::showLspSettings() {
    LspSettingsDialog dialog(lsp_,this);
    if(dialog.exec()!=QDialog::Accepted) return;
    const bool semantic=QSettings().value("lsp/semanticHighlighting",true).toBool();
    for(int i=0;i<tabs_->count();++i) if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i))) {
        editor->hideCompletions();
        if(!semantic) {lsp_->cancelSemanticTokens(editor->filePath());editor->clearSemanticHighlighting();}
    }
    if(dialog.serverChanged) configureLsp();
    else requestSemanticHighlighting(qobject_cast<EditorWidget*>(tabs_->currentWidget()),true);
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
        if(guard && tabs_->currentWidget()==guard && guard->filePath()==path && guard->lspRevision()==revision
            && guard->property("manualLspWait").toULongLong()==serial) retry();
    });
    QTimer::singleShot(15000,this,[connection]{QObject::disconnect(*connection);});
    return false;
}

void MainWindow::formatDocument() {
    if(closing_ || renameInProgress_ || !replaceLockedEditors_.isEmpty()) return;
    auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget());
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
    lsp_->formatDocument(path,4,true,[this,guard,path,revision,generation,before](QJsonValue result) {
        if(closing_ || renameInProgress_ || !replaceLockedEditors_.isEmpty() || !guard || tabs_->currentWidget()!=guard || guard->isBusy() || guard->filePath()!=path
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
    if (!lsp_ || !editor || tabs_->currentWidget()!=editor || editor->isBusy()
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
        if (!guard || guard->filePath() != path || tabs_->currentWidget() != guard || guard->lspRevision() != revision || guard->lspPosition() != position || !guard->hasFocus()) return;
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
void MainWindow::showCodeHelp(bool signature) {
    auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget());
    if (!editor) return;
    if(!ensureManualLsp(editor,[this,signature]{showCodeHelp(signature);})) return;
    const auto path=editor->filePath(); const auto revision=editor->lspRevision(); const auto pos=editor->lspPosition();
    lsp_->change(path, editor->lspText()); QPointer<EditorWidget> guard(editor);
    auto callback = [this, guard, path, revision, pos, signature](QJsonValue value) {
        if (!guard || tabs_->currentWidget()!=guard || guard->filePath()!=path || guard->lspRevision()!=revision || guard->lspPosition()!=pos) return;
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

EditorWidget* MainWindow::createEditor(const QString& path) {
    auto* editor = new EditorWidget(this, std::make_shared<Document>());
    editor->setDebugBreakpoints(debugPanel_->breakpoints(path),debugPanel_->disabledBreakpoints(path));
    connect(editor,&EditorWidget::debugAnchorsMoved,this,[this,editor](const QMap<int,int>& anchors){debugPanel_->remapBreakpoints(editor->filePath(),anchors);});
    connect(editor,&EditorWidget::debugLinesEdited,this,[this,editor](int row,int column,int removed,int inserted){debugPanel_->moveBreakpoints(editor->filePath(),row,column,removed,inserted);});
    editor->setDebugLine(path==debugPath_?debugLine_:0);
    connect(editor,&EditorWidget::breakpointToggled,this,[this,editor](int line) {
        debugPanel_->toggleBreakpoint(editor->filePath(),line);
    });
    // Identify the tab before the asynchronous load assigns Document::filePath.
    editor->setProperty("openingPath",path);
    const auto alignTitle=[this,editor] {
        if(tabs_->currentWidget()==editor) alignCurrentTabTitle();
    };
    connect(editor,&EditorWidget::gutterWidthChanged,this,alignTitle);
    connect(editor,&EditorWidget::semanticViewportChanged,this,alignTitle);
    connect(editor, &EditorWidget::codeContextMenuRequested, this, [this, editor](const QPoint& global) {
        tabs_->setCurrentWidget(editor);
        auto* menu = new QMenu(editor);
        menu->setObjectName("editorCodeContextMenu");
        menu->setAttribute(Qt::WA_DeleteOnClose);
        for (const auto& id : {"code.definition", "code.declaration", "code.typeDefinition", "code.references", "code.rename"})
            if (auto* action = findChild<QAction*>(id)) menu->addAction(action);
        menu->popup(global);
    });
    editor->setBusy(true);
    connect(editor, &EditorWidget::hoverRequested, this, [this, editor](int line, int column, const QPoint& global) {
        if (!lsp_ || !lsp_->ready(editor->filePath()) || !editor->lspEligible() || !lsp_->supports(editor->filePath())
            || !editor->lspPolicy().automaticFeatures(editor->byteSize(),QDateTime::currentMSecsSinceEpoch())) return;
        const auto path = editor->filePath(); const auto revision = editor->lspRevision();
        lsp_->change(path, editor->lspText()); QPointer<EditorWidget> guard(editor);
        lsp_->hover(path, line, column, [this, guard, path, revision, global](QJsonValue value) {
            if (!guard || tabs_->currentWidget()!=guard || guard->filePath()!=path || guard->lspRevision()!=revision || (QCursor::pos()-global).manhattanLength()>12) return;
            const auto contents=value.toObject().value("contents"); QString text;
            auto plain=[](QJsonValue item) { return item.isString()?item.toString():item.toObject().value("value").toString(); };
            if (contents.isArray()) for (const auto& item:contents.toArray()) text+=plain(item)+"\n";
            else text=plain(contents);
            if (!text.trimmed().isEmpty()) QToolTip::showText(global, "<pre>"+text.left(8000).toHtmlEscaped()+"</pre>", guard);
        });
    });
    connect(editor,&EditorWidget::highlightingStatusChanged,this,[this,editor](const QString& message) {
        if(tabs_->currentWidget()==editor) statusBar()->showMessage(message,8000);
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
    connect(editor, &EditorWidget::contentChanged, lspSync, [this, editor, lspSync] {
        if(tabs_->currentWidget()==editor) scheduleOutline();
        if (editor->filePath().isEmpty()) return;
        editor->setDiagnostics({});
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
        requestSemanticHighlighting(editor);
    });
    auto* completionTimer = new QTimer(editor);
    completionTimer->setSingleShot(true);
    connect(editor, &EditorWidget::contentChanged, completionTimer, [editor, completionTimer] {
        completionTimer->stop();
        if (!editor->applyingCompletion() && QSettings().value("lsp/autoCompletion", true).toBool())
            completionTimer->start(std::clamp(QSettings().value("lsp/completionDelayMs", 150).toInt(), 50, 2000));
    });
    connect(completionTimer, &QTimer::timeout, this, [this, editor] {
        if (tabs_->currentWidget() == editor) requestCompletion(editor, true);
    });
    editor->setProperty("recoveryId",QUuid::createUuid().toString(QUuid::WithoutBraces));
    connect(editor,&EditorWidget::contentChanged,this,[this,editor] {
        if(documentChangeTimer_) documentChangeTimer_->start();
        if(gitDiff_ && gitDiff_->nativeEditor()==editor) gitDiff_->documentChanged();
        if(!QSettings().value("editor/recoveryEnabled",true).toBool())return;
        if(recoveryTimer_)recoveryTimer_->start();
        if(recoveryDeadline_ && !recoveryDeadline_->isActive())recoveryDeadline_->start();
    });
    const auto document = editor->document();
    connect(editor, &QObject::destroyed, this, [this, document, editor] {
        recoveredRevisions_.remove(editor);
        externalFileStates_.remove(editor);
        externallyReloading_.remove(editor);
        if (lsp_) lsp_->close(document->filePath());
    });

    const int index = tabs_->addTab(editor, QFileInfo(path).fileName() + I18n::text(" (loading…)"));
    tabs_->setTabToolTip(index, path);
    tabs_->setCurrentIndex(index);

    connect(editor, &EditorWidget::modifiedChanged, this, [this, editor] {
        const int index = tabs_->indexOf(editor);
        if (index >= 0) {
            const QString name = editor->filePath().isEmpty()
                ? I18n::text("Untitled (deleted)")
                : QFileInfo(editor->filePath()).fileName();
            tabs_->setTabText(index, name + (editor->isModified() ? " •" : ""));
        }
        updateWindowTitle();
    });
    return editor;
}

void MainWindow::navigateSymbol(const QString& method) {
    auto* editor = qobject_cast<EditorWidget*>(tabs_->currentWidget());
    if (!ensureManualLsp(editor, [this, method] { navigateSymbol(method); })) return;
    const auto path = editor->filePath(); const auto revision = editor->lspRevision();
    const auto position = editor->lspPosition(); QPointer<EditorWidget> guard(editor);
    lsp_->change(path, editor->lspText());
    statusBar()->showMessage(I18n::text("Searching for symbol locations…"), 10000);
    auto callback = [this, guard, path, revision, position, method](QJsonValue result) {
        if (!guard || tabs_->currentWidget()!=guard || guard->filePath()!=path || guard->lspRevision()!=revision || guard->lspPosition()!=position) return;
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
    auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget());
    if (!editor || editor->isBusy() || editor->cursorCount()!=1) return;
    if(!ensureManualLsp(editor,[this]{renameSymbol();})) return;
    if (!lsp_->canRename(editor->filePath())) { statusBar()->showMessage(I18n::text("The language server is not ready or does not support rename."),6000); return; }
    renameInProgress_=true;
    QPointer<MainWindow> owner(this);
    const auto operation=std::shared_ptr<int>(new int(0),[owner](int* value) {
        delete value;
        if (owner) owner->renameInProgress_=false;
    });
    for (int i=0;i<tabs_->count();++i) if (auto* item=qobject_cast<EditorWidget*>(tabs_->widget(i)))
        if (!item->isBusy() && item->lspEligible() && lsp_->supports(item->filePath())) lsp_->change(item->filePath(),item->lspText());
    const auto path=editor->filePath(); const auto revision=editor->lspRevision(); const auto position=editor->lspPosition();
    const QByteArray source=QByteArray::fromStdString(editor->snapshotBuffer().text());
    QPointer<EditorWidget> guard(editor); editor->hideCompletions();
    statusBar()->showMessage(I18n::text("Checking whether the symbol can be renamed…"),10000);
    lsp_->prepareRename(path,position.x(),position.y(),[this,guard,path,revision,position,source,operation](QJsonValue prepared) {
        if (!guard || tabs_->currentWidget()!=guard || guard->filePath()!=path || guard->lspRevision()!=revision || guard->lspPosition()!=position) return;
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
            if (guard) { tabs_->setCurrentWidget(guard); guard->setFocus(); }
            statusBar()->showMessage(QString(I18n::text("Renamed %1 references in %2 files. Changes are not saved yet.")).arg(edits).arg(plan.size()),8000);
        });
    });
}

void MainWindow::showInterfaceLanguageSettings() {
    QDialog dialog(this); dialog.setWindowTitle(I18n::text("Interface language"));
    auto* layout=new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(I18n::text("Language:"),&dialog));
    auto* language=new QComboBox(&dialog); language->setObjectName("interfaceLanguage");
    language->addItem("English","en"); language->addItem(QString::fromUtf8("Русский"),"ru");
    language->setCurrentIndex(language->findData(I18n::selectedLanguage())); layout->addWidget(language);
    auto* note=new QLabel(I18n::text("Restart Turtle Code to apply the selected interface language."),&dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto* buttons=new IconDialogs::ButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        const auto selected=language->currentData().toString();
        if (!I18n::saveLanguage(selected)) { IconDialogs::MessageBox::warning(&dialog,I18n::text("Interface language"),I18n::text("Could not save the interface language.")); return; }
        if (selected!=I18n::activeLanguage()) IconDialogs::MessageBox::information(&dialog,I18n::text("Interface language"),I18n::text("The language choice is saved. Restart the editor when convenient; your open files stay unchanged."));
        dialog.accept();
    });
    dialog.exec();
}

void MainWindow::showEditorSettings() {
    QDialog dialog(this);dialog.setWindowTitle(I18n::text("Editor settings"));
    auto* outer=new QVBoxLayout(&dialog);
    auto* scroll=new QScrollArea(&dialog);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
    auto* content=new QWidget(scroll);scroll->setWidget(content);outer->addWidget(scroll,1);
    auto* layout=new QVBoxLayout(content);QSettings settings;
    auto* fonts=new QGroupBox(I18n::text("Fonts"),&dialog);
    auto* fontLayout=new QFormLayout(fonts);
    struct FontRow { FontSettings::Area area; QFontComboBox* family; QSpinBox* size; };
    std::vector<FontRow> fontRows;
    const auto addFontRow=[&](FontSettings::Area area,const QString& label,const QString& name) {
        const auto selected=FontSettings::font(area);
        auto* row=new QWidget(fonts);auto* rowLayout=new QHBoxLayout(row);rowLayout->setContentsMargins(0,0,0,0);
        auto* family=new QFontComboBox(row);family->setObjectName(name+"FontFamily");family->setCurrentFont(selected);
        auto* size=new SettingsSpinBox(row);size->setObjectName(name+"FontSize");size->setRange(6,96);
        size->setSuffix(I18n::text(" pt"));size->setValue(selected.pointSize());
        rowLayout->addWidget(family,1);rowLayout->addWidget(size);fontLayout->addRow(label,row);
        fontRows.push_back({area,family,size});
    };
    addFontRow(FontSettings::Area::Tree,I18n::text("Project tree:"),"tree");
    addFontRow(FontSettings::Area::Editor,I18n::text("Editor text:"),"editor");
    addFontRow(FontSettings::Area::Terminal,I18n::text("Terminal:"),"terminal");
    layout->addWidget(fonts);
    auto* backgroundGroup = new QGroupBox(I18n::text("Workspace background"), &dialog);
    auto* backgroundLayout = new QFormLayout(backgroundGroup);
    auto* imageRow = new QWidget(backgroundGroup);
    auto* imageLayout = new QHBoxLayout(imageRow); imageLayout->setContentsMargins(0,0,0,0);
    auto* imagePath = new QLineEdit(settings.value("appearance/backgroundImage").toString(), imageRow);
    imagePath->setObjectName("backgroundImage"); imagePath->setReadOnly(true);
    imagePath->setPlaceholderText(I18n::text("No image selected"));
    auto* browseImage = new QPushButton(I18n::text("Choose image…"), imageRow); browseImage->setIcon(Codicons::icon("file-media"));
    auto* clearImage = new QPushButton(I18n::text("Remove"), imageRow); clearImage->setIcon(Codicons::icon("trash"));
    clearImage->setObjectName("removeBackgroundImage");
    imageLayout->addWidget(imagePath,1); imageLayout->addWidget(browseImage); imageLayout->addWidget(clearImage);
    backgroundLayout->addRow(I18n::text("Image:"), imageRow);
    auto* imageOpacity = new SettingsSpinBox(backgroundGroup); imageOpacity->setObjectName("backgroundOpacity");
    imageOpacity->setRange(0,100); imageOpacity->setSuffix(" %");
    imageOpacity->setValue(std::clamp(settings.value("appearance/backgroundOpacity",15).toInt(),0,100));
    backgroundLayout->addRow(I18n::text("Opacity:"),imageOpacity);
    auto* backgroundNote = new QLabel(I18n::text("One image behind the project tree, text and panels. Scaled to fill the workspace without distortion."),backgroundGroup);
    backgroundNote->setWordWrap(true); backgroundLayout->addRow(backgroundNote);
    layout->addWidget(backgroundGroup);
    connect(clearImage,&QPushButton::clicked,imagePath,&QLineEdit::clear);
    connect(browseImage,&QPushButton::clicked,&dialog,[&] {
        QStringList patterns;
        for (const auto& format : QImageReader::supportedImageFormats()) patterns << "*." + QString::fromLatin1(format);
        const auto path = IconDialogs::FileDialog::getOpenFileName(&dialog,I18n::text("Choose background image"),imagePath->text(),
            I18n::text("Images (%1)").arg(patterns.join(' ')));
        if (!path.isEmpty()) imagePath->setText(path);
    });
    layout->addWidget(new QLabel(I18n::text("Undo/Redo memory budget, MiB"),&dialog));
    auto* memory=new SettingsSpinBox(&dialog);memory->setObjectName("historyMemoryMiB");memory->setRange(1,4096);
    memory->setValue(settings.value("editor/historyMemoryMiB",64).toInt());layout->addWidget(memory);
    auto* spill=new QCheckBox(I18n::text("Store history exceeding the memory budget on disk"),&dialog);
    spill->setObjectName("historySpillToDisk");spill->setChecked(settings.value("editor/historySpillToDisk",true).toBool());layout->addWidget(spill);
    layout->addWidget(new QLabel(I18n::text("History disk budget, MiB (0: unlimited)"),&dialog));
    auto* disk=new SettingsSpinBox(&dialog);disk->setObjectName("historyDiskMiB");disk->setRange(0,1048576);disk->setSpecialValueText(I18n::text("Unlimited"));
    disk->setValue(settings.value("editor/historyDiskMiB",0).toInt());disk->setEnabled(spill->isChecked());layout->addWidget(disk);
    connect(spill,&QCheckBox::toggled,disk,&QSpinBox::setEnabled);
    auto* note=new QLabel(I18n::text("When disk storage is disabled or its limit is reached, the oldest Undo/Redo actions are removed. Changes apply to open documents. Temporary history is deleted when its document closes."),&dialog);
    note->setWordWrap(true);layout->addWidget(note);
    auto* autoPairs=new QCheckBox(I18n::text("Automatically close brackets and quotes"),&dialog);
    autoPairs->setObjectName("autoPairs");autoPairs->setChecked(settings.value("editor/autoPairs",true).toBool());
    layout->addWidget(autoPairs);
    auto* backups=new QCheckBox(I18n::text("Automatically back up unsaved documents"),&dialog);
    backups->setObjectName("recoveryEnabled");backups->setChecked(settings.value("editor/recoveryEnabled",true).toBool());
    backups->setToolTip(I18n::text("Existing recovery copies are kept when disabled. Workspace and cursor positions are still remembered."));
    layout->addWidget(backups);
    auto* buttons=new IconDialogs::ButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);outer->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        if (!imagePath->text().isEmpty()) {
            QImageReader reader(imagePath->text()); reader.setAutoTransform(true);
            if (reader.read().isNull()) {
                IconDialogs::MessageBox::warning(&dialog,I18n::text("Workspace background"),I18n::text("Cannot open this image. Choose another file or remove the background."));
                return;
            }
        }
        const bool backgroundChanged = settings.value("appearance/backgroundImage").toString() != imagePath->text()
            || settings.value("appearance/backgroundOpacity",15).toInt() != imageOpacity->value();
        for(const auto& row:fontRows) {
            const auto prefix=FontSettings::key(row.area);
            settings.setValue(prefix+"family",row.family->currentFont().family());
            settings.setValue(prefix+"size",row.size->value());
        }
        settings.setValue("appearance/backgroundImage",imagePath->text());
        settings.setValue("appearance/backgroundOpacity",imageOpacity->value());
        settings.setValue("editor/historyMemoryMiB",memory->value());settings.setValue("editor/historySpillToDisk",spill->isChecked());
        settings.setValue("editor/historyDiskMiB",disk->value());
        settings.setValue("editor/autoPairs",autoPairs->isChecked());
        settings.setValue("editor/recoveryEnabled",backups->isChecked());settings.sync();
        if(settings.status()!=QSettings::NoError){IconDialogs::MessageBox::warning(&dialog,I18n::text("Editor settings"),I18n::text("Cannot save settings."));return;}
        if (backgroundChanged) ThemeManager::instance().refreshAppearance();
        for(auto* widget:QApplication::allWidgets()) {
            if(auto* window=qobject_cast<MainWindow*>(widget)) {
                window->applyRecoverySettings();
                if(window->searchPanel_) window->searchPanel_->applyFontSettings();
                if(window->outline_) window->outline_->applyFontSettings();
                if(window->gitPanel_) window->gitPanel_->applyFontSettings();
            }
            if(auto* editor=qobject_cast<EditorWidget*>(widget)) editor->applyFontSettings();
            else if(auto* terminal=qobject_cast<TerminalWidget*>(widget)) terminal->applyFontSettings();
            else if(auto* tree=qobject_cast<ProjectExplorer*>(widget)) tree->setFont(FontSettings::font(FontSettings::Area::Tree));
        }
        for(auto* editor:findChildren<EditorWidget*>())if(!editor->isBusy())editor->applyHistorySettings();
        dialog.accept();
    });
    // Size the initial window from the translated content, including the font
    // selectors and numeric fields. Keep vertical scrolling on smaller screens.
    dialog.ensurePolished();
    layout->activate();
    const auto margins = outer->contentsMargins();
    const int scrollWidth = scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll);
    const QSize available = dialog.screen()->availableGeometry().size() - QSize(40,80);
    const int width = std::min(available.width(), std::max(760,
        content->sizeHint().width() + margins.left() + margins.right() + scrollWidth + 4));
    const int contentWidth = width - margins.left() - margins.right() - scrollWidth - 4;
    const int contentHeight = layout->hasHeightForWidth()
        ? layout->totalHeightForWidth(contentWidth) : content->sizeHint().height();
    const int height = std::min(available.height(), std::max(700,
        contentHeight + margins.top() + margins.bottom() + buttons->sizeHint().height() + outer->spacing() + 4));
    dialog.resize(width, height);
    dialog.exec();
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
        if(editor->isBusy())continue;
        const auto path=editor->filePath();
        if(clean && path.isEmpty())continue;
        const auto id=editor->property("recoveryId").toString();
        const bool hasCopy=recovery_ && QFileInfo::exists(recovery_->directory()+"/"+id+".json");
        if(!clean && !backupsEnabled && path.isEmpty() && !hasCopy)continue;
        const auto position=editor->navigationPosition();
        QJsonObject entry{{"id",id},{"path",path},{"line",position.x()},{"column",position.y()},
            {"vertical",editor->verticalScrollBar()->value()},{"horizontal",editor->horizontalScrollBar()->value()},
            {"modified",!clean && editor->isModified() && (backupsEnabled || hasCopy)}};
        documents.append(entry);
        if(tabs_->currentWidget()==editor)active=id;
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
                auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget());
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
                if(id==candidate.state["active"].toString())tabs_->setCurrentWidget(guard);
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
            else if(tabs_->currentWidget()) tabs_->currentWidget()->setFocus();
        }
    });
    editorArea_->resizeDocks({terminalPanel_},{qMax(180,editorArea_->height()/3)},Qt::Vertical);
}

void MainWindow::scheduleOutline() {
    if(!outline_ || !outlineTimer_) return;
    ++outlineGeneration_; outlineTimer_->stop();
    if(!outline_->isVisible()) return;
    auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget());
    const bool changed=outlineEditor_!=editor; outlineEditor_=editor;
    if(!editor) { outline_->showMessage(I18n::text("Open a file to see its outline.")); return; }
    outline_->pending(changed);
    outlineTimer_->start();
}

void MainWindow::refreshOutline() {
    if(!outline_ || !outline_->isVisible() || !lsp_) return;
    auto* editor=qobject_cast<EditorWidget*>(tabs_->currentWidget());
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
            || guard->filePath()!=path || owner->tabs_->currentWidget()!=guard || !owner->outline_->isVisible()) return;
        if(result.isUndefined() || (!result.isNull() && !result.isArray())) {
            owner->outline_->showMessage(I18n::text("Could not load outline.")); return;
        }
        owner->outline_->setSymbols(result.toArray(),path);
    });
}
