#pragma once

#include <QPointer>
#include <QStackedWidget>
#include <functional>

class Document;
class EditorWidget;
class QSplitter;

class EditorPane final : public QStackedWidget {
public:
    EditorPane();
    EditorWidget* editor() const;
    EditorWidget* viewFor(const Document* document) const;
    QPointer<EditorWidget> alternate;
};

class EditorPaneWorkspace final : public QStackedWidget {
public:
    explicit EditorPaneWorkspace(QWidget* parent = nullptr);
    EditorPane* activePane() const;
    EditorWidget* activeEditor() const;
    EditorWidget* pageEditor(int index) const;
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

private:
    class Page;
    Page* page() const;
    QWidget* replaceInLayout(QWidget* before, QWidget* after);
    void detachPane(EditorPane* pane);
    void removePage(Page* page);
    QPointer<QWidget> previousPage_;
};
