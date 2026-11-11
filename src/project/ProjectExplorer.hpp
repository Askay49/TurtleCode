#pragma once

#include <QTreeWidget>
#include <QStringList>
#include <QFileSystemWatcher>
#include <QHash>

class ProjectExplorer final : public QTreeWidget {
    Q_OBJECT
public:
    explicit ProjectExplorer(QWidget* parent = nullptr);
    void setRoots(const QStringList& roots);
    void setShowHidden(bool show);
    [[nodiscard]] QStringList roots() const { return roots_; }

signals:
    void fileActivated(const QString& path);
    void pathRenamed(const QString& oldPath, const QString& newPath);
    void pathDeleted(const QString& path);
    void rootsChanged(const QStringList& roots);

private:
    struct Entry {
        QString name;
        QString path;
        bool directory{false};
    };
    using Entries = QList<Entry>;

    void addRoot(const QString& root);
    void addRootItem(const QString& path);
    void loadDirectory(QTreeWidgetItem* item, bool force = false);
    void refreshDirectory(const QString& path);
    void remapPathState(const QString& oldPath, const QString& newPath);
    void remapTreePaths(const QString& oldPath, const QString& newPath);
    void restoreExpandedPaths();
    void showContextMenu(const QPoint& position);
    void createFileIn(const QString& directory);
    void createFolderIn(const QString& directory);
    void renamePath(const QString& path);
    void deletePath(const QString& path);
    [[nodiscard]] QTreeWidgetItem* findItem(const QString& path) const;

    QStringList roots_;
    QFileSystemWatcher watcher_;
    QHash<QString, bool> loaded_;
    QHash<QString, bool> loading_;
    QHash<QString, bool> reloadPending_;
    QStringList pendingExpandedPaths_;
    QString pendingSelectedPath_;
    quint64 generation_{0};
    bool showHidden_{false};
};
