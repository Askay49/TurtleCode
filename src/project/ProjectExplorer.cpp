#include "WorkspaceFiles.hpp"
#include "WorkspaceThreads.hpp"
#include <QElapsedTimer>
#include <QProgressDialog>
#include <QPushButton>
#include <QPointer>
#include <QDirIterator>
#include <atomic>
#include <utility>
#include <vector>
#include "app/IconDialogs.hpp"
#include "app/FileIcons.hpp"
#include "app/Codicons.hpp"
#include "app/I18n.hpp"
#include "app/FontSettings.hpp"
#include "ProjectExplorer.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QKeyEvent>
#include <QMenu>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOption>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

namespace {
constexpr qint64 directoryPageSize = 500;
constexpr int compactStartRole = Qt::UserRole + 2;
const QString projectDragMimeType = QStringLiteral("application/x-turtle-code-project-item");

class ProjectDragData final : public QMimeData {
public:
    ProjectDragData(const ProjectExplorer* explorer, quint64 generation, const QString& path)
        : explorer(explorer), generation(generation), path(path) {
        setData(projectDragMimeType, {});
    }

    const ProjectExplorer* const explorer;
    const quint64 generation;
    const QString path;
};

QString cleanPath(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString itemPath(const QTreeWidgetItem* item) {
    return item->data(0, Qt::UserRole).toString();
}

QString firstItemPath(const QTreeWidgetItem* item) {
    const QString first = item->data(0, compactStartRole).toString();
    return first.isEmpty() ? itemPath(item) : first;
}

bool itemContainsPath(const QTreeWidgetItem* item, const QString& path) {
    const QString end = itemPath(item);
    if (path == end) return true;
    const QString first = item->data(0, compactStartRole).toString();
    return !first.isEmpty() && path.size() >= first.size() && end.startsWith(path + '/');
}

QStringList itemPaths(const QTreeWidgetItem* item) {
    const QString first = firstItemPath(item);
    QString path = itemPath(item);
    if (path.isEmpty()) return {};
    QStringList paths{path};
    while (path != first) {
        path = QFileInfo(path).absolutePath();
        paths.append(path);
    }
    return paths;
}

void setItemPath(QTreeWidgetItem* item, const QString& first, const QString& path) {
    item->setData(0, Qt::UserRole, path);
    item->setData(0, compactStartRole, first == path ? QVariant{} : QVariant(first));
    const QFileInfo start(first);
    item->setText(0, first == path ? start.fileName() : QDir(start.absolutePath()).relativeFilePath(path));
    item->setToolTip(0, path);
}

bool matchesFileClipboardKey(const QKeyEvent* event) {
    return event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut) ||
           event->matches(QKeySequence::Paste);
}

bool isVimTreeKey(const QKeyEvent& event) {
    if (event.modifiers() == Qt::ShiftModifier)
        return event.key() == Qt::Key_A || event.key() == Qt::Key_D || event.key() == Qt::Key_G;
    if (event.modifiers() != Qt::NoModifier) return false;
    switch (event.key()) {
    case Qt::Key_A: case Qt::Key_D: case Qt::Key_G: case Qt::Key_H:
    case Qt::Key_J: case Qt::Key_K: case Qt::Key_L: case Qt::Key_R: case Qt::Key_Escape:
        return true;
    default:
        return false;
    }
}

bool removePermanently(const QString& path, const std::atomic_bool& cancel) {
    struct Frame {
        QString path;
        std::unique_ptr<QDirIterator> entries;
    };
    std::vector<Frame> stack;
    bool success = true;
    const auto enter = [&](const QString& entry) {
        const QFileInfo info(entry);
        if (info.isSymLink() || !info.isDir()) {
            if (!QFile::remove(entry)) success = false;
            return;
        }
        stack.push_back({entry, std::make_unique<QDirIterator>(entry,
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)});
    };
    if (cancel.load()) return false;
    enter(path);
    while (!stack.empty()) {
        if (cancel.load()) return false;
        auto& frame = stack.back();
        if (frame.entries->hasNext()) {
            enter(frame.entries->next());
        } else {
            if (!QDir().rmdir(frame.path)) success = false;
            stack.pop_back();
        }
    }
    return success;
}

QString compactEnd(const QString& first, WorkspaceFiles& filter,
                   const std::shared_ptr<std::atomic_bool>& cancel) {
    QString path = first;
    while (!cancel->load()) {
        QDirIterator entries(path, QDir::AllEntries | QDir::NoDotAndDotDot |
                                      QDir::Hidden | QDir::System | QDir::NoSymLinks);
        QString onlyDirectory;
        while (!cancel->load() && entries.hasNext()) {
            entries.next();
            const QFileInfo info = entries.fileInfo();
            if (!filter.accepts(info)) continue;
            if (!info.isDir() || !onlyDirectory.isEmpty()) return path;
            onlyDirectory = info.absoluteFilePath();
        }
        if (onlyDirectory.isEmpty() || cancel->load()) break;
        path = onlyDirectory;
    }
    return path;
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
    setDragDropMode(QAbstractItemView::DragDrop);
    setDragDropOverwriteMode(true);

    // Qt 6.2's built-in hover timer can also collapse expanded folders.
    dragExpandTimer_.setSingleShot(true);
    dragExpandTimer_.setInterval(650);
    connect(&dragExpandTimer_, &QTimer::timeout, this, [this] {
        const auto index = indexAt(dragPosition_);
        if (index.isValid() && dragExpandIndex_ == index) expand(index);
    });

    connect(this, &QWidget::customContextMenuRequested,
            this, &ProjectExplorer::showContextMenu);

    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int){
        pendingSelectionPath_.clear();
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
    connect(&fallbackTimer_, &QTimer::timeout, this, [this] {
        const auto paths = failedWatches_;
        for (const auto& path : paths) refreshDirectory(path);
    });
    fallbackTimer_.start();
}

QStringList ProjectExplorer::mimeTypes() const {
    return {projectDragMimeType};
}

QMimeData* ProjectExplorer::mimeData(const QList<QTreeWidgetItem*>& items) const {
    if (items.size() != 1 || !items.front()) return nullptr;
    const QString source = itemPath(items.front());
    if (copyingPath_ || !canCutOrDuplicate(source)) return nullptr;
    return new ProjectDragData(this, generation_, source);
}

void ProjectExplorer::startDrag(Qt::DropActions) {
    const auto items = selectedItems();
    auto* data = mimeData(items);
    if (!data) return;
    auto* drag = new QDrag(this);
    drag->setMimeData(data);
    drag->setPixmap(items.front()->icon(0).pixmap(iconSize()));
    // The filesystem transfer refreshes the tree after a successful drop.
    drag->exec(Qt::MoveAction);
}

QString ProjectExplorer::draggedPath(const QDropEvent& event) const {
    if (copyingPath_ || !(event.possibleActions() & Qt::MoveAction)) return {};
    const auto* data = dynamic_cast<const ProjectDragData*>(event.mimeData());
    return data && data->explorer == this && data->generation == generation_ ? data->path : QString{};
}

QString ProjectExplorer::dropDirectory(const QPoint& position, const QString& source) const {
    if (source.isEmpty() || !viewport()->rect().contains(position)) return {};
    const auto* item = itemAt(position);
    if (!item) item = topLevelItem(0);
    if (!item || !(item->flags() & Qt::ItemIsDropEnabled)) return {};
    const QString directory = itemPath(item);
    if (directory == QFileInfo(source).absolutePath() || directory == source || directory.startsWith(source + '/'))
        return {};
    return directory;
}

void ProjectExplorer::dragEnterEvent(QDragEnterEvent* event) {
    if (draggedPath(*event).isEmpty()) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::MoveAction);
    QTreeWidget::dragEnterEvent(event);
}

void ProjectExplorer::dragMoveEvent(QDragMoveEvent* event) {
    dragPosition_ = event->position().toPoint();
    const bool valid = !dropDirectory(dragPosition_, draggedPath(*event)).isEmpty();
    const auto index = indexAt(dragPosition_);
    const bool overEmptySpace = valid && !index.isValid();
    const QModelIndex rootDrop = overEmptySpace ? indexFromItem(topLevelItem(0)) : QModelIndex{};
    if (rootDropIndex_ != rootDrop) {
        rootDropIndex_ = rootDrop;
        viewport()->update();
    }
    setDropIndicatorShown(valid && !overEmptySpace);
    QTreeWidget::dragMoveEvent(event);
    if (overEmptySpace) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }
    const auto hover = valid && event->isAccepted() ? index : QModelIndex{};
    if (dragExpandIndex_ != hover) {
        dragExpandTimer_.stop();
        dragExpandIndex_ = hover;
        if (hover.isValid() && !isExpanded(hover)) dragExpandTimer_.start();
    }
    if (!valid) {
        event->ignore();
        viewport()->update();
    }
}

void ProjectExplorer::dragLeaveEvent(QDragLeaveEvent* event) {
    dragExpandTimer_.stop();
    dragExpandIndex_ = QModelIndex{};
    if (rootDropIndex_.isValid()) {
        rootDropIndex_ = QModelIndex{};
        viewport()->update();
    }
    QTreeWidget::dragLeaveEvent(event);
}

void ProjectExplorer::dropEvent(QDropEvent* event) {
    const QString source = draggedPath(*event);
    const auto position = event->position().toPoint();
    const QString directory = dropDirectory(position, source);
    const bool overEmptySpace = !itemAt(position);
    // Clear Qt's drop indicator and autoscroll before a conflict dialog can open.
    QDragLeaveEvent leave;
    dragLeaveEvent(&leave);
    if (directory.isEmpty() || !transferPath(source, directory, true)) {
        event->ignore();
        return;
    }
    if (overEmptySpace) {
        selectProjectRoot();
        setFocus(Qt::MouseFocusReason);
    }
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void ProjectExplorer::paintEvent(QPaintEvent* event) {
    QTreeWidget::paintEvent(event);
    if (!rootDropIndex_.isValid()) return;
    const auto indicator = visualRect(rootDropIndex_);
    if (!indicator.intersects(viewport()->rect())) return;
    QStyleOption option;
    option.initFrom(this);
    option.rect = indicator;
    QPainter painter(viewport());
    style()->drawPrimitive(QStyle::PE_IndicatorItemViewItemDrop, &option, &painter, this);
}

bool ProjectExplorer::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->keyCombination() == QKeyCombination(Qt::Key_F2) ||
            key->keyCombination() == QKeyCombination(Qt::Key_Delete) || matchesFileClipboardKey(key) ||
            (vimEnabled_ && isVimTreeKey(*key))) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::FocusOut) vimGPending_ = false;
    return QTreeWidget::event(event);
}

void ProjectExplorer::mousePressEvent(QMouseEvent* event) {
    vimGPending_ = false;
    QTreeWidget::mousePressEvent(event);
    if (event->button() == Qt::LeftButton && !itemAt(event->position().toPoint()))
        selectProjectRoot();
}

void ProjectExplorer::keyPressEvent(QKeyEvent* event) {
    pendingSelectionPath_.clear();
    if (vimEnabled_ && isVimTreeKey(*event)) {
        handleVimKey(*event);
        event->accept();
        return;
    }
    vimGPending_ = false;
    const int key = event->key();
    const auto modifiers = event->modifiers();

    if (matchesFileClipboardKey(event)) {
        if (!event->isAutoRepeat()) {
            if (event->matches(QKeySequence::Paste)) {
                pasteInto(creationDirectory());
            } else if (const auto* item = currentItem()) {
                copyPathToClipboard(itemPath(item), event->matches(QKeySequence::Cut));
            }
        }
        event->accept();
        return;
    }

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

    if ((key == Qt::Key_F2 || key == Qt::Key_Delete) && modifiers == Qt::NoModifier) {
        if (const auto* item = currentItem(); item && !event->isAutoRepeat()) {
            const auto path = itemPath(item);
            if (!path.isEmpty()) {
                if (key == Qt::Key_F2) renamePath(path);
                else if (item->isSelected()) deletePath(path, Removal::Trash);
            }
        }
        event->accept();
        return;
    }

    QTreeWidget::keyPressEvent(event);
}

void ProjectExplorer::handleVimKey(const QKeyEvent& event) {
    cancelAutomaticReveal();
    const bool shifted = event.modifiers() == Qt::ShiftModifier;
    if (event.key() == Qt::Key_G && !shifted && event.isAutoRepeat()) return;
    const bool wasGPending = std::exchange(vimGPending_, false);
    const auto move = [this](Qt::Key key) {
        QKeyEvent navigation(QEvent::KeyPress, key, Qt::NoModifier);
        QTreeWidget::keyPressEvent(&navigation);
    };
    auto* item = currentItem();
    const QString path = item ? itemPath(item) : QString{};

    switch (event.key()) {
    case Qt::Key_G:
        if (shifted) move(Qt::Key_End);
        else if (wasGPending) move(Qt::Key_Home);
        else vimGPending_ = true;
        break;
    case Qt::Key_Escape:
        if (!wasGPending) emit editorFocusRequested();
        break;
    case Qt::Key_J: move(Qt::Key_Down); break;
    case Qt::Key_K: move(Qt::Key_Up); break;
    case Qt::Key_H:
        if (item && item->isExpanded()) item->setExpanded(false);
        else if (item && item->parent()) setCurrentItem(item->parent());
        break;
    case Qt::Key_L:
        if (!item || event.isAutoRepeat() || activatePageItem(item)) break;
        if (QFileInfo(path).isDir()) {
            if (!item->isExpanded()) item->setExpanded(true);
            else if (auto* child = itemBelow(item); child && child->parent() == item) setCurrentItem(child);
        } else if (QFileInfo(path).isFile()) {
            emit fileActivated(path, true);
        }
        break;
    case Qt::Key_A:
        if (!event.isAutoRepeat()) {
            const QString directory = creationDirectory();
            if (directory.isEmpty()) break;
            if (shifted) createFolderIn(directory, true);
            else createFileIn(directory, true);
            setFocus(Qt::ShortcutFocusReason);
        }
        break;
    case Qt::Key_R:
        if (!event.isAutoRepeat() && !path.isEmpty()) {
            renamePath(path, true);
            setFocus(Qt::ShortcutFocusReason);
        }
        break;
    case Qt::Key_D:
        if (!event.isAutoRepeat() && item && item->isSelected() && !path.isEmpty() && !roots_.contains(path)) {
            deletePath(path, shifted ? Removal::Permanent : Removal::Trash, true);
            setFocus(Qt::ShortcutFocusReason);
        }
        break;
    }
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
    vimGPending_ = false;
    scanCancel_->store(true);scanCancel_=std::make_shared<std::atomic_bool>(false);
    ++generation_;
    clear();
    pathIndex_.clear();failedWatches_.clear();dirtyDirectories_.clear();listings_.clear();pageOffsets_.clear();
    roots_.clear();
    loaded_.clear();
    loading_.clear();
    reloadPending_.clear();
    pendingExpandedPaths_.clear();
    pendingSelectionPath_.clear();
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
    reloadDirectoryRows();
}

void ProjectExplorer::setIncludeIgnored(bool include) {
    if (includeIgnored_ == include) return;
    includeIgnored_ = include;
    reloadDirectoryRows();
}

void ProjectExplorer::setCompactFolders(bool compact) {
    if (compactFolders_ == compact) return;
    compactFolders_ = compact;
    if (!compact) {
        QStringList unusedWatches;
        for (const auto& path : watcher_.directories()) {
            if (!loaded_.contains(path) && !loading_.contains(path))
                unusedWatches.append(path);
        }
        if (!unusedWatches.isEmpty()) watcher_.removePaths(unusedWatches);
        failedWatches_.removeIf([this](const QString& path) {
            return !loaded_.contains(path) && !loading_.contains(path);
        });
    }
    reloadDirectoryRows();
}

void ProjectExplorer::reloadDirectoryRows() {
    // Reconcile existing rows in place to preserve expansion, selection and scroll position.
    scanCancel_->store(true);scanCancel_=std::make_shared<std::atomic_bool>(false);
    ++generation_;
    QSet<QString> directories;
    for (const auto& path : loaded_.keys()) directories.insert(path);
    for (const auto& path : loading_.keys()) directories.insert(path);
    loading_.clear();
    reloadPending_.clear();
    for (const QString& path : directories) {
        if (QTreeWidgetItem* item = findItem(path); item && itemPath(item) == path)
            loadDirectory(item, true);
    }
    continueReveal();
}

void ProjectExplorer::revealPath(const QString& inputPath, bool interactive) {
    pendingSelectionPath_.clear();
    const QString path = inputPath.isEmpty() ? QString{} : cleanPath(inputPath);
    if (!revealPaths_.isEmpty() && revealPaths_.back() == path) {
        interactiveReveal_ |= interactive;
        return;
    }

    revealPaths_.clear();
    recheckedRevealDirectories_.clear();
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

void ProjectExplorer::collapseFolders() {
    finishReveal(true);
    pendingExpandedPaths_.clear();
    pendingSelectionPath_.clear();
    auto* item = currentItem();
    while (item && item->parent())
        item = item->parent();
    collapseAll();
    if (item) setCurrentItem(item);
}

void ProjectExplorer::continueReveal() {
    if (revealPaths_.isEmpty()) return;
    auto* item = findItem(revealPaths_.front());
    if (!item) {
        finishReveal(false);
        return;
    }

    for (qsizetype i = 1; i < revealPaths_.size();) {
        const auto parentPath = itemPath(item);
        const auto& childPath = revealPaths_[i];
        if (loading_.value(parentPath)) return;

        QTreeWidgetItem* child = nullptr;
        if (loaded_.value(parentPath)) {
            for (int row = 0; row < item->childCount(); ++row) {
                auto* candidate = item->child(row);
                if (firstItemPath(candidate) == childPath) {
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
        // One displayed row can represent multiple successive path components.
        const auto end = revealPaths_.indexOf(itemPath(child), i);
        if (end < 0) {
            if (!itemContainsPath(child, revealPaths_.back())) {
                // A new sibling can break the chain before the watcher refresh arrives.
                if (recheckedRevealDirectories_.contains(parentPath)) {
                    finishReveal(false);
                } else {
                    recheckedRevealDirectories_.insert(parentPath);
                    loadDirectory(item, true, false, childPath);
                }
                return;
            }
            item = child;
            break;
        }
        item = child;
        i = end + 1;
    }

    finishReveal(true);
    pendingSelectionPath_.clear();
    setCurrentItem(item);
    scrollToItem(item);
}

void ProjectExplorer::finishReveal(bool found) {
    const bool reportFailure = !found && interactiveReveal_;
    revealPaths_.clear();
    recheckedRevealDirectories_.clear();
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
    item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
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
    const QString path = itemPath(item);
    if (path.isEmpty() || !QFileInfo(path).isDir()) {
        if (!revealChild.isEmpty()) finishReveal(false);
        return;
    }
    if (loading_.value(path, false) || (loaded_.value(path, false) && !force)) return;

    const quint64 load = ++nextLoad_;
    loading_[path] = load;
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
    connect(future, &QFutureWatcher<Page>::finished, this, [this, future, path, root, generation, load, revealChild] {
        const auto page = future->result();
        future->deleteLater();
        if (generation != generation_ || loading_.value(path) != load) return;
        QTreeWidgetItem* parent = findItem(path);
        if (!parent || itemPath(parent) != path) {
            loading_.remove(path);
            continueReveal();
            return;
        }
        if (!revealChild.isEmpty() && !revealPaths_.contains(revealChild)) {
            loading_.remove(path);
            if (reloadPending_.take(path) && parent->isExpanded()) loadDirectory(parent, true);
            continueReveal();
            return;
        }
        if (!page.error.isEmpty()) {
            loading_.remove(path);
            if (revealPaths_.contains(path)) finishReveal(false);
            setToolTip(page.error);
            emit backgroundError(page.error);
            return;
        }
        listings_[path] = page.listing;
        pageOffsets_[path] = page.offset;
        // Publish the completed load before model changes can trigger callbacks.
        // Keep loading_ set until reconciliation is done to prevent reentry.
        loaded_[path] = true;
        // Reconcile this directory in place instead of rebuilding its entire
        // child tree. Keeping unchanged QTreeWidgetItems alive preserves their
        // expanded state, current selection, scroll position and lazy-loaded
        // descendants. Rename remaps the existing item's path before refresh,
        // so the renamed item is matched by its new path and reused as well.
        struct Delivery {
            qsizetype next{0};
            QHash<QString, QTreeWidgetItem*> existing;
            int previous{-1};
        };
        auto state = std::make_shared<Delivery>();
        state->previous = parent->childCount() - 1;
        auto deliver = std::make_shared<std::function<void()>>();
        const std::weak_ptr<std::function<void()>> weak = deliver;
        *deliver = [this, path, root, generation, load, state, weak, page, revealChild] {
            if (generation != generation_ || loading_.value(path) != load) return;
            auto* parent = findItem(path);
            if (!parent || itemPath(parent) != path) return;
            QElapsedTimer budget;
            budget.start();
            const auto yieldIfNeeded = [&] {
                if (budget.elapsed() < 4) return false;
                if (auto next = weak.lock()) QTimer::singleShot(0, this, [next] { (*next)(); });
                return true;
            };
            while (state->previous >= 0) {
                auto* child = parent->child(state->previous--);
                const auto childPath = firstItemPath(child);
                if (childPath.isEmpty()) delete child;
                else state->existing.insert(childPath, child);
                if (yieldIfNeeded()) return;
            }
            while (state->next < page.entries.size()) {
                const Entry& entry = page.entries[state->next++];
                QTreeWidgetItem* child = state->existing.take(entry.path);
                const QString end = page.compactEnds.value(entry.path, entry.path);
                const auto indicator = entry.directory ? QTreeWidgetItem::ShowIndicator
                                                       : QTreeWidgetItem::DontShowIndicator;
                bool reloadExpanded = false;
                if (!child) {
                    child = new QTreeWidgetItem(parent);
                } else if (itemPath(child) != end || child->childIndicatorPolicy() != indicator) {
                    // The row keeps its position, but its children belong to the old endpoint.
                    reloadExpanded = entry.directory && child->isExpanded();
                    if (reloadExpanded) {
                        for (const auto& component : itemPaths(child)) {
                            if (!pendingExpandedPaths_.contains(component))
                                pendingExpandedPaths_.append(component);
                        }
                    }
                    if (auto* selected = currentItem(); selected && pendingSelectionPath_.isEmpty()) {
                        for (auto* ancestor = selected->parent(); ancestor; ancestor = ancestor->parent()) {
                            if (ancestor == child) {
                                pendingSelectionPath_ = itemPath(selected);
                                break;
                            }
                        }
                    }
                    forgetSubtree(child, entry.directory);
                    qDeleteAll(child->takeChildren());
                }

                setItemPath(child, entry.path, end);
                child->setData(0, Qt::UserRole + 1, root);
                child->setIcon(0, entry.directory ? Codicons::folderIcon() : FileIcons::icon(entry.path));
                child->setChildIndicatorPolicy(indicator);
                child->setFlags(child->flags().setFlag(Qt::ItemIsDropEnabled, entry.directory));
                indexItem(child);
                if (compactFolders_ && entry.directory) {
                    for (const auto& component : itemPaths(child))
                        watchDirectory(component);
                }
                if (reloadExpanded) loadDirectory(child);
                if (yieldIfNeeded()) return;
            }
            while (!state->existing.isEmpty()) {
                auto it = state->existing.begin();
                auto* stale = it.value();
                state->existing.erase(it);
                forgetSubtree(stale);
                delete stale;
                if (yieldIfNeeded()) return;
            }
            // Even an empty folder needs an indicator so it can be collapsed again.
            parent->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            if (page.offset > 0) {
                auto* previous = new QTreeWidgetItem(parent, {I18n::text("Previous page")});
                previous->setFlags(previous->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
                previous->setData(0, Qt::UserRole + 10, std::max<qint64>(0, page.offset - directoryPageSize));
                previous->setIcon(0, Codicons::icon("chevron-left"));
            }
            const qint64 nextOffset = page.offset + page.entries.size();
            if (nextOffset < page.listing->size()) {
                auto* next = new QTreeWidgetItem(parent, {I18n::text("Next page")});
                next->setFlags(next->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
                next->setData(0, Qt::UserRole + 10, nextOffset);
                next->setIcon(0, Codicons::icon("chevron-right"));
            }
            watchDirectory(path);
            loading_.remove(path);
            restoreExpandedPaths();
            if (reloadPending_.take(path)) {
                loaded_[path] = false;
                if (parent->isExpanded() || revealPaths_.contains(path)) loadDirectory(parent, true);
            } else if (page.targetMissing && revealPaths_.contains(revealChild)) {
                finishReveal(false);
            }
            continueReveal();
            if (!pendingSelectionPath_.isEmpty()) {
                if (auto* selected = findItem(pendingSelectionPath_)) {
                    pendingSelectionPath_.clear();
                    setCurrentItem(selected);
                }
            }
        };
        (*deliver)();
    });

    const bool showHidden = showHidden_;
    const bool includeIgnored = includeIgnored_;
    const bool compactFolders = compactFolders_;
    const auto cancel=scanCancel_;
    const auto cached = pageOnly ? listings_.value(path) : std::shared_ptr<DirectoryListing>{};
    const auto offset = pageOffsets_.value(path);
    future->setFuture(QtConcurrent::run(&WorkspaceThreads::indexing(),
                                      [path, root, showHidden, includeIgnored, compactFolders, cached, offset, cancel, revealChild] {
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
            if (compactFolders) {
                WorkspaceFiles filter(root, {showHidden, includeIgnored, {}});
                for (const auto& entry : std::as_const(page.entries)) {
                    if (cancel->load()) break;
                    if (!entry.directory) continue;
                    const QString end = compactEnd(entry.path, filter, cancel);
                    if (end != entry.path) page.compactEnds.insert(entry.path, end);
                }
            }
        } catch (const std::exception& error) {
            page.error = QString::fromUtf8(error.what());
        }
        return page;
    }));
}

void ProjectExplorer::indexItem(QTreeWidgetItem* item) {
    const QPersistentModelIndex index(indexFromItem(item));
    for (const auto& path : itemPaths(item))
        pathIndex_.insert(path, index);
}

QTreeWidgetItem* ProjectExplorer::findItem(const QString& path) const {
    const auto known=pathIndex_.constFind(path);
    if(known!=pathIndex_.cend() && known.value().isValid()) {
        auto* item=itemFromIndex(known.value());
        if (item && itemContainsPath(item, path)) return item;
    }
    QList<QTreeWidgetItem*> stack;
    for (int i = 0; i < topLevelItemCount(); ++i) stack.push_back(topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem* item = stack.takeLast();
        if (itemContainsPath(item, path)) {
            pathIndex_[path] = QPersistentModelIndex(indexFromItem(item));
            return item;
        }
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
    auto* item = findItem(path);
    if (!item) return;
    // A change anywhere in a compact row can change where its chain ends.
    if (compactFolders_ && item->parent() && item->childIndicatorPolicy() == QTreeWidgetItem::ShowIndicator)
        invalidateDirectory(item->parent());
    if (itemPath(item) == path)
        invalidateDirectory(item);
}

void ProjectExplorer::invalidateDirectory(QTreeWidgetItem* item) {
    const QString path = itemPath(item);
    loaded_[path] = false;
    if (loading_.value(path, false)) {
        reloadPending_[path] = true;
        return;
    }
    if (item->isExpanded())
        loadDirectory(item, true);
}

void ProjectExplorer::forgetSubtree(QTreeWidgetItem* item, bool keepPageOffsets) {
    QStringList removeWatches;
    const auto watched = watcher_.directories();
    QList<QTreeWidgetItem*> stack{item};
    while (!stack.isEmpty()) {
        auto* descendant = stack.takeLast();
        for (const auto& path : itemPaths(descendant)) {
            loaded_.remove(path);
            listings_.remove(path);
            if (!keepPageOffsets) pageOffsets_.remove(path);
            pathIndex_.remove(path);
            loading_.remove(path);
            reloadPending_.remove(path);
            failedWatches_.remove(path);
            dirtyDirectories_.remove(path);
            if (watched.contains(path)) removeWatches.append(path);
        }
        for (int i = 0; i < descendant->childCount(); ++i)
            stack.append(descendant->child(i));
    }
    if (!removeWatches.isEmpty()) watcher_.removePaths(removeWatches);
}

void ProjectExplorer::remapTreePaths(const QString& oldPath, const QString& newPath) {
    const QString oldPrefix = oldPath + QDir::separator();
    QList<QTreeWidgetItem*> stack;
    for (int i = 0; i < topLevelItemCount(); ++i)
        stack.push_back(topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem* item = stack.takeLast();
        const QString currentPath = itemPath(item);
        if (currentPath == oldPath || currentPath.startsWith(oldPrefix)) {
            const QString updatedPath = newPath + currentPath.mid(oldPath.size());
            QString first = firstItemPath(item);
            for (const auto& component : itemPaths(item))
                pathIndex_.remove(component);
            if (first == oldPath || first.startsWith(oldPrefix))
                first = newPath + first.mid(oldPath.size());
            setItemPath(item, first, updatedPath);
            indexItem(item);
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

    for (const auto& path : pageOffsets_.keys()) {
        const QString remapped = remapKey(path);
        if (path != remapped) {
            pageOffsets_.insert(remapped, pageOffsets_.take(path));
            listings_.remove(path);
        }
    }
    for (auto& path : pendingExpandedPaths_) path = remapKey(path);
    for (auto& path : revealPaths_) path = remapKey(path);
    pendingSelectionPath_ = remapKey(pendingSelectionPath_);

    // The old jobs still carry their original paths. Restart expanded rows at
    // the new paths so a rename during delivery cannot leave a partial page.
    QStringList restartLoads;
    for (auto it = loading_.begin(); it != loading_.end();) {
        const QString remapped = remapKey(it.key());
        if (remapped != it.key()) {
            loaded_[remapped] = false;
            restartLoads.append(remapped);
            it = loading_.erase(it);
        } else {
            ++it;
        }
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
    for (const QString& directory : newWatched)
        if (QFileInfo(directory).isDir()) watchDirectory(directory);
    for (const auto& path : failedWatches_.values()) {
        const QString remapped = remapKey(path);
        if (path != remapped) {
            failedWatches_.remove(path);
            watchDirectory(remapped);
        }
    }
    for (const auto& path : restartLoads) {
        if (auto* item = findItem(path); item && itemPath(item) == path && item->isExpanded())
            loadDirectory(item);
    }
}

void ProjectExplorer::showContextMenu(const QPoint& position) {
    pendingSelectionPath_.clear();
    QTreeWidgetItem* item = itemAt(position);
    if (item)
        setCurrentItem(item);
    else
        selectProjectRoot();

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
    QAction* fileManagerAction = nullptr;
    QAction* terminalAction = nullptr;
    if (item && selectedInfo.exists()) {
        menu.addSeparator();
        if (selectedInfo.isDir())
            searchAction = menu.addAction(Codicons::icon("search"), I18n::text("Search in This Folder"));
        fileManagerAction = menu.addAction(Codicons::icon("folder-opened"), I18n::text("Open in File Manager"));
        terminalAction = menu.addAction(Codicons::icon("terminal"), I18n::text("New Terminal Here"));
    }
    QAction* renameAction = nullptr;
    QAction* trashAction = nullptr;
    QAction* deleteAction = nullptr;
    QAction* copyPathAction = nullptr;
    QAction* copyRelativePathAction = nullptr;
    const QString selectedRoot = item
        ? item->data(0, Qt::UserRole + 1).toString() : QString();
    menu.addSeparator();
    QAction* cutAction = nullptr;
    QAction* copyAction = nullptr;
    QAction* duplicateAction = nullptr;
    if (item) {
        cutAction = menu.addAction(Codicons::icon("screen-cut"), I18n::text("Cut"));
        cutAction->setShortcut(QKeySequence::Cut);
        cutAction->setEnabled(canCutOrDuplicate(selectedPath));
        copyAction = menu.addAction(Codicons::icon("copy"), I18n::text("Copy"));
        copyAction->setShortcut(QKeySequence::Copy);
        copyAction->setEnabled(selectedInfo.exists() || selectedInfo.isSymLink());
    }
    auto* pasteAction = menu.addAction(Codicons::icon("clippy"), I18n::text("Paste"));
    pasteAction->setShortcut(QKeySequence::Paste);
    pasteAction->setEnabled(!copyingPath_ && !clipboardPath().isEmpty());
    if (item) {
        duplicateAction = menu.addAction(Codicons::icon("files"), I18n::text("Duplicate"));
        duplicateAction->setEnabled(!copyingPath_ && canCutOrDuplicate(selectedPath));
    }
    if (item) {
        menu.addSeparator();
        renameAction = menu.addAction(Codicons::icon("edit"), I18n::text("Rename..."));
        const bool isRoot = roots_.contains(selectedPath);
        trashAction = menu.addAction(Codicons::icon(isRoot ? "close" : "trash"),
            I18n::text(isRoot ? "Remove Folder from Workspace" : "Move to Trash"));
        trashAction->setShortcut(QKeySequence(Qt::Key_Delete));
        if (!isRoot)
            deleteAction = menu.addAction(Codicons::icon("trash"), I18n::text("Delete Permanently..."));
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
    } else if (fileManagerAction && chosen == fileManagerAction) {
        if (!QFileInfo(targetDirectory).isDir() || !QDesktopServices::openUrl(QUrl::fromLocalFile(targetDirectory)))
            emit backgroundError(I18n::text("Could not open %1 in the file manager.")
                                     .arg(QDir::toNativeSeparators(targetDirectory)));
    } else if (terminalAction && chosen == terminalAction) {
        emit newTerminalRequested(targetDirectory);
    } else if (cutAction && chosen == cutAction) {
        copyPathToClipboard(selectedPath, true);
    } else if (copyAction && chosen == copyAction) {
        copyPathToClipboard(selectedPath, false);
    } else if (chosen == pasteAction) {
        pasteInto(targetDirectory);
    } else if (duplicateAction && chosen == duplicateAction) {
        duplicatePath(selectedPath);
    } else if (renameAction && chosen == renameAction) {
        renamePath(selectedPath);
    } else if (trashAction && chosen == trashAction) {
        deletePath(selectedPath, Removal::Trash);
    } else if (deleteAction && chosen == deleteAction) {
        deletePath(selectedPath, Removal::Permanent);
    } else if (copyPathAction && chosen == copyPathAction) {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(selectedPath));
    } else if (copyRelativePathAction && chosen == copyRelativePathAction) {
        QApplication::clipboard()->setText(
            QDir::toNativeSeparators(QDir(selectedRoot).relativeFilePath(selectedPath)));
    }
}

void ProjectExplorer::selectProjectRoot() {
    if (auto* root = topLevelItem(0)) {
        pendingSelectionPath_.clear();
        cancelAutomaticReveal();
        setCurrentItem(root);
    }
}

QString ProjectExplorer::creationDirectory() const {
    for (auto* item = currentItem(); item; item = item->parent()) {
        const QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty()) {
            if (item->childIndicatorPolicy() == QTreeWidgetItem::ShowIndicator) return path;
            return QFileInfo(path).absolutePath();
        }
    }
    return roots_.value(0);
}

void ProjectExplorer::createFile() {
    const QString directory = creationDirectory();
    if (!directory.isEmpty()) createFileIn(directory);
}

void ProjectExplorer::createFolder() {
    const QString directory = creationDirectory();
    if (!directory.isEmpty()) createFolderIn(directory);
}

void ProjectExplorer::createFileIn(const QString& directory, bool selectInTree) {
    QString label = I18n::text("File name:");
    if (selectInTree) label += '\n' + QDir::toNativeSeparators(directory);
    const QString name = IconDialogs::InputDialog::getText(
        this, I18n::text("New File"), label);
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
    emit filesChanged();
    if (selectInTree) revealPath(path);
    else emit fileActivated(path, false);
}

void ProjectExplorer::createFolderIn(const QString& directory, bool selectInTree) {
    QString label = I18n::text("Folder name:");
    if (selectInTree) label += '\n' + QDir::toNativeSeparators(directory);
    const QString name = IconDialogs::InputDialog::getText(
        this, I18n::text("New Folder"), label);
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
    emit filesChanged();
    if (selectInTree) revealPath(QDir(directory).filePath(name));
}

void ProjectExplorer::renamePath(const QString& path, bool selectInTree) {
    const QFileInfo info(path);
    if (!info.exists()) {
        IconDialogs::MessageBox::warning(this, I18n::text("Rename failed"), I18n::text("The selected item no longer exists."));
        refreshDirectory(info.absolutePath());
        return;
    }

    const QString oldName = info.fileName();
    IconDialogs::InputDialog dialog(this);
    dialog.setWindowTitle(I18n::text("Rename"));
    dialog.setLabelText(I18n::text("New name:"));
    dialog.setTextValue(oldName);
    const auto extension = oldName.lastIndexOf('.');
    if (selectInTree && info.isFile() && extension > 0) {
        QTimer::singleShot(0, &dialog, [&dialog, extension] {
            if (auto* input = dialog.findChild<QLineEdit*>()) input->setSelection(0, int(extension));
        });
    }
    if (dialog.exec() != QDialog::Accepted) return;
    const QString newName = dialog.textValue().trimmed();
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
    if (selectInTree) revealPath(normalizedNewPath);
}

void ProjectExplorer::selectRemovalNeighbor(QTreeWidgetItem* item) {
    auto* parent = item->parent();
    if (!parent) return;
    const int row = parent->indexOfChild(item);
    auto* next = parent->child(row + 1);
    if (!next || itemPath(next).isEmpty()) next = parent->child(row - 1);
    if (!next || itemPath(next).isEmpty()) next = parent;
    setCurrentItem(next);
    scrollToItem(next);
}

void ProjectExplorer::deletePath(const QString& path, Removal removal, bool selectNeighbor) {
    if (removingPath_) return;
    const QString normalized = cleanPath(path);
    const auto generation = generation_;
    if (roots_.contains(normalized)) {
        const auto answer = IconDialogs::MessageBox::question(
            this, I18n::text("Remove Folder"), I18n::text("Remove this folder from the workspace?\n\n")
            + normalized + I18n::text("\n\nFiles on disk will not be deleted."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes || generation != generation_)
            return;
        QStringList updatedRoots = roots_;
        updatedRoots.removeOne(normalized);
        setRoots(updatedRoots);
        emit rootsChanged(updatedRoots);
        return;
    }

    const bool trash = removal == Removal::Trash;
    const QString errorTitle = I18n::text(trash ? "Move to Trash failed" : "Delete failed");
    const QFileInfo info(normalized);
    const QString parent = info.absolutePath();
    if (!info.exists() && !info.isSymLink()) {
        refreshDirectory(parent);
        IconDialogs::MessageBox::warning(this, errorTitle, I18n::text("The selected item no longer exists."));
        return;
    }
    if (!trash || confirmMoveToTrash_) {
        const bool directory = info.isDir() && !info.isSymLink();
        const QString prompt = trash
            ? I18n::text(directory ? "Move this folder and all its contents to the Trash?" : "Move this item to the Trash?")
            : I18n::text(directory ? "Permanently delete this folder and all its contents?" : "Permanently delete this item?");
        IconDialogs::MessageBox confirmation(trash ? QMessageBox::Question : QMessageBox::Warning,
            I18n::text(trash ? "Move to Trash" : "Delete Permanently"),
            prompt + "\n\n" + QDir::toNativeSeparators(normalized), QMessageBox::Yes | QMessageBox::No, this);
        confirmation.setTextFormat(Qt::PlainText);
        confirmation.setDefaultButton(trash ? QMessageBox::Yes : QMessageBox::No);
        if (trash) confirmation.defaultButton()->setFocus();
        confirmation.setEscapeButton(QMessageBox::No);
        if (confirmation.exec() != QMessageBox::Yes || generation != generation_ || removingPath_)
            return;
    }
    if (copyingPath_ || (pathBusyCheck_ && pathBusyCheck_(normalized))) {
        emit backgroundError(I18n::text(copyingPath_ ? "Wait for file copying to finish and retry."
                                                     : "Wait for file loading/saving to finish and retry."));
        return;
    }

    removingPath_ = true;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto* progress = new QProgressDialog(I18n::text(trash ? "Moving to Trash…" : "Deleting files…"),
                                         QString(), 0, 0, this);
    auto* worker = new QFutureWatcher<bool>(this);
    if (trash) {
        progress->setCancelButton(nullptr);
        progress->setWindowFlag(Qt::WindowCloseButtonHint, false);
    } else {
        progress->setCancelButton(new QPushButton(Codicons::icon("close"), I18n::text("Cancel"), progress));
        connect(progress, &QProgressDialog::canceled, worker, [cancel] { cancel->store(true); });
        connect(this, &QObject::destroyed, worker, [cancel] { cancel->store(true); });
    }
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    connect(worker, &QFutureWatcher<bool>::finished, this,
            [this, worker, progress, normalized, parent, cancel, generation, trash, errorTitle, selectNeighbor] {
        const bool removed = worker->result();
        worker->deleteLater();
        progress->hide();
        progress->deleteLater();
        removingPath_ = false;
        if (generation == generation_ && removed) {
            const QString deletedPrefix = normalized + '/';
            pendingExpandedPaths_.removeIf([&](const QString& path) {
                return path == normalized || path.startsWith(deletedPrefix);
            });
            if (auto* item = currentItem(); selectNeighbor && item && itemPath(item) == normalized)
                selectRemovalNeighbor(item);
        }
        // Open buffers can still refer to this path after the project changes.
        if (removed) emit pathDeleted(normalized);
        refreshDirectory(parent);
        emit filesChanged();
        if (!removed) {
            const QString message = trash ? I18n::text("Could not move the selected item to the system Trash.")
                : I18n::text(cancel->load() ? "Deletion cancelled; some files may already have been deleted."
                                           : "Could not delete the selected item.");
            IconDialogs::MessageBox::warning(this, errorTitle, message);
        }
        if (selectNeighbor && generation == generation_) setFocus(Qt::ShortcutFocusReason);
    });
    worker->setFuture(QtConcurrent::run([normalized, trash, cancel] {
        return trash ? QFile::moveToTrash(normalized) : removePermanently(normalized, *cancel);
    }));
}
