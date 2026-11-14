#pragma once
#include <QObject>
#include <QHash>
#include <QProcess>
#include <QStringList>
#include <QVector>
#include <functional>
#include <atomic>
#include <memory>

struct GitEntry {
    QString path;
    QString originalPath;
    char index{' '};
    char worktree{' '};
    bool operator==(const GitEntry&) const = default;
    bool conflict() const;
    bool untracked() const { return index == '?' && worktree == '?'; }
    bool staged() const { return !conflict() && !untracked() && index != ' '; }
    bool unstaged() const { return !conflict() && (untracked() || worktree != ' '); }
    QStringList paths() const;
};
struct GitStatus {
    bool operator==(const GitStatus&) const = default;
    bool valid{false};
    QString branch;
    QString currentBranch;
    QStringList branches;
    bool unborn{false};
    QVector<GitEntry> entries;
};

struct GitDiffSnapshot {
    QString repository;
    GitEntry entry;
    bool staged{false};
    QByteArray left, right, base, ours, theirs;
    QString patch;
    QByteArray mode{"100644"};
    bool special{false};
};
Q_DECLARE_METATYPE(GitDiffSnapshot)

// One asynchronous operation at a time. A workspace change invalidates results,
// but never interrupts an index write or commit already in progress.
class GitService final : public QObject {
    Q_OBJECT
public:
    explicit GitService(QObject* parent = nullptr);
    ~GitService() override;
    void setRoots(const QStringList& roots);
    QStringList repositories() const { return repositories_; }
    const GitStatus& status(const QString& repository) const;
    bool selectRepository(const QString& repository);
    void rescan();
    void discard(const GitEntry& entry);
    void fetch();
    void pull();
    void push();
    void switchBranch(const QString& branch);
    void refresh();
    void refreshBackground();
    void stage(const QStringList& paths);
    void unstage(const QStringList& paths);
    void commit(const QString& message);
    void diff(const GitEntry& entry, bool staged);
    void applySelection(const QString& repository, const QString& path,
                        const QByteArray& expectedIndex, const QByteArray& patch, bool indexExists = true);
    bool busy() const { return busy_; }
    bool writing() const { return writing_; }
    QString repository() const { return repository_; }
    const GitStatus& status() const { return status_; }
    static bool parseStatus(const QByteArray& bytes, GitStatus& status);
signals:
    void changed();
    void busyChanged(bool busy);
    void error(const QString& message);
    void diffReady(const QString& path, bool staged, const QString& text);
    void versionsReady(const GitDiffSnapshot& snapshot);
    void committed();
    void commitFailed(const QString& message);
    void branchSwitchFinished(const QString& repository, bool success);
private:
    using Completion = std::function<void(int, const QByteArray&, const QString&)>;
    void run(const QString& directory, const QStringList& args, Completion completion,
             const QByteArray& input = {});
    bool begin();
    void finish();
    void discover(quint64 generation);
    void readStatus(quint64 generation, bool all = true);
    QHash<QString,qint64> lastStatusRead_;
    std::shared_ptr<std::atomic_bool> discoveryCancel_;
    void mutate(const QStringList& args, const QByteArray& input = {}, bool isCommit = false);
    QStringList roots_;
    QString repository_;
    QStringList repositories_;
    QHash<QString, GitStatus> statuses_;
    GitStatus status_;
    QProcess* process_{nullptr};
    quint64 generation_{0};
    bool writing_{false};
    bool busy_{false}, pendingRefresh_{false}, rediscover_{true};
};
