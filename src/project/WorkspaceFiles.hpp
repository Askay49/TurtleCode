#pragma once
#include <QStringList>
#include <QHash>
#include <QFileInfo>
#include <QRegularExpression>
#include <atomic>
#include <functional>
#include <memory>

struct WorkspaceFileOptions {
    bool hidden{false};
    bool ignored{false};
    QStringList excludes;
};
// A traversal owns its rules; no GUI objects or global mutable caches on workers.
class WorkspaceFiles {
public:
    explicit WorkspaceFiles(QString root, WorkspaceFileOptions options = {});
    bool accepts(const QFileInfo& info);
    void clearRuleCache() { rules_.clear(); }
    void walk(const std::function<void(const QFileInfo&)>& file,
              const std::shared_ptr<std::atomic_bool>& cancel = {},
              const std::function<void(const QString&)>& directory = {});
private:
    struct Rule { QRegularExpression expression; bool negative{}, directory{}; };
    const QList<Rule>& rules(const QString& directory);
    void readRules(const QString& path, const QString& base, QList<Rule>& rules);
    std::shared_ptr<void> gitRepository_;
    QString gitRoot_;
    QString root_;
    WorkspaceFileOptions options_;
    QHash<QString, QList<Rule>> rules_;
};
