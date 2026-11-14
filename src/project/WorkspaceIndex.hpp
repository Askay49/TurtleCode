#pragma once
#include <QObject>
#include <QTimer>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>
#include <functional>
#include "WorkspaceFiles.hpp"
struct IndexedFile { QString path, relative; int score{}; };
struct FileQueryResult { QVector<IndexedFile> files; QString error; bool indexing{}; qint64 indexedFiles{}; };
class WorkspaceIndex final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceIndex(QObject* parent=nullptr);
    ~WorkspaceIndex() override;
    void configure(const QStringList& roots, WorkspaceFileOptions options = {});
    void invalidate();
    void cancelQuery() { if(queryCancel_)queryCancel_->store(true); }
    void query(const QString& query, const QStringList& recent, const QStringList& open,
               QObject* receiver, std::function<void(FileQueryResult)> callback);
    static int score(const QString& text,const QString& query);
signals:
    void updated();
private:
    struct State;
    void refresh();
    std::shared_ptr<State> state_;
    std::shared_ptr<std::atomic_bool> queryCancel_;
    QTimer debounce_, poll_, progress_;
    quint64 deliveredProgress_{};
    bool busy_{false}, pending_{false};
};
