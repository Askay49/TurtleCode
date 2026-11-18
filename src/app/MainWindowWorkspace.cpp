#include "MainWindow.hpp"
#include "Codicons.hpp"
#include "I18n.hpp"
#include "editor/EditorPaneWorkspace.hpp"
#include "editor/EditorWidget.hpp"
#include "editor/VimDiffSession.hpp"
#include "git/GitDiffView.hpp"
#include "language/LspManager.hpp"

#include <QFileInfo>
#include <QAction>
#include <QLabel>
#include <QMenu>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <algorithm>

namespace {
class BufferNames {
public:
    void add(const QString& path) {
        if (!path.isEmpty()) paths_[QFileInfo(path).fileName()].insert(path);
    }

    QString label(const QString& path, const EditorWidget* editor) const {
        if (path.isEmpty()) return I18n::text(editor->document()->property("detachedDeletedFile").toBool()
            ? "Untitled (deleted)" : "Untitled");
        const auto name = QFileInfo(path).fileName();
        const auto found = paths_.constFind(name);
        if (found == paths_.cend() || found->size() < 2) return name;
        auto separator = path.lastIndexOf('/');
        while (separator > 0) {
            separator = path.lastIndexOf('/', separator - 1);
            const auto suffix = path.sliced(separator + 1);
            if (std::none_of(found->cbegin(), found->cend(), [&](const QString& other) {
                return other != path && (other == suffix || other.endsWith('/' + suffix));
            })) return suffix;
        }
        return path;
    }

private:
    QHash<QString, QSet<QString>> paths_;
};

QString menuText(QString title) {
    return title.replace('&', QStringLiteral("&&"));
}
}

EditorWidget* MainWindow::activeEditor() const {
    if (paneWorkspace_ && paneWorkspace_->count()) return paneWorkspace_->activeEditor();
    return tabs_ ? qobject_cast<EditorWidget*>(tabs_->currentWidget()) : nullptr;
}

EditorWidget* MainWindow::documentEditor(const EditorWidget* view) const {
    if (!view || !tabs_) return nullptr;
    if (tabs_->indexOf(view) >= 0) return const_cast<EditorWidget*>(view);
    return documentEditor(view->document().get());
}

EditorWidget* MainWindow::documentEditor(const Document* document) const {
    if (!tabs_) return nullptr;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
        if (editor && editor->document().get() == document) return editor;
    }
    return nullptr;
}

EditorWidget* MainWindow::showBufferInPane(EditorPane* pane, EditorWidget* source) {
    if (!pane || !source) return nullptr;
    auto* current = pane->editor();
    if (current && current->document() == source->document()) return current;
    if (current) {
        pane->alternate = documentEditor(current);
        removeVimDiffView(current);
    }
    auto* view = pane->viewFor(source->document().get());
    if (!view) {
        view = new EditorWidget(pane, source->document());
        view->initializeSharedView(*source);
        connectEditor(view, source->filePath(), false);
    }
    pane->showView(view);
    return view;
}

EditorWidget* MainWindow::activateEditor(EditorWidget* editor, bool focus) {
    if (!editor) return nullptr;
    if (!paneWorkspace_->count()) {
        tabs_->setCurrentWidget(editor);
        if (focus) editor->setFocus();
        return editor;
    }
    const QScopedValueRollback changing(updatingWorkspace_, true);
    auto* pane = paneWorkspace_->paneFor(editor);
    if (pane) {
        paneWorkspace_->selectPane(pane);
        if (pane->editor() != editor) showBufferInPane(pane, editor);
    } else {
        pane = paneWorkspace_->activePane();
        editor = showBufferInPane(pane, editor);
    }
    updateWorkspaceFocus(focus);
    return editor;
}

void MainWindow::updateWorkspaceFocus(bool focus) {
    auto* view = paneWorkspace_->activeEditor();
    const auto alternate = alternateBuffer(view);
    if (view) {
        view->setProperty("vimAlternatePath", alternate ? alternate->filePath() : QString{});
        if (auto* primary = documentEditor(view)) {
            tabs_->setCurrentWidget(primary);
            updateTabMru(primary);
        }
        if (focus) view->setFocus();
    }
    if (gitDiff_ && gitDiff_->isVisible() && gitDiff_->nativeEditor() != view) gitDiff_->hide();
    if (lsp_ && lspStatus_) {
        lspStatus_->setText(lsp_->status(view ? view->filePath() : QString{}));
        requestSemanticHighlighting(view);
    }
    updateVimIndicator();
    updateEditorPaneActions();
    updateWindowTitle();
    scheduleOutline();
}

void MainWindow::updateEditorPaneActions() {
    const auto* editor = activeEditor();
    const bool ready = editor && !editor->isBusy() && !closing_;
    for (const auto* name : {"editor.splitBelow", "editor.splitRight"})
        if (auto* action = findChild<QAction*>(name)) action->setEnabled(ready);
    if (auto* action = findChild<QAction*>("editor.closePane"))
        action->setEnabled(ready && (paneWorkspace_->count() > 1 || paneWorkspace_->panes().size() > 1));
}

void MainWindow::updateEditorHeaders() {
    const QSignalBlocker block(vimPageTabs_);
    const int count = paneWorkspace_->count();
    auto* single = count ? nullptr : activeEditor();
    singleEditorHeader_->setVisible(single != nullptr);
    if (single) {
        const auto path = paneBufferPath(single);
        singleEditorHeader_->setDocumentTitle(BufferNames{}.label(path, single), path, single->isModified());
        singleEditorHeader_->setActive(true);
        singleEditorHeader_->setCloseAction(!closing_ && !single->isBusy(), false);
    }

    while (vimPageTabs_->count() > count) vimPageTabs_->removeTab(vimPageTabs_->count() - 1);
    while (vimPageTabs_->count() < count) vimPageTabs_->addTab({});
    const auto* activePane = paneWorkspace_->activePane();
    for (int i = 0; i < count; ++i) {
        const auto panes = paneWorkspace_->panesOnPage(i);
        const bool canClosePane = count > 1 || panes.size() > 1;
        BufferNames names;
        for (auto* pane : panes) if (auto* view = pane->editor()) names.add(paneBufferPath(view));

        QString firstTitle;
        QStringList files;
        bool modified = false;
        for (auto* pane : panes) {
            auto* view = pane->editor();
            auto* header = pane->header();
            header->setVisible(view != nullptr);
            if (!view) continue;
            const auto path = paneBufferPath(view);
            const auto label = names.label(path, view);
            const bool dirty = view->isModified();
            header->setDocumentTitle(label, path, dirty);
            header->setActive(pane == activePane);
            header->setCloseAction(!closing_ && !view->isBusy(), canClosePane);
            if (files.isEmpty()) firstTitle = label;
            auto detail = path.isEmpty() ? label : path;
            if (dirty) detail += QStringLiteral(" •");
            files.append(detail);
            modified |= dirty;
        }
        auto title = files.isEmpty() ? I18n::text("Tab page") : firstTitle;
        if (files.size() > 1) title += QStringLiteral(" +%1").arg(files.size() - 1);
        if (modified) title += QStringLiteral(" •");
        vimPageTabs_->setTabText(i, menuText(title));
        vimPageTabs_->setTabToolTip(i, files.join('\n'));
    }
    vimPageTabs_->setCurrentIndex(paneWorkspace_->currentIndex());
    vimPageTabs_->setVisible(count > 1);
    vimPageTabs_->setEnabled(!closing_);
}

QString MainWindow::paneBufferPath(const EditorWidget* editor) const {
    const auto path = editor->filePath();
    if (!path.isEmpty() || !editor->isBusy()) return path;
    const auto* primary = documentEditor(editor);
    return primary ? primary->property("openingPath").toString() : QString{};
}

void MainWindow::connectEditorHeader(EditorPaneHeader* header, EditorPane* pane) {
    connect(header, &EditorPaneHeader::buffersRequested, this, [this, pane](const QPoint& position) {
        showPaneBuffers(pane, position);
    });
    connect(header, &EditorPaneHeader::actionsRequested, this, [this, pane](const QPoint& position) {
        showPaneActions(pane, position);
    });
    connect(header, &EditorPaneHeader::closeRequested, this, [this, pane] {
        auto* editor = pane ? pane->editor() : activeEditor();
        if (!editor || closing_ || editor->isBusy()) return;
        if (!pane || (paneWorkspace_->count() == 1 && paneWorkspace_->panes().size() == 1))
            closeTab(tabs_->indexOf(documentEditor(editor)));
        else
            closeEditorPane(editor);
    });
}

void MainWindow::showPaneBuffers(EditorPane* pane, const QPoint& position) {
    if (closing_) return;
    QPointer<EditorPane> destination(pane);
    const auto* current = documentEditor(pane ? pane->editor() : activeEditor());
    QMenu menu(this);
    menu.setObjectName("editorPaneBuffers");
    QHash<QAction*, QPointer<EditorWidget>> choices;
    BufferNames names;
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i))) names.add(paneBufferPath(editor));
    for (const auto& item : tabMru_) {
        auto* editor = qobject_cast<EditorWidget*>(item.data());
        if (!editor || tabs_->indexOf(editor) < 0) continue;
        auto title = names.label(paneBufferPath(editor), editor);
        if (editor->isModified()) title += QStringLiteral(" •");
        auto* action = menu.addAction(menuText(title));
        action->setCheckable(true);
        action->setChecked(editor == current);
        choices.insert(action, editor);
    }
    const auto target = choices.value(menu.exec(position));
    if (closing_ || !target || tabs_->indexOf(target) < 0) return;
    if (pane) {
        if (!destination || !paneWorkspace_->panes(true).contains(destination)) return;
        paneWorkspace_->selectPane(destination);
    }
    activateEditor(target, true);
}

void MainWindow::showPaneActions(EditorPane* pane, const QPoint& position) {
    QPointer<EditorWidget> editor(pane ? pane->editor() : activeEditor());
    if (!editor || closing_) return;
    QMenu menu(this);
    menu.setObjectName("editorPaneMenu");
    auto* right = menu.addAction(Codicons::icon("split-vertical"), I18n::text("Split Editor Right"));
    auto* below = menu.addAction(Codicons::icon("split-horizontal"), I18n::text("Split Editor Below"));
    right->setEnabled(!editor->isBusy());
    below->setEnabled(!editor->isBusy());
    QAction* only = nullptr;
    const bool split = pane && paneWorkspace_->panes().size() > 1;
    if (split) {
        only = menu.addAction(I18n::text("Keep Only This Pane"));
        only->setObjectName("keepOnlyEditorPane");
    }
    const bool closesPane = pane && (split || paneWorkspace_->count() > 1);
    menu.addSeparator();
    auto* close = menu.addAction(Codicons::icon("close"), I18n::text(closesPane ? "Close File" : "Close"));
    close->setObjectName("closeCurrentTab");
    close->setEnabled(!editor->isBusy());
    auto* closeAll = menu.addAction(Codicons::icon("close-all"), I18n::text("Close All"));
    closeAll->setObjectName("closeAllTabs");
    auto* selected = menu.exec(position);
    if (!selected || !editor || closing_) return;
    if (selected == right || selected == below) {
        if (editor->isBusy() || (pane && !paneWorkspace_->paneFor(editor))) return;
        activateEditor(editor, false);
        splitEditor(selected == right ? Qt::Horizontal : Qt::Vertical);
    } else if (selected == only) {
        const QScopedValueRollback changing(updatingWorkspace_, true);
        if (keepOnlyEditorPane(editor)) updateWorkspaceFocus(true);
    } else if (selected == close) {
        closeTab(tabs_->indexOf(documentEditor(editor)));
    } else if (selected == closeAll) {
        closeAllTabs();
    }
}

void MainWindow::viewFocused(EditorWidget* editor) {
    if (updatingWorkspace_ || !paneWorkspace_->count()) return;
    auto* pane = paneWorkspace_->paneFor(editor);
    if (!pane || pane->editor() != editor) return;
    const QScopedValueRollback changing(updatingWorkspace_, true);
    paneWorkspace_->selectPane(pane);
    updateWorkspaceFocus(false);
}

EditorWidget* MainWindow::alternateBuffer(const EditorWidget* view) const {
    if (paneWorkspace_ && paneWorkspace_->count()) {
        auto* pane = view ? paneWorkspace_->paneFor(view) : paneWorkspace_->activePane();
        if (pane) return pane->alternate;
    }
    return vimAlternateEditor_;
}

void MainWindow::initializePaneWorkspace() {
    if (paneWorkspace_->count()) return;
    auto* source = activeEditor();
    const QScopedValueRollback changing(updatingWorkspace_, true);
    if (gitDiff_) gitDiff_->hide();
    auto* pane = paneWorkspace_->newPage();
    pane->alternate = vimAlternateEditor_;
    if (source) showBufferInPane(pane, source);
    tabs_->hide();
    paneWorkspace_->show();
    updateWorkspaceFocus(false);
}

EditorWidget* MainWindow::splitEditor(Qt::Orientation orientation) {
    auto* source = activeEditor();
    if (!source || source->isBusy() || closing_) return nullptr;
    initializePaneWorkspace();
    const QScopedValueRollback changing(updatingWorkspace_, true);
    source = activeEditor();
    auto* previous = paneWorkspace_->activePane();
    auto* pane = paneWorkspace_->split(orientation);
    if (!pane) return nullptr;
    pane->alternate = previous->alternate;
    auto* view = showBufferInPane(pane, source);
    updateWorkspaceFocus(true);
    return view;
}

bool MainWindow::closeEditorPane(EditorWidget* editor) {
    auto* pane = paneWorkspace_->paneFor(editor);
    if (!pane || closing_ || (editor && editor->isBusy())) return false;
    if (paneWorkspace_->count() == 1 && paneWorkspace_->panes().size() == 1) return false;
    const QScopedValueRollback changing(updatingWorkspace_, true);
    removeVimDiffView(editor);
    if (!paneWorkspace_->closePane(pane)) return false;
    updateWorkspaceFocus(true);
    return true;
}

bool MainWindow::keepOnlyEditorPane(EditorWidget* editor) {
    auto* pane = paneWorkspace_->paneFor(editor);
    const auto siblings = paneWorkspace_->panes();
    if (closing_ || !siblings.contains(pane)) return false;
    for (auto* other : siblings) {
        if (other != pane && other->editor() && other->editor()->isBusy()) {
            editor->reportVimExMessage(I18n::text("Vim: the document is busy"));
            return false;
        }
    }
    for (auto* other : siblings) if (other != pane) removeVimDiffView(other->editor());
    paneWorkspace_->selectPane(pane);
    paneWorkspace_->keepPane(pane);
    return true;
}

bool MainWindow::hideEditorView(EditorWidget* editor) {
    auto* primary = documentEditor(editor);
    if (closeEditorPane(editor) && documentEditor(activeEditor()) != primary) return true;
    auto* next = alternateBuffer(activeEditor());
    if (next == primary || (next && next->isBusy())) next = nullptr;
    for (int i = 0; !next && i < tabs_->count(); ++i) {
        auto* candidate = qobject_cast<EditorWidget*>(tabs_->widget(i));
        if (candidate && candidate != primary && !candidate->isBusy()) next = candidate;
    }
    if (!next) next = createUntitledEditor();
    return activateEditor(next, true) != nullptr;
}

void MainWindow::removeBufferViews(EditorWidget* editor) {
    if (!paneWorkspace_->count()) return;
    const QScopedValueRollback changing(updatingWorkspace_, true);
    for (auto* pane : paneWorkspace_->panes(true)) {
        if (pane->alternate == editor) pane->alternate.clear();
        auto* view = pane->viewFor(editor->document().get());
        if (!view) continue;
        removeVimDiffView(view);
        if (pane->editor() == view && paneWorkspace_->closePane(pane)) continue;
        if (pane->editor() == view) {
            EditorWidget* next = nullptr;
            for (int i = 0; i < tabs_->count(); ++i) {
                auto* candidate = qobject_cast<EditorWidget*>(tabs_->widget(i));
                if (!candidate || candidate == editor || closingEditors_.contains(candidate)) continue;
                next = candidate;
                break;
            }
            if (next) showBufferInPane(pane, next);
        }
        pane->removeView(view);
        view->deleteLater();
    }
    updateWorkspaceFocus(false);
}

VimDiffSession* MainWindow::vimDiffSession(bool create) {
    auto* page = paneWorkspace_->currentWidget();
    if (!page) return nullptr;
    auto* diff = page->findChild<VimDiffSession*>("vimDiffSession", Qt::FindDirectChildrenOnly);
    return diff || !create ? diff : new VimDiffSession(page);
}

void MainWindow::removeVimDiffView(EditorWidget* editor) {
    if (!editor) return;
    for (auto* session : paneWorkspace_->findChildren<VimDiffSession*>()) session->remove(editor);
}

QString MainWindow::transferVimDiff(EditorWidget* editor, bool put, int first, int last, bool range, const QString& buffer) {
    auto* diff = vimDiffSession();
    if (!diff || !diff->otherView(editor)) return QStringLiteral("E99: Current buffer is not in diff mode");
    if (!buffer.isEmpty()) {
        bool number = false;
        const auto id = buffer.toULongLong(&number);
        const auto* alternate = alternateBuffer(editor);
        QString error;
        const auto path = number || buffer == QLatin1String("#") ? QString{} :
            VimEx::fileArgument(buffer, editor->filePath(), &error, alternate ? alternate->filePath() : QString{});
        if (!error.isEmpty()) return error;
        const auto matches = [&](EditorWidget* view) {
            auto* primary = documentEditor(view);
            if (number) return primary && primary->property("vimBufferId").toULongLong() == id;
            if (buffer == QLatin1String("#")) return primary && primary == alternate;
            return view->filePath() == path || QFileInfo(view->filePath()).fileName() == buffer;
        };
        if (matches(editor)) return {};
        if (!matches(diff->otherView(editor))) return QStringLiteral("E102: Can't find buffer in diff mode");
    }
    return diff->transfer(editor, put, first, last, range);
}

EditorWidget* MainWindow::openVimFile(const QString& path) {
    if (QFileInfo::exists(path)) {
        const QFileInfo file(path);
        if (!file.isFile() || !file.isReadable()) return nullptr;
        openFile(path);
    } else {
        for (int i = 0; i < tabs_->count(); ++i) {
            auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i));
            if (editor && editor->filePath() == path) return activateEditor(editor);
        }
        auto* editor = createEditor(path);
        editor->loadBuffer(PieceTreeBuffer(), path);
        editor->setProperty("openingPath", QVariant{});
        editor->setBusy(false);
        tabs_->setTabText(tabs_->indexOf(editor), QFileInfo(path).fileName());
    }
    return activeEditor();
}

void MainWindow::executeVimWorkspaceKey(EditorWidget* editor, const QString& command, int count, bool explicitCount) {
    if (closing_ || editor->isBusy()) return;
    if (command == "[c" || command == "]c" || command == "do" || command == "dp") {
        auto* diff = vimDiffSession();
        const int line = editor->navigationPosition().x() - 1;
        const auto error = command.startsWith('d') ? transferVimDiff(editor, command == "dp", line, line, false,
                                                                    explicitCount ? QString::number(count) : QString{})
            : diff ? diff->jump(editor, command == "]c" ? 1 : -1, count)
                   : QStringLiteral("E99: Current buffer is not in diff mode");
        if (!error.isEmpty()) editor->reportVimExMessage(error);
        return;
    }
    if (command == "gt" || command == "gT") {
        const int pages = paneWorkspace_->count();
        if (!pages) return;
        const int current = paneWorkspace_->currentIndex();
        const int index = command == "gt" && explicitCount ? count - 1
            : (current + (command == "gt" ? 1 : pages - count % pages)) % pages;
        const QScopedValueRollback changing(updatingWorkspace_, true);
        if (paneWorkspace_->selectPage(index)) updateWorkspaceFocus(true);
        else editor->reportVimExMessage(QStringLiteral("E475: Invalid tab page"));
    } else if (command.startsWith("^W") && command.size() == 3) {
        const QChar key = command.back();
        if (key == 's' || key == 'S' || key == 'v' || key == 'n') {
            if (splitEditor(key == 'v' ? Qt::Horizontal : Qt::Vertical) && key == 'n')
                activateEditor(createUntitledEditor(), true);
        } else if (key == 'q' || key == 'c') {
            if (!closeEditorPane(editor)) {
                if (key == 'q') {
                    if (editor->isModified()) editor->reportVimExMessage(QStringLiteral("E37: No write since last change (add ! to override)"));
                    else requestClose({documentEditor(editor)}, false);
                } else editor->reportVimExMessage(QStringLiteral("E444: Cannot close last window"));
            }
        } else if (paneWorkspace_->count()) {
            const QScopedValueRollback changing(updatingWorkspace_, true);
            if (QStringLiteral("hjklwWtbp").contains(key)) {
                paneWorkspace_->selectPane(paneWorkspace_->focusTarget(key, count, explicitCount));
            } else if (key == 'o') {
                if (!keepOnlyEditorPane(editor)) return;
            } else if (key == '=') {
                paneWorkspace_->equalize();
            } else if (QStringLiteral("+-_<>|").contains(key)) {
                const auto orientation = QStringLiteral("+-_").contains(key) ? Qt::Vertical : Qt::Horizontal;
                const int cell = orientation == Qt::Vertical ? editor->fontMetrics().lineSpacing()
                    : editor->fontMetrics().horizontalAdvance('M');
                const bool absolute = key == '_' || key == '|';
                const int amount = key == '-' || key == '<' ? -count : absolute && !explicitCount ? 10000 : count;
                paneWorkspace_->resizePane(orientation, amount, absolute, cell);
            } else if (key == 'r' || key == 'R') {
                paneWorkspace_->rotate(count, key == 'R');
            } else if (key == 'x') {
                paneWorkspace_->exchange(count, explicitCount);
            } else if (QStringLiteral("HJKL").contains(key)) {
                paneWorkspace_->moveToEdge(key);
            } else if (key == 'T') {
                const auto page = paneWorkspace_->currentWidget();
                if (paneWorkspace_->movePaneToNewPage()) {
                    if (auto* diff = page->findChild<VimDiffSession*>()) diff->remove(editor);
                }
            }
            updateWorkspaceFocus(true);
        }
    }
    if (auto* target = activeEditor(); target && target != editor) {
        target->resetVimMode();
        if (editor->vimCommandExecuting()) editor->transferVimEx(target);
        else editor->transferVimReplay(target);
    }
}

bool MainWindow::executeVimWorkspaceEx(EditorWidget* editor, const VimEx::Invocation& command,
                                       const std::function<void(QString)>& completed) {
    const auto name = command.definition.name;
    const auto argument = command.arguments.trimmed();
    const auto completeInActivePane = [this, editor, &completed] {
        if (auto* target = activeEditor(); target && target != editor) {
            target->resetVimMode();
            editor->transferVimEx(target);
        }
        completed({});
    };
    if (name == "hide") {
        if (!closeEditorPane(editor)) completed(QStringLiteral("E444: Cannot close last window"));
        else completeInActivePane();
        return true;
    }
    if (name == "only" || name == "wincmd") {
        if (name == "wincmd" && (argument.size() != 1 || !QStringLiteral("sSvnqchjklwWtbpo=+-_<>|rRxHJKLT").contains(argument))) {
            completed(QStringLiteral("E474: Invalid argument")); return true;
        }
        executeVimWorkspaceKey(editor, QStringLiteral("^W") + (name == "only" ? QStringLiteral("o") : argument), 1, false);
        completeInActivePane();
        return true;
    }
    if (name == "close" && paneWorkspace_->count() && closeEditorPane(editor)) {
        completeInActivePane();
        return true;
    }
    if (name == "split" || name == "vsplit" || name == "new" || name == "vnew" ||
        name == "tabnew" || name == "tabedit" || name == "diffsplit") {
        const auto* alternate = alternateBuffer(editor);
        QString error;
        const auto path = VimEx::fileArgument(argument, editor->filePath(), &error,
                                              alternate ? alternate->filePath() : QString{});
        if (!error.isEmpty()) { completed(error); return true; }
        const QFileInfo file(path);
        if ((!path.isEmpty() && file.exists() && (!file.isFile() || !file.isReadable())) ||
            (path.isEmpty() && name == "diffsplit")) {
            completed(QStringLiteral("E484: Cannot open file")); return true;
        }
        if (name == "diffsplit" && path == editor->filePath()) {
            completed(I18n::text("Vim: select two different buffers for diff")); return true;
        }
        initializePaneWorkspace();
        auto* original = activeEditor();
        if (name == "tabnew" || name == "tabedit") {
            const QScopedValueRollback changing(updatingWorkspace_, true);
            paneWorkspace_->newPage();
        } else if (!splitEditor(command.vertical || name == "vsplit" || name == "vnew" ? Qt::Horizontal : Qt::Vertical)) {
            completed(I18n::text("Vim: the document is busy")); return true;
        }
        if (!path.isEmpty()) {
            if (auto* target = openVimFile(path)) activateEditor(target, true);
        } else if (name == "new" || name == "vnew" || name == "tabnew" || name == "tabedit") {
            activateEditor(createUntitledEditor(), true);
        }
        if (name == "diffsplit") {
            if (gitDiff_) gitDiff_->hide();
            auto* diff = vimDiffSession(true);
            diff->clear();
            auto* target = activeEditor();
            error = diff->add(original);
            if (error.isEmpty() && target && target->isBusy()) {
                auto* pane = paneWorkspace_->activePane();
                connect(target->document().get(), &Document::busyChanged, diff,
                        [diff, source = QPointer<EditorWidget>(original), target = QPointer<EditorWidget>(target),
                         pane = QPointer<EditorPane>(pane)](bool busy) {
                    if (busy || !source || !target || !pane || pane->editor() != target || !diff->contains(source)) return;
                    const auto error = diff->add(target);
                    if (!error.isEmpty()) target->reportVimExMessage(error);
                }, Qt::SingleShotConnection);
            } else if (error.isEmpty()) {
                error = diff->add(target);
            }
            if (!error.isEmpty()) { completed(error); return true; }
        }
        completeInActivePane();
        return true;
    }
    if (name == "tabs") {
        QStringList result;
        const int pages = paneWorkspace_->count();
        if (!pages) result << I18n::text("Tab page 1") << QStringLiteral("> ") + editor->filePath();
        for (int i = 0; i < pages; ++i) {
            result << QStringLiteral("%1 %2").arg(I18n::text("Tab page"), QString::number(i + 1));
            for (auto* pane : paneWorkspace_->panesOnPage(i)) if (const auto* view = pane->editor())
                result << QStringLiteral("%1 %2%3")
                    .arg(view == editor ? ">" : " ", view->isModified() ? "+" : " ",
                         view->filePath().isEmpty() ? I18n::text("Untitled") : view->filePath());
        }
        editor->reportVimExMessage(result.join('\n').left(4096));
        completed({});
        return true;
    }
    if (name.startsWith("tab")) {
        const int pages = paneWorkspace_->count();
        if (!pages) {
            completed(name == "tabclose" ? QStringLiteral("E784: Cannot close last tab page") : QString{});
            return true;
        }
        const QScopedValueRollback changing(updatingWorkspace_, true);
        const int current = paneWorkspace_->currentIndex();
        bool valid = true;
        const qint64 number = argument.isEmpty() ? 0 : argument.toInt(&valid);
        qint64 index = argument.isEmpty() ? current : number - 1;
        if (name == "tabmove") {
            if (argument.isEmpty() || argument == "$") { index = pages - 1; valid = true; }
            else if (argument == "#") {
                const int previous = paneWorkspace_->previousPage();
                valid = previous >= 0;
                index = previous + (previous < current);
            } else if (argument == "+" || argument == "-") {
                valid = true;
                index = current + (argument == "+" ? 1 : -1);
            } else if (argument.startsWith('+') || argument.startsWith('-')) {
                index = current + number;
            } else {
                valid = valid && number <= pages;
                index = number - (number > current);
            }
            if (valid) paneWorkspace_->movePage(int(std::clamp<qint64>(index, 0, pages - 1)));
        } else if (name == "tabclose" || name == "tabonly") {
            if (name == "tabclose" && pages == 1) { completed(QStringLiteral("E784: Cannot close last tab page")); return true; }
            if (index < 0 || index >= pages) valid = false;
            QList<EditorPane*> closingPanes;
            if (valid) {
                if (name == "tabclose") closingPanes = paneWorkspace_->panesOnPage(index);
                else for (int i = 0; i < pages; ++i) if (i != current) closingPanes.append(paneWorkspace_->panesOnPage(i));
            }
            for (auto* pane : closingPanes) if (pane->editor() && pane->editor()->isBusy()) {
                completed(I18n::text("Vim: the document is busy")); return true;
            }
            for (auto* pane : closingPanes) {
                removeVimDiffView(pane->editor());
                paneWorkspace_->closePane(pane);
            }
        } else {
            if (name == "tabfirst" || name == "tabrewind") index = 0;
            else if (name == "tablast") index = pages - 1;
            else if (name == "tabprevious" || name == "tabNext") {
                if (!argument.isEmpty() && number < 1) valid = false;
                index = (current + pages - (argument.isEmpty() ? 1 : number % pages)) % pages;
            }
            else if (argument == "#") { index = paneWorkspace_->previousPage(); valid = true; }
            else if (argument.isEmpty()) index = (current + 1) % pages;
            if (valid) valid = index >= 0 && index < pages && paneWorkspace_->selectPage(int(index));
        }
        if (!valid) { completed(QStringLiteral("E475: Invalid tab page")); return true; }
        updateWorkspaceFocus(true);
        completeInActivePane();
        return true;
    }
    if (name == "diffthis") {
        initializePaneWorkspace();
        if (gitDiff_) gitDiff_->hide();
        const auto error = vimDiffSession(true)->add(activeEditor());
        if (error.isEmpty()) completeInActivePane();
        else completed(error);
        return true;
    }
    if (name == "diffoff" || name == "diffupdate" || name == "diffget" || name == "diffput") {
        auto* diff = vimDiffSession();
        if (name == "diffoff") {
            if (diff) { if (command.bang) diff->clear(); else diff->remove(editor); }
            completed({});
        } else if (name == "diffupdate") {
            completed(diff ? diff->refresh() : QString{});
        } else {
            completed(transferVimDiff(editor, name == "diffput", int(command.first) - 1,
                                      int(command.last) - 1, command.explicitRange, argument));
        }
        return true;
    }
    return false;
}
