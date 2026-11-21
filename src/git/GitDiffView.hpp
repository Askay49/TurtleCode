#pragma once
#include "GitService.hpp"
#include "GitDiffModel.hpp"
#include <QAbstractScrollArea>
#include <QPointer>
#include <QWidget>
#include <functional>
class QLabel;
class QPlainTextEdit;
class QScrollBar;
class QTextEdit;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class GitDiffView final : public QWidget {
    Q_OBJECT
public:
    struct LiveDocument {
        QPointer<QAbstractScrollArea> editor;
        std::function<QByteArray()> read;
        std::function<bool()> available;
        std::function<bool(const QByteArray&, const QByteArray&)> replace;
        std::function<void(const GitDiff::Model*)> highlight;
        std::function<void(const LineAlignment&)> align;
        std::function<void(bool)> showOverview;
    };
    explicit GitDiffView(QWidget* parent = nullptr);
    void display(const QString& path, bool staged, const QString& text);
    void displayVersions(const GitDiffSnapshot& snapshot, LiveDocument live = {}, QWidget* editorHost = nullptr);
    void refreshVersions(const GitDiffSnapshot& snapshot);
    QString path() const { return path_; }
    bool staged() const { return staged_; }
    QAbstractScrollArea* nativeEditor() const { return live_.editor; }
    void setOperationBusy(bool busy);
    void documentChanged();
    void revertFile();
    void setVisible(bool visible) override;
signals:
    void visibilityChanged(bool visible);
    void refreshRequested();
    void fileReverted(const QString& path);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    class Overview;
    void rebuild();
    void render(GitDiff::Model model);
    void tryRevertFile();
    void pollDocument();
    void showPatch(const QString& patch);
    void updateTitles();
    void setSharedScrolling(bool enabled);
    void updatePaneMetrics();
    QScrollBar* scrollSource() const;
    void syncScroll();
    int editorLineForRow(int row) const;
    void scrollToRow(int row);
    void clearBinding();
    QString path_;
    bool staged_{false}, updating_{false}, syncing_{false}, busy_{false}, legacy_{true}, previewOnly_{false}, revertPending_{false}, sourceReady_{false}, documentDirty_{true};
    quint64 revision_{0}, modelRevision_{0};
    GitDiffSnapshot snapshot_;
    LiveDocument live_;
    GitDiff::Model model_;
    QByteArray working_;
    QPlainTextEdit* text_;
    QTextEdit *original_, *current_;
    QLabel *leftTitle_, *rightTitle_, *patchTitle_;
    QStackedWidget* stack_;
    QWidget *comparison_, *patch_;
    QVBoxLayout* rightBody_;
    QScrollBar* scroll_;
    Overview* overview_;
    QWidget* home_;
    QPointer<QWidget> editorHost_;
    QTimer* debounce_;
    QMetaObject::Connection scrollConnection_, rangeConnection_;
    Qt::ScrollBarPolicy editorVerticalPolicy_, editorHorizontalPolicy_;
    bool sharedScrolling_{false};
    int lineHeight_{1};
};
