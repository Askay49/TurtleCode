#include "GitDiffSourcePane.hpp"
#include "app/ThemeManager.hpp"
#include "editor/EditorLineRendering.hpp"
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextBoundaryFinder>
#include <limits>

namespace {
constexpr int overscan = 2;
const QColor removed(190, 55, 65, 50);
const QColor changed(220, 55, 65, 110);

QString decode(QByteArrayView bytes) { return QString::fromUtf8(bytes.data(), bytes.size()); }
int cellWidth(std::size_t columns, int cell) {
    return int(std::min(columns * cell, std::size_t(std::numeric_limits<int>::max() / 2)));
}
}

GitDiffSourcePane::GitDiffSourcePane(QWidget* parent) : QAbstractScrollArea(parent) {
    setObjectName("gitDiffOld");
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    viewport()->setCursor(Qt::IBeamCursor);
    dragScroll_.setInterval(40);
    connect(&dragScroll_, &QTimer::timeout, this, &GitDiffSourcePane::updateDragSelection);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, viewport(), qOverload<>(&QWidget::update));
}

void GitDiffSourcePane::setComparison(const QByteArray& source, const GitDiff::Model& model) {
    if (source_ != source) {
        source_ = source;
        buffer_.load({source_.constData(), std::size_t(source_.size())});
        anchor_ = position_ = 0;
        resetLayouts();
    }
    alignment_ = model.alignment.left;
    hunks_ = model.hunks;
    rightLines_ = model.right;
    viewport()->update();
}

void GitDiffSourcePane::clear() {
    dragging_ = false;
    dragScroll_.stop();
    setComparison({}, {});
    verticalScrollBar()->setValue(0);
}

void GitDiffSourcePane::insertPadding(int row, int count) {
    alignment_.insertPadding(row, count);
    viewport()->update();
}

void GitDiffSourcePane::setRowMetrics(int height, int top, int inset, int tabSize) {
    rowHeight_ = height;
    topMargin_ = top;
    textInset_ = inset;
    if (tabSize_ != tabSize) {
        tabSize_ = tabSize;
        resetLayouts();
    }
    updateHorizontalRange();
    viewport()->update();
}

QRect GitDiffSourcePane::rowRect(int row) const {
    const int y = topMargin_ + row * rowHeight_ - verticalScrollBar()->value();
    return {0, y, viewport()->width(), rowHeight_};
}

std::size_t GitDiffSourcePane::lineEnd(int line) const {
    const auto end = buffer_.lineEnd(line);
    return end < buffer_.size() && end > 0 && source_.at(qsizetype(end - 1)) == '\r' ? end - 1 : end;
}

QTextLayout& GitDiffSourcePane::layoutForLine(int line, std::size_t begin, std::size_t end) {
    if (auto* existing = layouts_.object(line)) return *existing;
    auto* layout = new QTextLayout(decode({source_.constData() + begin, qsizetype(end - begin)}), font(), viewport());
    QTextOption options;
    options.setWrapMode(QTextOption::NoWrap);
    options.setFlags(QTextOption::IncludeTrailingSpaces);
    options.setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' ')) * tabSize_);
    layout->setTextOption(options);
    layout->setCacheEnabled(true);
    layout->beginLayout();
    layout->createLine();
    layout->endLayout();
    // Bound glyph storage as well as the number of cached rows.
    layouts_.insert(line, layout, std::max(512, int(layout->text().size())));
    return *layout;
}

void GitDiffSourcePane::paintEvent(QPaintEvent*) {
    using namespace EditorLineRendering;
    const auto& theme = ThemeManager::instance();
    QPainter painter(viewport());
    painter.setFont(font());
    painter.fillRect(viewport()->rect(), theme.color("editor.background"));
    painter.fillRect(0, 0, gutter_, viewport()->height(), theme.color("editor.gutterBackground"));
    const int scroll = verticalScrollBar()->value();
    const int first = std::max(0, (scroll - topMargin_) / rowHeight_ - overscan);
    const int last = std::min(alignment_.rowCount(), (scroll + viewport()->height() - topMargin_) / rowHeight_ + 1 + overscan);
    const int horizontal = horizontalScrollBar()->value();
    const int x = gutter_ + textInset_ - horizontal;
    const auto metrics = fontMetrics();
    const int cell = std::max(1, metrics.horizontalAdvance('M'));
    const auto [selectionBegin, selectionEnd] = std::minmax(anchor_, position_);
    const auto selectionColor = theme.color("editor.selection");
    const auto foreground = theme.color("text.primary");
    const auto numberColor = theme.color("editor.lineNumber");
    auto hunk = std::lower_bound(hunks_.cbegin(), hunks_.cend(), alignment_.lineAtOrAfter(first),
        [](const GitDiff::Hunk& change, int line) { return change.left + change.leftCount <= line; });

    for (int row = first; row < last; ++row) {
        const int line = alignment_.lineAt(row);
        const int y = rowRect(row).top();
        const QRect textArea(gutter_, y, viewport()->width() - gutter_, rowHeight_);
        if (line < 0) {
            painter.fillRect(textArea, alignmentGap());
            continue;
        }
        while (hunk != hunks_.cend() && hunk->left + hunk->leftCount <= line) ++hunk;
        const int offset = hunk != hunks_.cend() ? line - hunk->left : -1;
        const bool modified = offset >= 0 && offset < hunk->leftCount;
        if (modified) painter.fillRect(textArea, removed);
        painter.setPen(numberColor);
        drawLineNumber(painter, gutter_, y, rowHeight_, line);
        painter.save();
        painter.setClipRect(textArea, Qt::IntersectClip);
        painter.setPen(foreground);
        const auto begin = buffer_.lineStart(line), end = lineEnd(line);
        const int textBaseline = y + baseline(metrics, rowHeight_);
        if (end - begin > longLineBytes) {
            // The editor uses fixed cells on long lines so horizontal scrolling
            // only needs the visible slice of the buffer.
            const auto base = buffer_.codePointsBefore(begin);
            const auto count = buffer_.codePointsBefore(end) - base;
            widestLine_ = std::max(widestLine_, cellWidth(count, cell));
            const auto from = std::min(count, std::size_t(horizontal / cell));
            const auto to = std::min(count, from + std::size_t(viewport()->width() / cell + overscan));
            const auto firstByte = std::min(end, buffer_.byteAtCodePoint(base + from));
            const auto lastByte = std::min(end, buffer_.byteAtCodePoint(base + to));
            auto chars = decode({source_.constData() + firstByte, qsizetype(lastByte - firstByte)}).toUcs4();
            const auto selectedFrom = buffer_.codePointsBefore(std::clamp(selectionBegin, firstByte, lastByte)) - base;
            const auto selectedTo = buffer_.codePointsBefore(std::clamp(selectionEnd, firstByte, lastByte)) - base;
            for (qsizetype i = 0; i < chars.size(); ++i) {
                const auto column = from + std::size_t(i);
                const int glyphX = x + cellWidth(column, cell);
                if (column >= selectedFrom && column < selectedTo)
                    painter.fillRect(glyphX, y, cell, rowHeight_, selectionColor);
                const auto character = static_cast<char32_t>(chars[i]);
                painter.drawText(QPoint(glyphX, textBaseline), QString::fromUcs4(&character, 1));
            }
        } else {
            auto& layout = layoutForLine(line, begin, end);
            const auto textLine = layout.lineAt(0);
            const auto text = layout.text();
            widestLine_ = std::max(widestLine_, qCeil(textLine.naturalTextWidth()));
            QList<QTextLayout::FormatRange> formats;
            if (modified && offset < hunk->rightCount) {
                const auto& other = rightLines_.at(hunk->right + offset);
                if (other.size() <= qsizetype(longLineBytes + 2)) {
                    const auto [prefix, suffix] = GitDiff::commonColumns(text, GitDiff::displayLine(other));
                    const int length = int(text.size()) - prefix - suffix;
                    if (length > 0) {
                        QTextCharFormat format;
                        format.setBackground(changed);
                        formats.append({prefix, length, format});
                    }
                }
            }
            if (selectionBegin < end && selectionEnd > begin) {
                const auto firstUnit = buffer_.utf16Before(std::max(begin, selectionBegin)) - buffer_.utf16Before(begin);
                const auto lastUnit = buffer_.utf16Before(std::min(end, selectionEnd)) - buffer_.utf16Before(begin);
                QTextCharFormat format;
                format.setBackground(selectionColor);
                formats.append({int(firstUnit), int(lastUnit - firstUnit), format});
            }
            layout.draw(&painter, QPointF(x, textBaseline - textLine.ascent()), formats);
            if (selectionBegin <= end && selectionEnd > end && end < buffer_.size()) {
                const qreal eol = x + textLine.cursorToX(int(text.size()));
                painter.fillRect(QRectF(eol, y, metrics.horizontalAdvance(' '), rowHeight_), selectionColor);
            }
        }
        painter.restore();
    }
    updateHorizontalRange();
}

void GitDiffSourcePane::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateHorizontalRange();
}

void GitDiffSourcePane::hideEvent(QHideEvent* event) {
    dragging_ = false;
    dragScroll_.stop();
    QAbstractScrollArea::hideEvent(event);
}

void GitDiffSourcePane::changeEvent(QEvent* event) {
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) resetLayouts();
}

void GitDiffSourcePane::scrollContentsBy(int, int) { viewport()->update(); }

void GitDiffSourcePane::resetLayouts() {
    layouts_.clear();
    widestLine_ = 0;
    updateHorizontalRange();
}

void GitDiffSourcePane::updateHorizontalRange() {
    gutter_ = EditorLineRendering::gutterWidth(fontMetrics(), buffer_.lineCount());
    const int page = std::max(1, viewport()->width() - gutter_ - textInset_);
    auto* bar = horizontalScrollBar();
    bar->setPageStep(page);
    bar->setSingleStep(std::max(8, fontMetrics().horizontalAdvance(' ') * 4));
    bar->setRange(0, std::max(0, widestLine_ - page));
}

std::size_t GitDiffSourcePane::positionAtColumn(int line, qreal x) {
    const auto begin = buffer_.lineStart(line), end = lineEnd(line);
    if (end - begin > EditorLineRendering::longLineBytes) {
        const int cell = std::max(1, fontMetrics().horizontalAdvance('M'));
        const auto point = buffer_.codePointsBefore(begin) + std::size_t(std::max(0, qRound(x / cell)));
        return std::min(end, buffer_.byteAtCodePoint(point));
    }
    auto& layout = layoutForLine(line, begin, end);
    const int column = layout.lineAt(0).xToCursor(x);
    return begin + std::size_t(layout.text().left(column).toUtf8().size());
}

qreal GitDiffSourcePane::columnAtPosition(int line, std::size_t position) {
    const auto begin = buffer_.lineStart(line), end = lineEnd(line);
    if (end - begin > EditorLineRendering::longLineBytes) {
        const int cell = std::max(1, fontMetrics().horizontalAdvance('M'));
        return cellWidth(buffer_.codePointsBefore(position) - buffer_.codePointsBefore(begin), cell);
    }
    auto& layout = layoutForLine(line, begin, end);
    return layout.lineAt(0).cursorToX(int(buffer_.utf16Before(position) - buffer_.utf16Before(begin)));
}

std::size_t GitDiffSourcePane::positionAt(const QPoint& point) {
    const int y = point.y() + verticalScrollBar()->value() - topMargin_;
    if (y < 0) return 0;
    const int row = y / rowHeight_;
    const int line = alignment_.lineAt(row);
    if (line < 0) return buffer_.lineStart(alignment_.lineAtOrAfter(row));
    const int x = point.x() - gutter_ - textInset_ + horizontalScrollBar()->value();
    return positionAtColumn(line, x);
}

void GitDiffSourcePane::revealPosition() {
    const int line = int(buffer_.lineIndexAt(position_));
    const int row = alignment_.rowForLine(line);
    if (row < 0) return;
    auto* vertical = verticalScrollBar();
    const auto bounds = rowRect(row);
    if (bounds.top() < 0) vertical->setValue(row * rowHeight_);
    else if (bounds.bottom() >= viewport()->height())
        vertical->setValue((row - std::max(1, (viewport()->height() - topMargin_) / rowHeight_) + 1) * rowHeight_);
    auto* horizontal = horizontalScrollBar();
    const int x = qCeil(columnAtPosition(line, position_));
    if (x < horizontal->value()) horizontal->setValue(x);
    else if (x > horizontal->value() + horizontal->pageStep()) {
        widestLine_ = std::max(widestLine_, x);
        updateHorizontalRange();
        horizontal->setValue(x - horizontal->pageStep());
    }
}

void GitDiffSourcePane::selectAll() {
    anchor_ = 0;
    position_ = buffer_.size();
    viewport()->update();
}

void GitDiffSourcePane::copy() const {
    const auto [begin, end] = std::minmax(anchor_, position_);
    if (begin == end) return;
    auto text = decode({source_.constData() + begin, qsizetype(end - begin)});
    text.replace("\r\n", "\n");
    QGuiApplication::clipboard()->setText(text);
}

void GitDiffSourcePane::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    auto* copyAction = menu.addAction(QCoreApplication::translate("QWidgetTextControl", "&Copy"));
    copyAction->setShortcut(QKeySequence::Copy);
    connect(copyAction, &QAction::triggered, this, &GitDiffSourcePane::copy);
    copyAction->setEnabled(anchor_ != position_);
    auto* selectAction = menu.addAction(QCoreApplication::translate("QWidgetTextControl", "Select All"));
    selectAction->setShortcut(QKeySequence::SelectAll);
    connect(selectAction, &QAction::triggered, this, &GitDiffSourcePane::selectAll);
    selectAction->setEnabled(!source_.isEmpty());
    menu.exec(event->globalPos());
}

void GitDiffSourcePane::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy)) { copy(); return; }
    if (event->matches(QKeySequence::SelectAll)) { selectAll(); return; }
    if (alignment_.isEmpty()) { QAbstractScrollArea::keyPressEvent(event); return; }
    const bool selecting = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    const int line = int(buffer_.lineIndexAt(position_));
    const auto begin = buffer_.lineStart(line), end = lineEnd(line);
    switch (event->key()) {
    case Qt::Key_Home: position_ = control ? 0 : begin; break;
    case Qt::Key_End: position_ = control ? buffer_.size() : end; break;
    case Qt::Key_Left:
    case Qt::Key_Right: {
        const bool backward = event->key() == Qt::Key_Left;
        if (!selecting && anchor_ != position_ && !control)
            position_ = backward ? std::min(anchor_, position_) : std::max(anchor_, position_);
        else if (backward && position_ == begin && line > 0) position_ = lineEnd(line - 1);
        else if (!backward && position_ == end) position_ = buffer_.lineStart(line + 1);
        else if (end - begin > EditorLineRendering::longLineBytes) {
            const auto point = buffer_.codePointsBefore(position_);
            position_ = buffer_.byteAtCodePoint(backward ? std::max<std::size_t>(1, point) - 1 : point + 1);
        } else {
            auto& layout = layoutForLine(line, begin, end);
            const int column = int(buffer_.utf16Before(position_) - buffer_.utf16Before(begin));
            const auto mode = control ? QTextLayout::SkipWords : QTextLayout::SkipCharacters;
            const int next = backward ? layout.previousCursorPosition(column, mode) : layout.nextCursorPosition(column, mode);
            position_ = begin + std::size_t(layout.text().left(next).toUtf8().size());
        }
        break;
    }
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        const bool page = event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown;
        const bool backward = event->key() == Qt::Key_Up || event->key() == Qt::Key_PageUp;
        if (page && !selecting) { QAbstractScrollArea::keyPressEvent(event); return; }
        const int step = page ? std::max(1, viewport()->height() / rowHeight_) : 1;
        const int target = std::clamp(line + (backward ? -step : step), 0, alignment_.lineCount() - 1);
        position_ = positionAtColumn(target, columnAtPosition(line, position_));
        break;
    }
    default: QAbstractScrollArea::keyPressEvent(event); return;
    }
    if (!selecting) anchor_ = position_;
    revealPosition();
    viewport()->update();
}

void GitDiffSourcePane::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) { QAbstractScrollArea::mousePressEvent(event); return; }
    setFocus(Qt::MouseFocusReason);
    dragPoint_ = event->position().toPoint();
    position_ = positionAt(dragPoint_);
    if (!event->modifiers().testFlag(Qt::ShiftModifier)) anchor_ = position_;
    dragging_ = true;
    viewport()->update();
    event->accept();
}

void GitDiffSourcePane::updateDragSelection() {
    const auto area = viewport()->rect();
    auto* vertical = verticalScrollBar();
    if (dragPoint_.y() < 0) vertical->setValue(vertical->value() - rowHeight_);
    else if (dragPoint_.y() > area.bottom()) vertical->setValue(vertical->value() + rowHeight_);
    auto* horizontal = horizontalScrollBar();
    if (dragPoint_.x() < 0) horizontal->setValue(horizontal->value() - horizontal->singleStep());
    else if (dragPoint_.x() > area.right()) horizontal->setValue(horizontal->value() + horizontal->singleStep());
    const QPoint point(std::clamp(dragPoint_.x(), 0, std::max(0, area.right())),
                       std::clamp(dragPoint_.y(), 0, std::max(0, area.bottom())));
    position_ = positionAt(point);
    viewport()->update();
}

void GitDiffSourcePane::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_ || !event->buttons().testFlag(Qt::LeftButton)) return;
    dragPoint_ = event->position().toPoint();
    if (viewport()->rect().contains(dragPoint_)) dragScroll_.stop();
    else if (!dragScroll_.isActive()) dragScroll_.start();
    updateDragSelection();
    event->accept();
}

void GitDiffSourcePane::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragging_ || event->button() != Qt::LeftButton) return;
    dragging_ = false;
    dragScroll_.stop();
    dragPoint_ = event->position().toPoint();
    updateDragSelection();
    event->accept();
}

void GitDiffSourcePane::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const int y = event->position().toPoint().y() + verticalScrollBar()->value() - topMargin_;
    const int line = y < 0 ? -1 : alignment_.lineAt(y / rowHeight_);
    if (line < 0) return;
    const auto begin = buffer_.lineStart(line), end = lineEnd(line);
    if (end - begin > EditorLineRendering::longLineBytes) return;
    position_ = positionAt(event->position().toPoint());
    const auto text = layoutForLine(line, begin, end).text();
    QTextBoundaryFinder words(QTextBoundaryFinder::Word, text);
    words.setPosition(qsizetype(buffer_.utf16Before(position_) - buffer_.utf16Before(begin)));
    const auto from = words.boundaryReasons().testFlag(QTextBoundaryFinder::StartOfItem)
        ? words.position() : std::max<qsizetype>(0, words.toPreviousBoundary());
    const auto to = words.toNextBoundary();
    anchor_ = begin + std::size_t(text.left(from).toUtf8().size());
    position_ = begin + std::size_t(text.left(to < 0 ? text.size() : to).toUtf8().size());
    dragging_ = false;
    dragScroll_.stop();
    viewport()->update();
    event->accept();
}
