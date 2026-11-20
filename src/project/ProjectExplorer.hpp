#pragma once

#include <QTreeWidget>
#include "DirectoryListing.hpp"
#include <QStringList>
#include <QFileSystemWatcher>
#include <QHash>
#include <QTimer>
#include <QSet>
#include <QPersistentModelIndex>
#include <functional>

class ProjectExplorer final : public QTreeWidget {
    Q_OBJECT
public:
    explicit ProjectExplorer(QWidget* parent = nullptr);
    ~ProjectExplorer() override { scanCancel_->store(true); }
    void setRoots(const QStringList& roots);
    void setShowHidden(bool show);
    void setIncludeIgnored(bool include);
    void setCompactFolders(bool compact);
    void setConfirmMoveToTrash(bool enabled) { confirmMoveToTrash_ = enabled; }
    void setVimEnabled(bool enabled) { vimEnabled_ = enabled; }
    void setPathBusyCheck(std::function<bool(const QString&)> check);
    void revealPath(const QString& path, bool interactive = false);
    void cancelAutomaticReveal();
    void createFile();
    void createFolder();
    void collapseFolders();
    [[nodiscard]] QStringList roots() const { return roots_; }

signals:
    void filesChanged();
    void backgroundError(const QString& error);
    void revealFailed();
    void fileClicked(const QString& path);
    void fileActivated(const QString& path, bool focusEditor);
    void editorFocusRequested();
    void searchInFolderRequested(const QString& directory);
    void newTerminalRequested(const QString& directory);
    void pathRenamed(const QString& oldPath, const QString& newPath);
    void pathDeleted(const QString& path);
    void rootsChanged(const QStringList& roots);

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    enum class Removal { Trash, Permanent };
    using Entry=DirectoryEntry;
    using Entries=QList<Entry>;
    struct Page {
        Entries entries;
        QHash<QString, QString> compactEnds;
        std::shared_ptr<DirectoryListing> listing;
        qint64 offset{};
        QString error;
        bool targetMissing{false};
    };
    std::shared_ptr<std::atomic_bool> scanCancel_{std::make_shared<std::atomic_bool>(false)};
    QHash<QString,std::shared_ptr<DirectoryListing>> listings_;
    QHash<QString,qint64> pageOffsets_;

    void addRoot(const QString& root);
    void addRootItem(const QString& path);
    void reloadDirectoryRows();
    bool activatePageItem(QTreeWidgetItem* item);
    void loadDirectory(QTreeWidgetItem* item, bool force = false, bool pageOnly = false,
                       const QString& revealChild = {});
    void continueReveal();
    void finishReveal(bool found);
    void refreshDirectory(const QString& path);
    void invalidateDirectory(QTreeWidgetItem* item);
    void forgetSubtree(QTreeWidgetItem* item, bool keepPageOffsets = false);
    void indexItem(QTreeWidgetItem* item);
    void remapPathState(const QString& oldPath, const QString& newPath);
    void remapTreePaths(const QString& oldPath, const QString& newPath);
    void restoreExpandedPaths();
    void showContextMenu(const QPoint& position);
    void selectProjectRoot();
    [[nodiscard]] QString creationDirectory() const;
    void createFileIn(const QString& directory);
    void createFolderIn(const QString& directory);
    [[nodiscard]] QString draggedPath(const QDropEvent& event) const;
    [[nodiscard]] QString dropDirectory(const QPoint& position, const QString& source) const;
    [[nodiscard]] bool canCutOrDuplicate(const QString& path) const;
    [[nodiscard]] static QString clipboardPath();
    void copyPathToClipboard(const QString& path, bool cut);
    void pasteInto(const QString& directory);
    void duplicatePath(const QString& path);
    bool transferPath(const QString& source, const QString& directory, bool move, bool duplicate = false);
    bool movePath(const QString& source, const QString& destination);
    void renamePath(const QString& path);
    void deletePath(const QString& path, Removal removal);
    [[nodiscard]] QTreeWidgetItem* findItem(const QString& path) const;

    void watchDirectory(const QString& path);
    QTimer refreshTimer_, fallbackTimer_;
    QTimer dragExpandTimer_;
    QPersistentModelIndex dragExpandIndex_;
    QPersistentModelIndex rootDropIndex_;
    QPoint dragPosition_;
    QSet<QString> dirtyDirectories_, failedWatches_;
    mutable QHash<QString,QPersistentModelIndex> pathIndex_;
    QStringList roots_;
    QFileSystemWatcher watcher_;
    std::function<bool(const QString&)> pathBusyCheck_;
    QHash<QString, bool> loaded_;
    QHash<QString, quint64> loading_;
    QHash<QString, bool> reloadPending_;
    QStringList pendingExpandedPaths_;
    QStringList revealPaths_;
    QSet<QString> recheckedRevealDirectories_;
    QString pendingSelectionPath_;
    bool interactiveReveal_{false};
    quint64 generation_{0};
    quint64 nextLoad_{0};
    bool showHidden_{false}, includeIgnored_{false};
    bool vimEnabled_{false};
    bool compactFolders_{false};
    bool confirmMoveToTrash_{true};
    bool copyingPath_{false};
    bool removingPath_{false};
};
