#pragma once

#include "git/GitDiffModel.hpp"
#include <QObject>
#include <QPointer>

class EditorWidget;
class QTimer;

class VimDiffSession final : public QObject {
    Q_OBJECT
public:
    explicit VimDiffSession(QObject* parent);
    QString add(EditorWidget* editor);
    bool contains(const EditorWidget* editor) const { return sideOf(editor) >= 0; }
    EditorWidget* otherView(const EditorWidget* editor) const;
    void remove(EditorWidget* editor);
    void clear();
    QString refresh();
    QString jump(EditorWidget* editor, int direction, int count);
    QString transfer(EditorWidget* editor, bool put, int first, int last, bool hasRange);

private:
    void detach(EditorWidget* editor);
    void clearDecorations();
    void synchronizeScroll(EditorWidget* source, int row);
    int sideOf(const EditorWidget* editor) const;
    bool ready() const;

    QPointer<EditorWidget> views_[2];
    GitDiff::Model model_;
    QTimer* refreshTimer_;
    bool syncing_{false};
};
