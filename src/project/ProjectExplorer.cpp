#include "WorkspaceFiles.hpp"
#include "WorkspaceThreads.hpp"
#include <QElapsedTimer>
#include <QProgressDialog>
#include <QPushButton>
#include <QPointer>
#include <QDirIterator>
#include <atomic>
#include <vector>
#include "app/IconDialogs.hpp"
#include "app/FileIcons.hpp"
#include "app/Codicons.hpp"
#include "app/I18n.hpp"
#include "app/FontSettings.hpp"
#include "ProjectExplorer.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QKeyEvent>
#include <QMenu>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QtConcurrent/QtConcurrentRun>

namespace {
constexpr qint64 directoryPageSize = 500;

QString cleanPath(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

}

ProjectExplorer::ProjectExplorer(QWidget* parent) : QTreeWidget(parent) {
    setFont(FontSettings::font(FontSettings::Area::Tree));
    setHeaderHidden(true);
    setAnimated(true);
    setIconSize(QSize(16, 16));
    setIndentation(16);
    setUniformRowHeights(true);
    setMinimumWidth(190);
    setMaximumWidth(440);
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this, &QWidget::customContextMenuRequested,
            this, &ProjectExplorer::showContextMenu);

    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int){
        if (!item || activatePageItem(item)) return;

        const QString path = item->data(0, Qt::UserRole).toString();

        if (QFileInfo(path).isDir()) {
            item->setExpanded(!item->isExpanded());
            return;
        }

        if (QFileInfo(path).isFile())
            emit fileClicked(path);
    });

    connect(this, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item) return;
        const auto path = item->data(0, Qt::UserRole).toString();
        if (QFileInfo(path).isFile()) emit fileActivated(path, false);
    });

    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        loadDirectory(item);
    });

    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
            [this](const QString& path) { dirtyDirectories_.insert(path); refreshTimer_.start(); emit filesChanged(); });
    refreshTimer_.setSingleShot(true);refreshTimer_.setInterval(250);
    connect(&refreshTimer_,&QTimer::timeout,this,[this]{const auto paths=dirtyDirectories_;dirtyDirectories_.clear();for(const auto& path:paths)refreshDirectory(path);});
    fallbackTimer_.setInterval(10000);
    connect(&fallbackTimer_,&QTimer::timeout,this,[this]{for(const auto& path:failedWatches_)refreshDirectory(path);});
    fallbackTimer_.start();
}

bool ProjectExplorer::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->keyCombination() == QKeyCombination(Qt::Key_F2)) {
            // Claim F2 before the window shortcut for LSP symbol rename.
            event->accept();
            return true;
        }
    }
    return QTreeWidget::event(event);
}

void ProjectExplorer::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    const auto modifiers = event->modifiers();

    if ((key == Qt::Key_Return || key == Qt::Key_Enter) &&
        (modifiers & ~Qt::KeypadModifier) == Qt::NoModifier) {
        if (auto* item = currentItem(); item && !activatePageItem(item)) {
            const QFileInfo info(item->data(0, Qt::UserRole).toString());
            if (info.isDir()) item->setExpanded(!item->isExpanded());
            else if (info.isFile()) emit fileActivated(info.filePath(), true);
        }
        event->accept();
        return;
    }

    if (key == Qt::Key_F2 && modifiers == Qt::NoModifier) {
        if (const auto* item = currentItem(); item && !event->isAutoRepeat()) {
            const auto path = item->data(0, Qt::UserRole).toString();
            if (!path.isEmpty()) renamePath(path);
        }
        event->accept();
        return;
    }

    if (vimEnabled_ && modifiers == Qt::NoModifier) {
        if (key == Qt::Key_J || key == Qt::Key_K) {
            // Use the tree's arrow handling for visible rows, scrolling and selection.
            QKeyEvent arrow(QEvent::KeyPress, key == Qt::Key_J ? Qt::Key_Down : Qt::Key_Up,
                            Qt::NoModifier);
            QTreeWidget::keyPressEvent(&arrow);
            event->accept();
            return;
        }
        if (key == Qt::Key_H || key == Qt::Key_L) {
            if (auto* item = currentItem(); item &&
                QFileInfo(item->data(0, Qt::UserRole).toString()).isDir())
                item->setExpanded(key == Qt::Key_L);
            event->accept();
            return;
        }
    }

    QTreeWidget::keyPressEvent(event);
}

bool ProjectExplorer::activatePageItem(QTreeWidgetItem* item) {
    const auto offset = item->data(0, Qt::UserRole + 10);
    auto* parent = item->parent();
    if (!parent || !offset.isValid()) return false;

    const auto path = parent->data(0, Qt::UserRole).toString();
    pageOffsets_[path] = offset.toLongLong();
    loadDirectory(parent, true, true);
    return true;
}

void ProjectExplorer::setRoots(const QStringList& roots) {
    scanCancel_->store(true);scanCancel_=std::make_shared<std::atomic_bool>(false);
    ++generation_;
    clear();
    pathIndex_.clear();failedWatches_.clear();dirtyDirectories_.clear();listings_.clear();pageOffsets_.clear();
    roots_.clear();
    loaded_.clear();
    loading_.clear();
    reloadPending_.clear();
    pendingExpandedPaths_.clear();
    finishReveal(true);
    const QStringList watched = watcher_.directories();
    if (!watched.isEmpty()) watcher_.removePaths(watched);
    for (const QString& root : roots) {
        addRoot(root);
        if (!roots_.isEmpty()) break;
    }
}

void ProjectExplorer::setShowHidden(bool show) {
    if (showHidden_ == show) return;
    showHidden_ = show;
    // Keep the existing QTreeWidgetItems alive so expansion, selection and
    // scroll position survive the filter change.  Reconcile every directory
    // that has already been loaded; unopened directories will use the new
    // filter when they are expanded later.
    scanCancel_->store(true);scanCancel_=std::make_shared<std::atomic_bool>(false);
    ++generation_;
    loading_.clear();
    reloadPending_.clear();
    const QStringList loadedDirectories = loaded_.keys();
    for (const QString& path : loadedDirectories) {
        if (QTreeWidgetItem* item = findItem(path))
            loadDirectory(item, true);
    }
    continueReveal();
}

void ProjectExplorer::setIncludeIgnored(bool include) {
    if(includeIgnored_==include)return;
    includeIgnored_=include;
    const bool hidden=showHidden_;showHidden_=!hidden;setShowHidden(hidden);
}

void ProjectExplorer::revealPath(const QString& inputPath, bool interactive) {
    const QString path = inputPath.isEmpty() ? QString{} : cleanPath(inputPath);
    if (!revealPaths_.isEmpty() && revealPaths_.back() == path) {
        interactiveReveal_ |= interactive;
        return;
    }

    revealPaths_.clear();
    interactiveReveal_ = interactive;
    if (!path.isEmpty()) {
        for (const auto& root : roots_) {
            const QDir directory(root);
            const auto relative = directory.relativeFilePath(path);
            if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative))
                continue;

            revealPaths_.append(root);
            if (path != root) {
                for (const auto& part : relative.split('/'))
                    revealPaths_.append(QDir(revealPaths_.back()).filePath(part));
            }
            continueReveal();
            return;
        }
    }
    finishReveal(false);
}

void ProjectExplorer::cancelAutomaticReveal() {
    if (!interactiveReveal_) revealPaths_.clear();
}

void ProjectExplorer::continueReveal() {
    if (revealPaths_.isEmpty()) return;
    auto* item = findItem(revealPaths_.front());
    if (!item) {
        finishReveal(false);
        return;
    }

    for (qsizetype i = 1; i < revealPaths_.size(); ++i) {
        const auto& parentPath = revealPaths_[i - 1];
        const auto& childPath = revealPaths_[i];
        if (loading_.value(parentPath)) return;

        QTreeWidgetItem* child = nullptr;
        if (loaded_.value(parentPath)) {
            for (int row = 0; row < item->childCount(); ++row) {
                auto* candidate = item->child(row);
                if (candidate->data(0, Qt::UserRole).toString() == childPath) {
                    child = candidate;
                    break;
                }
            }
        }
        if (!child) {
            const bool useListing = loaded_.value(parentPath) && !dirtyDirectories_.contains(parentPath);
            loadDirectory(item, true, useListing, childPath);
            return;
        }

        item->setExpanded(true);
        item = child;
    }

    finishReveal(true);
    setCurrentItem(item);
    scrollToItem(item);
}

void ProjectExplorer::finishReveal(bool found) {
    const bool reportFailure = !found && interactiveReveal_;
    revealPaths_.clear();
    interactiveReveal_ = false;
    if (reportFailure) emit revealFailed();
}

void ProjectExplorer::watchDirectory(const QString& path) {
    if(watcher_.directories().contains(path) || watcher_.addPath(path))failedWatches_.remove(path);
    else failedWatches_.insert(path);
}

void ProjectExplorer::addRoot(const QString& inputPath) {
    const QString path = cleanPath(inputPath);
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir() || roots_.contains(path))
        return;
    roots_.push_back(path);
    addRootItem(path);
    watchDirectory(path);
}

void ProjectExplorer::addRootItem(const QString& path) {
    auto* item = new QTreeWidgetItem(this);
    item->setText(0, QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName());
    item->setToolTip(0, path);
    item->setData(0, Qt::UserRole, path);
    item->setData(0, Qt::UserRole + 1, path);
    item->setIcon(0, Codicons::folderIcon());
    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    item->setExpanded(true); // Show the root contents immediately; descendants remain lazy.
}


void ProjectExplorer::loadDirectory(QTreeWidgetItem* item, bool force, bool pageOnly, const QString& revealChild) {
    if (!item) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty() || !QFileInfo(path).isDir()) {
        if (!revealChild.isEmpty()) finishReveal(false);
        return;
    }
    if (loading_.value(path, false) || (loaded_.value(path, false) && !force)) return;

    loading_[path] = true;
    const quint64 generation = generation_;
    QStringList expandedPaths;
    QList<QTreeWidgetItem*> expandedStack;
    for (int i = 0; i < item->childCount(); ++i)
        expandedStack.push_back(item->child(i));
    while (!expandedStack.isEmpty()) {
        QTreeWidgetItem* descendant = expandedStack.takeLast();
        if (descendant->isExpanded())
            expandedPaths.push_back(descendant->data(0, Qt::UserRole).toString());
        for (int i = 0; i < descendant->childCount(); ++i)
            expandedStack.push_back(descendant->child(i));
    }
    for (const QString& expandedPath : expandedPaths) {
        if (!pendingExpandedPaths_.contains(expandedPath))
            pendingExpandedPaths_.push_back(expandedPath);
    }
    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    const QString root = item->data(0, Qt::UserRole + 1).toString();
    auto* future = new QFutureWatcher<Page>(this);
    connect(future, &QFutureWatcher<Page>::finished, this, [this, future, path, root, generation, revealChild] {
        const auto page = future->result();
        const Entries entries=page.entries;
        future->deleteLater();
        if (generation != generation_) return;
        QTreeWidgetItem* parent = findItem(path);
        if (!parent) {
            loading_.remove(path);
            continueReveal();
            return;
        }
        if (!revealChild.isEmpty() && !revealPaths_.contains(revealChild)) {
            loading_[path] = false;
            if (reloadPending_.take(path) && parent->isExpanded()) loadDirectory(parent, true);
            continueReveal();
            return;
        }
        if (!page.error.isEmpty()) {
            loading_[path] = false;
            if (revealPaths_.contains(path)) finishReveal(false);
            setToolTip(page.error);
            emit backgroundError(page.error);
            return;
        }
        listings_[path]=page.listing;pageOffsets_[path]=page.offset;
        // Publish the completed load before model changes can trigger callbacks.
        // Keep loading_ set until reconciliation is done to prevent reentry.
        loaded_[path] = true;
        // Reconcile this directory in place instead of rebuilding its entire
        // child tree. Keeping unchanged QTreeWidgetItems alive preserves their
        // expanded state, current selection, scroll position and lazy-loaded
        // descendants. Rename remaps the existing item's path before refresh,
        // so the renamed item is matched by its new path and reused as well.
        struct Delivery { Entries entries; qsizetype next{0}; QHash<QString,QTreeWidgetItem*> existing; int previous{-1}; };
        auto state=std::make_shared<Delivery>();state->entries=entries;state->previous=parent->childCount()-1;
        auto deliver=std::make_shared<std::function<void()>>();
        const std::weak_ptr<std::function<void()>> weak=deliver;
        *deliver=[this,path,root,generation,state,weak,page,revealChild] {
            if(generation!=generation_)return;
            auto* parent=findItem(path);if(!parent)return;
            QElapsedTimer budget;budget.start();
            while(state->previous>=0) {
                auto* child=parent->child(state->previous--);
                const auto childPath=child->data(0,Qt::UserRole).toString();
                if(childPath.isEmpty())delete child;else state->existing.insert(childPath,child);
                if(budget.elapsed()>=4) {if(auto next=weak.lock())QTimer::singleShot(0,this,[next]{(*next)();});return;}
            }
            while(state->next<state->entries.size()) {
                const Entry& entry=state->entries[state->next++];

            QTreeWidgetItem* child = state->existing.take(entry.path);
            if (!child)
                child = new QTreeWidgetItem(parent);
            child->setText(0, entry.name);
            child->setToolTip(0, entry.path);
            child->setData(0, Qt::UserRole, entry.path);
            child->setData(0, Qt::UserRole + 1, root);
            child->setIcon(0, entry.directory ? Codicons::folderIcon() : FileIcons::icon(entry.path));
            if (entry.directory) {
                // The indicator itself enables lazy expansion; no dummy row is
                // needed, even before this directory's contents are known.
                child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            } else {
                // A path that changed from directory to file must not retain
                // obsolete descendants or a directory expansion indicator.
                const auto obsoleteChildren = child->takeChildren();
                for (QTreeWidgetItem* obsolete : obsoleteChildren)
                    delete obsolete;
                child->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
            }
                pathIndex_[entry.path]=QPersistentModelIndex(indexFromItem(child));
                if(budget.elapsed()>=4) {if(auto next=weak.lock())QTimer::singleShot(0,this,[next]{(*next)();});return;}
            }
            auto& existingChildren=state->existing;

        // Only entries that disappeared from disk are removed. Invalidate the
        // lazy-load state for their descendants before Qt recursively destroys
        // their item subtrees.
        const QStringList watchedDirectories = watcher_.directories();
        for (auto it = existingChildren.cbegin(); it != existingChildren.cend(); ++it) {
            QTreeWidgetItem* stale = it.value();
            const QString stalePrefix = it.key() + QDir::separator();
            QList<QTreeWidgetItem*> stack{stale};
            QStringList removedPaths;
            while (!stack.isEmpty()) {
                QTreeWidgetItem* descendant = stack.takeLast();
                const QString descendantPath = descendant->data(0, Qt::UserRole).toString();
                if (!descendantPath.isEmpty()) removedPaths.push_back(descendantPath);
                for (int i = 0; i < descendant->childCount(); ++i)
                    stack.push_back(descendant->child(i));
            }
            for (const QString& removedPath : removedPaths) {
                loaded_.remove(removedPath);
                listings_.remove(removedPath);pageOffsets_.remove(removedPath);pathIndex_.remove(removedPath);failedWatches_.remove(removedPath);
                loading_.remove(removedPath);
                reloadPending_.remove(removedPath);
            }
            QStringList removeWatchers;
            for (const QString& watchedPath : watchedDirectories) {
                if (watchedPath == it.key() || watchedPath.startsWith(stalePrefix))
                    removeWatchers.push_back(watchedPath);
            }
            if (!removeWatchers.isEmpty()) watcher_.removePaths(removeWatchers);
            delete stale;
        }
        // An empty directory is still expandable/collapsible. Hiding its
        // indicator left it visually stuck open, with no way to collapse it.
        parent->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        if(page.offset>0){auto* previous=new QTreeWidgetItem(parent,{I18n::text("Previous page")});previous->setData(0,Qt::UserRole+10,std::max<qint64>(0,page.offset-directoryPageSize));previous->setIcon(0,Codicons::icon("chevron-left"));}
        if(page.offset+page.entries.size()<page.listing->size()){auto* next=new QTreeWidgetItem(parent,{I18n::text("Next page")});next->setData(0,Qt::UserRole+10,page.offset+page.entries.size());next->setIcon(0,Codicons::icon("chevron-right"));}
        watchDirectory(path);
        loading_[path] = false;
        // Restore expanded descendants progressively as each lazy level loads.
        restoreExpandedPaths();
        if (reloadPending_.take(path) && (parent->isExpanded() || revealPaths_.contains(path)))
            loadDirectory(parent, true);
        else if (page.targetMissing && revealPaths_.contains(revealChild))
            finishReveal(false);
        continueReveal();
        };
        (*deliver)();
    });

    const bool showHidden = showHidden_;
    const bool includeIgnored = includeIgnored_;
    const auto cancel=scanCancel_;
    const auto cached = pageOnly ? listings_.value(path) : std::shared_ptr<DirectoryListing>{};
    const auto offset = pageOffsets_.value(path);
    future->setFuture(QtConcurrent::run(&WorkspaceThreads::indexing(),
                                      [path, root, showHidden, includeIgnored, cached, offset, cancel, revealChild] {
        Page page;
        try {
            page.listing = cached ? cached : DirectoryListing::scan(root, path, {showHidden, includeIgnored, {}}, cancel);
            const auto last = std::max<qint64>(0, page.listing->size() - 1);
            page.offset = std::min(offset, last / directoryPageSize * directoryPageSize);
            if (!revealChild.isEmpty()) {
                const auto position = page.listing->position(revealChild, cancel);
                if (position) page.offset = *position / directoryPageSize * directoryPageSize;
                else page.targetMissing = true;
            }
            page.entries = page.listing->page(page.offset, directoryPageSize);
        } catch (const std::exception& error) {
            page.error = QString::fromUtf8(error.what());
        }
        return page;
    }));
}

QTreeWidgetItem* ProjectExplorer::findItem(const QString& path) const {
    const auto known=pathIndex_.constFind(path);
    if(known!=pathIndex_.cend() && known.value().isValid()) {
        auto* item=itemFromIndex(known.value());
        if(item && item->data(0,Qt::UserRole).toString()==path)return item;
    }
    QList<QTreeWidgetItem*> stack;
    for (int i = 0; i < topLevelItemCount(); ++i) stack.push_back(topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem* item = stack.takeLast();
        if (item->data(0, Qt::UserRole).toString() == path) {pathIndex_[path]=QPersistentModelIndex(indexFromItem(item));return item;}
        for (int i = 0; i < item->childCount(); ++i) stack.push_back(item->child(i));
    }
    return nullptr;
}

void ProjectExplorer::restoreExpandedPaths() {
    for (int i = 0; i < pendingExpandedPaths_.size();) {
        const QString path = pendingExpandedPaths_.at(i);
        QTreeWidgetItem* item = findItem(path);
        if (!item) {
            if (!QFileInfo(path).isDir())
                pendingExpandedPaths_.removeAt(i);
            else
                ++i;
            continue;
        }
        pendingExpandedPaths_.removeAt(i);
        item->setExpanded(true);
    }
}

void ProjectExplorer::refreshDirectory(const QString& path) {
    QTreeWidgetItem* item = findItem(path);
    loaded_[path] = false;
    if (loading_.value(path, false)) {
        reloadPending_[path] = true;
        return;
    }
    if (item && item->isExpanded())
        loadDirectory(item, true);
}


void ProjectExplorer::remapTreePaths(const QString& oldPath, const QString& newPath) {
    const QString oldPrefix = oldPath + QDir::separator();
    QList<QTreeWidgetItem*> stack;
    for (int i = 0; i < topLevelItemCount(); ++i)
        stack.push_back(topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem* item = stack.takeLast();
        const QString currentPath = item->data(0, Qt::UserRole).toString();
        if (currentPath == oldPath || currentPath.startsWith(oldPrefix)) {
            const QString updatedPath = newPath + currentPath.mid(oldPath.size());
            item->setData(0, Qt::UserRole, updatedPath);
            item->setToolTip(0, updatedPath);
            item->setText(0, QFileInfo(updatedPath).fileName());
            item->setIcon(0, QFileInfo(updatedPath).isDir() ? Codicons::folderIcon() : FileIcons::icon(updatedPath));
        }
        const QString rootPath = item->data(0, Qt::UserRole + 1).toString();
        if (rootPath == oldPath || rootPath.startsWith(oldPrefix))
            item->setData(0, Qt::UserRole + 1, newPath + rootPath.mid(oldPath.size()));
        for (int i = 0; i < item->childCount(); ++i)
            stack.push_back(item->child(i));
    }
}

void ProjectExplorer::remapPathState(const QString& oldPath, const QString& newPath) {
    const QString oldPrefix = oldPath + QDir::separator();
    const auto remapKey = [&oldPath, &oldPrefix, &newPath](const QString& key) {
        return key == oldPath || key.startsWith(oldPrefix)
            ? newPath + key.mid(oldPath.size()) : key;
    };

    QHash<QString, bool> remappedLoaded;
    for (auto it = loaded_.cbegin(); it != loaded_.cend(); ++it)
        remappedLoaded.insert(remapKey(it.key()), it.value());
    loaded_.swap(remappedLoaded);

    // In-flight jobs still carry their original path. Let them finish as stale
    // results; a subsequent expansion can start a fresh job at the new path.
    for (auto it = loading_.begin(); it != loading_.end();) {
        if (it.key() == oldPath || it.key().startsWith(oldPrefix)) it = loading_.erase(it);
        else ++it;
    }
    for (auto it = reloadPending_.begin(); it != reloadPending_.end();) {
        if (it.key() == oldPath || it.key().startsWith(oldPrefix)) it = reloadPending_.erase(it);
        else ++it;
    }

    const QStringList watched = watcher_.directories();
    QStringList oldWatched;
    QStringList newWatched;
    for (const QString& directory : watched) {
        if (directory == oldPath || directory.startsWith(oldPrefix)) {
            oldWatched.push_back(directory);
            newWatched.push_back(remapKey(directory));
        }
    }
    if (!oldWatched.isEmpty()) watcher_.removePaths(oldWatched);
    for (const QString& directory : newWatched) {
        if (QFileInfo(directory).isDir() && !watcher_.directories().contains(directory))
            watcher_.addPath(directory);
    }
}

void ProjectExplorer::showContextMenu(const QPoint& position) {
    QTreeWidgetItem* item = itemAt(position);
    if (item)
        setCurrentItem(item);

    QString selectedPath;
    if (item) {
        selectedPath = item->data(0, Qt::UserRole).toString();
    } else if (!roots_.isEmpty()) {
        selectedPath = roots_.front();
    }
    if (selectedPath.isEmpty())
        return;

    const QFileInfo selectedInfo(selectedPath);
    const QString targetDirectory = selectedInfo.isDir()
        ? selectedInfo.absoluteFilePath() : selectedInfo.absolutePath();
    QMenu menu(this);
    QAction* newFileAction = menu.addAction(Codicons::icon("new-file"), I18n::text("New File..."));
    QAction* newFolderAction = menu.addAction(Codicons::icon("new-folder"), I18n::text("New Folder..."));
    QAction* searchAction = nullptr;
    if (item && selectedInfo.isDir()) {
        menu.addSeparator();
        searchAction = menu.addAction(Codicons::icon("search"), I18n::text("Search in This Folder"));
    }
    QAction* renameAction = nullptr;
    QAction* deleteAction = nullptr;
    QAction* copyPathAction = nullptr;
    QAction* copyRelativePathAction = nullptr;
    const QString selectedRoot = item
        ? item->data(0, Qt::UserRole + 1).toString() : QString();
    if (item) {
        menu.addSeparator();
        renameAction = menu.addAction(Codicons::icon("edit"), I18n::text("Rename..."));
        deleteAction = menu.addAction(Codicons::icon("trash"),
            roots_.contains(cleanPath(selectedPath))
                ? I18n::text("Remove Folder from Workspace") : I18n::text("Delete"));
        menu.addSeparator();
        copyPathAction = menu.addAction(Codicons::icon("copy"), I18n::text("Copy path"));
        copyRelativePathAction = menu.addAction(Codicons::icon("copy"), I18n::text("Copy relative path"));
        copyRelativePathAction->setEnabled(!selectedRoot.isEmpty());
    }

    QAction* chosen = menu.exec(viewport()->mapToGlobal(position));
    if (chosen == newFileAction) {
        createFileIn(targetDirectory);
    } else if (chosen == newFolderAction) {
        createFolderIn(targetDirectory);
    } else if (searchAction && chosen == searchAction) {
        emit searchInFolderRequested(targetDirectory);
    } else if (renameAction && chosen == renameAction) {
        renamePath(selectedPath);
    } else if (deleteAction && chosen == deleteAction) {
        deletePath(selectedPath);
    } else if (copyPathAction && chosen == copyPathAction) {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(selectedPath));
    } else if (copyRelativePathAction && chosen == copyRelativePathAction) {
        QApplication::clipboard()->setText(
            QDir::toNativeSeparators(QDir(selectedRoot).relativeFilePath(selectedPath)));
    }
}

void ProjectExplorer::createFileIn(const QString& directory) {
    const QString name = IconDialogs::InputDialog::getText(
        this, I18n::text("New File"), I18n::text("File name:"), QLineEdit::Normal, QString());
    if (name.isEmpty())
        return;
    if (QFileInfo(name).fileName() != name || name == "." || name == "..") {
        IconDialogs::MessageBox::warning(this, I18n::text("Invalid name"), I18n::text("Enter a file name, not a path."));
        return;
    }

    const QString path = QDir(directory).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        IconDialogs::MessageBox::warning(this, I18n::text("Create file failed"),
                             I18n::text("Could not create the file. It may already exist or the directory may be read-only."));
        return;
    }
    file.close();
    refreshDirectory(directory);
    emit fileActivated(path, false);
}

void ProjectExplorer::createFolderIn(const QString& directory) {
    const QString name = IconDialogs::InputDialog::getText(
        this, I18n::text("New Folder"), I18n::text("Folder name:"), QLineEdit::Normal, QString());
    if (name.isEmpty())
        return;
    if (QFileInfo(name).fileName() != name || name == "." || name == "..") {
        IconDialogs::MessageBox::warning(this, I18n::text("Invalid name"), I18n::text("Enter a folder name, not a path."));
        return;
    }

    if (!QDir(directory).mkdir(name)) {
        IconDialogs::MessageBox::warning(this, I18n::text("Create folder failed"),
                             I18n::text("Could not create the folder. It may already exist or the directory may be read-only."));
        return;
    }
    refreshDirectory(directory);
}



void ProjectExplorer::renamePath(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists()) {
        IconDialogs::MessageBox::warning(this, I18n::text("Rename failed"), I18n::text("The selected item no longer exists."));
        refreshDirectory(info.absolutePath());
        return;
    }

    const QString oldName = info.fileName();
    const QString newName = IconDialogs::InputDialog::getText(
        this, I18n::text("Rename"), I18n::text("New name:"), QLineEdit::Normal, oldName).trimmed();
    if (newName.isEmpty() || newName == oldName)
        return;
    if (QFileInfo(newName).fileName() != newName || newName == "." || newName == "..") {
        IconDialogs::MessageBox::warning(this, I18n::text("Invalid name"), I18n::text("Enter a name, not a path."));
        return;
    }

    const QString newPath = QDir(info.absolutePath()).filePath(newName);
    if (QFileInfo::exists(newPath)) {
        IconDialogs::MessageBox::warning(this, I18n::text("Rename failed"), I18n::text("An item with that name already exists."));
        return;
    }
    if (!QDir().rename(path, newPath)) {
        IconDialogs::MessageBox::warning(this, I18n::text("Rename failed"), I18n::text("Could not rename the selected item."));
        return;
    }

    const int rootIndex = roots_.indexOf(cleanPath(path));
    const QString normalizedOldPath = cleanPath(path);
    const QString normalizedNewPath = cleanPath(newPath);
    if (rootIndex >= 0) {
        roots_[rootIndex] = normalizedNewPath;
        setRoots(roots_);
        emit rootsChanged(roots_);
    } else {
        remapTreePaths(normalizedOldPath, normalizedNewPath);
        remapPathState(normalizedOldPath, normalizedNewPath);
        refreshDirectory(info.absolutePath());
    }
    emit pathRenamed(normalizedOldPath, normalizedNewPath);
}

void ProjectExplorer::deletePath(const QString& path) {
    const QString normalized = cleanPath(path);
    const int rootIndex = roots_.indexOf(normalized);
    if (rootIndex >= 0) {
        const auto answer = IconDialogs::MessageBox::question(
            this, I18n::text("Remove Folder"), I18n::text("Remove this folder from the workspace?\n\n")
            + normalized + I18n::text("\n\nFiles on disk will not be deleted."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        QStringList updatedRoots = roots_;
        updatedRoots.removeAt(rootIndex);
        setRoots(updatedRoots);
        emit rootsChanged(updatedRoots);
        return;
    }

    const QFileInfo info(normalized);
    const QString description = info.isDir() ? I18n::text("folder and all its contents") : I18n::text("file");
    const auto answer = IconDialogs::MessageBox::warning(
        this, I18n::text("Delete"), I18n::text("Permanently delete this ") + description + "?\n\n" + normalized,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    auto cancel=std::make_shared<std::atomic_bool>(false);
    auto* progress=new QProgressDialog(I18n::text("Deleting files…"),I18n::text("Cancel"),0,0,this);
    progress->setCancelButton(new QPushButton(Codicons::icon("close"),I18n::text("Cancel"),progress));
    progress->setWindowModality(Qt::WindowModal);progress->setMinimumDuration(0);
    connect(progress,&QProgressDialog::canceled,this,[cancel]{cancel->store(true);});
    connect(this,&QObject::destroyed,[cancel]{cancel->store(true);});
    const auto generation=generation_;
    auto* worker=new QFutureWatcher<bool>(this);
    connect(worker,&QFutureWatcher<bool>::finished,this,[this,worker,progress,normalized,info,cancel,generation] {
        const bool removed=worker->result();worker->deleteLater();progress->deleteLater();
        emit filesChanged();
        if(generation!=generation_)return;
        refreshDirectory(info.absolutePath());
        if(!removed) {
            IconDialogs::MessageBox::warning(this,I18n::text("Delete failed"),I18n::text(cancel->load()?"Deletion cancelled; some files may already have been deleted.":"Could not delete the selected item."));
            return;
        }

    const QString deletedPrefix = normalized + QDir::separator();
    pendingExpandedPaths_.erase(
        std::remove_if(pendingExpandedPaths_.begin(), pendingExpandedPaths_.end(),
            [&normalized, &deletedPrefix](const QString& pendingPath) {
                return pendingPath == normalized || pendingPath.startsWith(deletedPrefix);
            }), pendingExpandedPaths_.end());
    emit pathDeleted(normalized);
    refreshDirectory(info.absolutePath());
    });
    worker->setFuture(QtConcurrent::run([normalized,cancel] {
        struct Frame {QString path;std::unique_ptr<QDirIterator> entries;};
        std::vector<Frame> stack;bool success=true;
        const auto enter=[&](const QString& path) {
            const QFileInfo info(path);
            if(info.isSymLink() || !info.isDir()){if(!QFile::remove(path))success=false;return;}
            stack.push_back({path,std::make_unique<QDirIterator>(path,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System)});
        };
        enter(normalized);
        while(!stack.empty()) {
            if(cancel->load())return false;
            auto& frame=stack.back();
            if(frame.entries->hasNext()){const auto path=frame.entries->next();enter(path);}
            else {if(!QDir().rmdir(frame.path))success=false;stack.pop_back();}
        }
        return success;
    }));
}
