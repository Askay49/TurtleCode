#pragma once
#include "GitService.hpp"
#include "GitDiffModel.hpp"
#include <QAbstractScrollArea>
#include <QDockWidget>
#include <QPointer>
#include <functional>
class QPlainTextEdit;
class QTextEdit;
class QStackedWidget;
class QSplitter;
class QTimer;
class GitDiffView final : public QDockWidget {
    Q_OBJECT
public:
    struct LiveDocument {
        QPointer<QAbstractScrollArea> editor;
        std::function<QByteArray()> read;
        std::function<bool()> available;
        std::function<bool(const QByteArray&, const QByteArray&)> replace;
        std::function<void(const GitDiff::Model*)> highlight;
        std::function<void(const QVector<int>&)> align;
    };
    explicit GitDiffView(QWidget* parent = nullptr);
    void display(const QString& path, bool staged, const QString& text);
    void displayVersions(const GitDiffSnapshot& snapshot, LiveDocument live = {});
    void refreshVersions(const GitDiffSnapshot& snapshot);
    QString path() const { return path_; }
    bool staged() const { return staged_; }
    QAbstractScrollArea* nativeEditor() const { return live_.editor; }
    void setOperationBusy(bool busy);
    void documentChanged();
    void revertFile();
signals:
    void refreshRequested();
    void fileReverted(const QString& path);
protected:
    void hideEvent(QHideEvent* event) override;
private:
    void rebuild();
    void render();
    void tryRevertFile();
    void pollDocument();
    void syncFromEditor();
    void clearBinding();
    QString path_;
    bool staged_{false}, updating_{false}, syncing_{false}, busy_{false}, legacy_{true}, previewOnly_{false}, revertPending_{false}, sourceReady_{false}, documentDirty_{true};
    quint64 revision_{0}, modelRevision_{0};
    GitDiffSnapshot snapshot_;
    LiveDocument live_;
    GitDiff::Model model_;
    QVector<int> alignment_;
    QByteArray working_;
    QPlainTextEdit* text_;
    QTextEdit *original_, *current_;
    QStackedWidget* stack_;
    QSplitter* split_;
    QTimer* debounce_;
    QMetaObject::Connection scrollConnection_;
};
