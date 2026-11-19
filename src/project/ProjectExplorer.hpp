#pragma once

#include <QTreeWidget>
#include "DirectoryListing.hpp"
#include <QStringList>
#include <QFileSystemWatcher>
#include <QHash>
#include <QTimer>
#include <QSet>
#include <QPersistentModelIndex>

class ProjectExplorer final : public QTreeWidget {
    Q_OBJECT
public:
    explicit ProjectExplorer(QWidget* parent = nullptr);
    ~ProjectExplorer() override { scanCancel_->store(true); }
    void setRoots(const QStringList& roots);
    void setShowHidden(bool show);
    void setIncludeIgnored(bool include);
    void setVimEnabled(bool enabled) { vimEnabled_ = enabled; }
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
    void searchInFolderRequested(const QString& directory);
    void newTerminalRequested(const QString& directory);
    void pathRenamed(const QString& oldPath, const QString& newPath);
    void pathDeleted(const QString& path);
    void rootsChanged(const QStringList& roots);

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    using Entry=DirectoryEntry;
    using Entries=QList<Entry>;
    struct Page {
        Entries entries;
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
    bool activatePageItem(QTreeWidgetItem* item);
    void loadDirectory(QTreeWidgetItem* item, bool force = false, bool pageOnly = false,
                       const QString& revealChild = {});
    void continueReveal();
    void finishReveal(bool found);
    void refreshDirectory(const QString& path);
    void remapPathState(const QString& oldPath, const QString& newPath);
    void remapTreePaths(const QString& oldPath, const QString& newPath);
    void restoreExpandedPaths();
    void showContextMenu(const QPoint& position);
    [[nodiscard]] QString creationDirectory() const;
    void createFileIn(const QString& directory);
    void createFolderIn(const QString& directory);
    void renamePath(const QString& path);
    void deletePath(const QString& path);
    [[nodiscard]] QTreeWidgetItem* findItem(const QString& path) const;

    void watchDirectory(const QString& path);
    QTimer refreshTimer_, fallbackTimer_;
    QSet<QString> dirtyDirectories_, failedWatches_;
    mutable QHash<QString,QPersistentModelIndex> pathIndex_;
    QStringList roots_;
    QFileSystemWatcher watcher_;
    QHash<QString, bool> loaded_;
    QHash<QString, bool> loading_;
    QHash<QString, bool> reloadPending_;
    QStringList pendingExpandedPaths_;
    QStringList revealPaths_;
    bool interactiveReveal_{false};
    quint64 generation_{0};
    bool showHidden_{false}, includeIgnored_{false};
    bool vimEnabled_{false};
};
