#pragma once

#include <QFrame>
#include <QPointer>
#include <QStackedWidget>
#include <functional>

class Document;
class EditorWidget;
class QSplitter;
class QToolButton;

class EditorPaneHeader final : public QFrame {
    Q_OBJECT
public:
    explicit EditorPaneHeader(QWidget* parent);
    void setDocumentTitle(const QString& title, const QString& path, bool modified);
    void setActive(bool active);
    void setCloseAction(bool enabled, bool closesPane);

signals:
    void buffersRequested(const QPoint& position);
    void actionsRequested(const QPoint& position);
    void closeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    class TitleButton;
    TitleButton* title_;
    QToolButton* close_;
    bool active_{false};
};

class EditorPane final : public QWidget {
public:
    EditorPane();
    EditorWidget* editor() const;
    EditorWidget* viewFor(const Document* document) const;
    bool containsView(const EditorWidget* view) const;
    EditorPaneHeader* header() const { return header_; }
    void showView(EditorWidget* view);
    void removeView(EditorWidget* view);
    QPointer<EditorWidget> alternate;

private:
    EditorPaneHeader* header_;
    QStackedWidget* views_;
};

class EditorPaneWorkspace final : public QStackedWidget {
    Q_OBJECT
public:
    explicit EditorPaneWorkspace(QWidget* parent = nullptr);
    EditorPane* activePane() const;
    EditorWidget* activeEditor() const;
    QList<EditorPane*> panes(bool allPages = false) const;
    QList<EditorPane*> panesOnPage(int page) const;
    EditorPane* paneFor(const EditorWidget* editor) const;
    void selectPane(EditorPane* pane);
    EditorPane* split(Qt::Orientation orientation);
    bool closePane(EditorPane* pane);
    void keepPane(EditorPane* pane);
    EditorPane* newPage(int after = -1);
    bool movePaneToNewPage();
    bool selectPage(int page);
    void movePage(int target);
    int previousPage() const;
    EditorPane* focusTarget(QChar direction, int count, bool explicitCount) const;
    void resizePane(Qt::Orientation orientation, int amount, bool absolute, int cellSize);
    void equalize();
    void rotate(int count, bool backwards);
    void exchange(int count, bool explicitCount);
    void moveToEdge(QChar direction);

signals:
    void paneCreated(EditorPane* pane);

private:
    class Page;
    Page* page() const;
    EditorPane* createPane();
    QWidget* replaceInLayout(QWidget* before, QWidget* after);
    void detachPane(EditorPane* pane);
    void removePage(Page* page);
    QPointer<QWidget> previousPage_;
};
