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
#include <QMenu>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QtConcurrent/QtConcurrentRun>

namespace {
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

    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        loadDirectory(item);
    });
    connect(this, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
        const QString path = item->data(0, Qt::UserRole).toString();
        if (QFileInfo(path).isFile())
            emit fileActivated(path);
    });
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
            [this](const QString& path) { refreshDirectory(path); });
}

void ProjectExplorer::setRoots(const QStringList& roots) {
    ++generation_;
    clear();
    roots_.clear();
    loaded_.clear();
    loading_.clear();
    reloadPending_.clear();
    pendingExpandedPaths_.clear();
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
    ++generation_;
    loading_.clear();
    reloadPending_.clear();
    const QStringList loadedDirectories = loaded_.keys();
    for (const QString& path : loadedDirectories) {
        if (QTreeWidgetItem* item = findItem(path))
            loadDirectory(item, true);
    }
}

void ProjectExplorer::addRoot(const QString& inputPath) {
    const QString path = cleanPath(inputPath);
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir() || roots_.contains(path))
        return;
    roots_.push_back(path);
    addRootItem(path);
    if (!watcher_.directories().contains(path)) watcher_.addPath(path);
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


void ProjectExplorer::loadDirectory(QTreeWidgetItem* item, bool force) {
    if (!item) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty() || !QFileInfo(path).isDir()) return;
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
    auto* future = new QFutureWatcher<Entries>(this);
    connect(future, &QFutureWatcher<Entries>::finished, this, [this, future, path, root, generation] {
        const Entries entries = future->result();
        future->deleteLater();
        if (generation != generation_) return;
        QTreeWidgetItem* parent = findItem(path);
        if (!parent) {
            loading_.remove(path);
            return;
        }
        // Publish the completed load before model changes can trigger callbacks.
        // Keep loading_ set until reconciliation is done to prevent reentry.
        loaded_[path] = true;
        // Reconcile this directory in place instead of rebuilding its entire
        // child tree. Keeping unchanged QTreeWidgetItems alive preserves their
        // expanded state, current selection, scroll position and lazy-loaded
        // descendants. Rename remaps the existing item's path before refresh,
        // so the renamed item is matched by its new path and reused as well.
        QHash<QString, QTreeWidgetItem*> existingChildren;
        for (int i = parent->childCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* child = parent->child(i);
            const QString childPath = child->data(0, Qt::UserRole).toString();
            if (childPath.isEmpty()) {
                delete parent->takeChild(i); // discard the lazy-load placeholder
            } else {
                existingChildren.insert(childPath, child);
            }
        }

        for (const Entry& entry : entries) {
            QTreeWidgetItem* child = existingChildren.take(entry.path);
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
        }

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
        if (!watcher_.directories().contains(path)) watcher_.addPath(path);
        loading_[path] = false;
        // Restore expanded descendants progressively as each lazy level loads.
        restoreExpandedPaths();
        if (reloadPending_.take(path) && parent->isExpanded()) loadDirectory(parent, true);
    });

    const bool showHidden = showHidden_;
    future->setFuture(QtConcurrent::run([path, showHidden] {
        Entries entries;
        QDir directory(path);
        QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
        if (showHidden) filters |= QDir::Hidden;
        const QFileInfoList infos = directory.entryInfoList(
            filters,
            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        entries.reserve(infos.size());
        for (const QFileInfo& info : infos) {
            entries.push_back({info.fileName(), info.absoluteFilePath(), info.isDir()});
        }
        return entries;
    }));
}

QTreeWidgetItem* ProjectExplorer::findItem(const QString& path) const {
    QList<QTreeWidgetItem*> stack;
    for (int i = 0; i < topLevelItemCount(); ++i) stack.push_back(topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem* item = stack.takeLast();
        if (item->data(0, Qt::UserRole).toString() == path) return item;
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
    emit fileActivated(path);
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

    bool removed = false;
    if (info.isSymLink() || info.isFile())
        removed = QFile::remove(normalized);
    else if (info.isDir())
        removed = QDir(normalized).removeRecursively();

    if (!removed) {
        IconDialogs::MessageBox::warning(this, I18n::text("Delete failed"), I18n::text("Could not delete the selected item."));
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
}
