#include "VimDiffSession.hpp"
#include "EditorWidget.hpp"
#include "app/I18n.hpp"

#include <QScopedValueRollback>
#include <QScrollBar>
#include <QTimer>
#include <algorithm>
#include <stdexcept>

namespace {
constexpr std::size_t maxBytes = 8 * 1024 * 1024;
constexpr std::size_t maxLines = 30000;

int firstLine(const GitDiff::Hunk& hunk, int side) {
    return side == 0 ? hunk.left : hunk.right;
}

int lineCount(const GitDiff::Hunk& hunk, int side) {
    return side == 0 ? hunk.leftCount : hunk.rightCount;
}

bool fits(const EditorWidget* editor) {
    return editor->byteSize() <= maxBytes && editor->document()->buffer().lineCount() <= maxLines;
}

void hideDecoration(EditorWidget* editor) {
    if (!editor) return;
    editor->setDiffHighlights({});
    editor->setDiffAlignment({});
}
}

VimDiffSession::VimDiffSession(QObject* parent) : QObject(parent), refreshTimer_(new QTimer(this)) {
    setObjectName("vimDiffSession");
    refreshTimer_->setInterval(80);
    refreshTimer_->setSingleShot(true);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        const auto error = refresh();
        if (!error.isEmpty() && views_[0]) views_[0]->reportVimExMessage(error);
    });
}

int VimDiffSession::sideOf(const EditorWidget* editor) const {
    if (views_[0] == editor) return 0;
    return views_[1] == editor ? 1 : -1;
}

EditorWidget* VimDiffSession::otherView(const EditorWidget* editor) const {
    const int side = sideOf(editor);
    return side < 0 ? nullptr : views_[1 - side].data();
}

bool VimDiffSession::ready() const {
    return views_[0] && views_[1] && !views_[0]->isBusy() && !views_[1]->isBusy();
}

QString VimDiffSession::add(EditorWidget* editor) {
    if (!editor || editor->isBusy()) return I18n::text("Vim: the document is busy");
    if (sideOf(editor) >= 0) return refresh();
    if (views_[0] && views_[0]->document() == editor->document())
        return I18n::text("Vim: select two different buffers for diff");
    if (views_[1]) return I18n::text("Vim: this tab already compares two buffers");
    if (!fits(editor)) return I18n::text("Vim: the buffer is too large for diff");
    views_[views_[0] ? 1 : 0] = editor;
    connect(editor, &EditorWidget::contentChanged, refreshTimer_, qOverload<>(&QTimer::start));
    connect(editor->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this, editor](int row) { synchronizeScroll(editor, row); });
    return refresh();
}

void VimDiffSession::detach(EditorWidget* editor) {
    if (!editor) return;
    disconnect(editor, nullptr, refreshTimer_, nullptr);
    disconnect(editor->verticalScrollBar(), nullptr, this, nullptr);
    hideDecoration(editor);
}

void VimDiffSession::clearDecorations() {
    const QScopedValueRollback guard(syncing_, true);
    for (auto& view : views_) hideDecoration(view);
    model_ = {};
}

void VimDiffSession::remove(EditorWidget* editor) {
    const int side = sideOf(editor);
    if (side == -1) return;
    refreshTimer_->stop();
    clearDecorations();
    detach(views_[side]);
    if (side == 0) views_[0] = views_[1];
    views_[1].clear();
}

void VimDiffSession::clear() {
    if (views_[1]) remove(views_[1]);
    if (views_[0]) remove(views_[0]);
}

void VimDiffSession::synchronizeScroll(EditorWidget* source, int row) {
    const int side = sideOf(source);
    if (syncing_ || model_.rows.isEmpty() || side < 0 || !views_[1 - side]) return;
    const QScopedValueRollback guard(syncing_, true);
    views_[1 - side]->verticalScrollBar()->setValue(row);
}

QString VimDiffSession::refresh() {
    refreshTimer_->stop();
    if (!ready()) return {};
    if (!fits(views_[0]) || !fits(views_[1])) {
        clearDecorations();
        return I18n::text("Vim: the buffer is too large for diff");
    }
    try {
        const auto left = QByteArray::fromStdString(views_[0]->document()->buffer().text());
        const auto right = QByteArray::fromStdString(views_[1]->document()->buffer().text());
        model_ = GitDiff::compare(left, right);
    } catch (const std::exception& error) {
        clearDecorations();
        return QString::fromUtf8(error.what());
    }
    const QScopedValueRollback guard(syncing_, true);
    QVector<int> alignment[2];
    QVector<EditorWidget::DiffHighlight> highlights[2];
    for (const auto& row : model_.rows) {
        alignment[0].append(row.left);
        alignment[1].append(row.right);
        if (row.changed && row.left >= 0) highlights[0].append({row.left, 0, -1});
        if (row.changed && row.right >= 0) highlights[1].append({row.right, 0, -1});
    }
    const int realLines[] = {int(model_.left.size()), int(model_.right.size())};
    const bool extra[] = {views_[0]->document()->buffer().lineCount() > std::size_t(realLines[0]),
                          views_[1]->document()->buffer().lineCount() > std::size_t(realLines[1])};
    for (int side = 0; side < 2; ++side) {
        if (extra[0] || extra[1]) alignment[side].append(extra[side] ? realLines[side] : -1);
        views_[side]->setDiffAlignment(alignment[side]);
        views_[side]->setDiffHighlights(highlights[side]);
    }
    if (views_[0]->hasFocus()) views_[1]->verticalScrollBar()->setValue(views_[0]->verticalScrollBar()->value());
    else views_[0]->verticalScrollBar()->setValue(views_[1]->verticalScrollBar()->value());
    return {};
}

QString VimDiffSession::jump(EditorWidget* editor, int direction, int count) {
    const int side = sideOf(editor);
    if (side < 0 || !ready()) return QStringLiteral("E99: Current buffer is not in diff mode");
    if (refreshTimer_->isActive()) if (const auto error = refresh(); !error.isEmpty()) return error;
    const int row = editor->navigationPosition().x() - 1;
    const int finalLine = std::max(0, int((side == 0 ? model_.left : model_.right).size()) - 1);
    int target = -1;
    for (int index = direction > 0 ? 0 : int(model_.hunks.size()) - 1;
         index >= 0 && index < model_.hunks.size(); index += direction) {
        const auto& hunk = model_.hunks[index];
        const int line = std::min(finalLine, firstLine(hunk, side));
        if ((line - row) * direction <= 0) continue;
        target = line;
        if (--count == 0) break;
    }
    if (target < 0) return {};
    editor->recordVimJump();
    editor->setNavigationPosition(target + 1, 1);
    return {};
}

QString VimDiffSession::transfer(EditorWidget* editor, bool put, int first, int last, bool hasRange) {
    const int side = sideOf(editor);
    if (side < 0 || !ready()) return QStringLiteral("E99: Current buffer is not in diff mode");
    if (refreshTimer_->isActive()) if (const auto error = refresh(); !error.isEmpty()) return error;
    const int from = put ? side : 1 - side, to = 1 - from;
    auto* target = views_[to].data();
    const auto& fromLines = from == 0 ? model_.left : model_.right;
    const auto& activeLines = side == 0 ? model_.left : model_.right;
    const auto& toBuffer = target->document()->buffer();
    const auto byteAtLine = [&](int line) {
        return std::size_t(line) < toBuffer.lineCount() ? toBuffer.lineStart(line) : toBuffer.size();
    };
    std::vector<EditChange> changes;
    for (const auto& hunk : model_.hunks) {
        const int start = firstLine(hunk, side);
        const int lines = lineCount(hunk, side);
        int begin = 0;
        int end = std::max(hunk.leftCount, hunk.rightCount);
        if (!hasRange) {
            const int cursorStart = std::min(start, std::max(0, int(activeLines.size()) - 1));
            if (first < cursorStart || first > start + lines) continue;
        } else if (lines == 0) {
            if (first >= start || last < start) continue;
        } else {
            begin = std::max(0, first - start);
            if (begin >= lines || last < start) continue;
            if (last < start + lines - 1) end = last - start + 1;
        }
        const auto at = byteAtLine(firstLine(hunk, to) + std::min(begin, lineCount(hunk, to)));
        const auto endOffset = byteAtLine(firstLine(hunk, to) + std::min(end, lineCount(hunk, to)));
        QByteArray replacement;
        for (int i = begin; i < std::min(end, lineCount(hunk, from)); ++i)
            replacement += fromLines[firstLine(hunk, from) + i];
        if (at == endOffset && replacement.isEmpty()) continue;
        changes.push_back({at, toBuffer.slice(at, endOffset - at), replacement.toStdString()});
        if (!hasRange) break;
    }
    if (changes.empty()) return {};
    target->applyWorkspaceEdits(std::move(changes));
    return refresh();
}
