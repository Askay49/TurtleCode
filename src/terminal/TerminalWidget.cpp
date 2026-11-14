#include "TerminalWidget.hpp"
#include "app/FontSettings.hpp"
#include "app/ThemeManager.hpp"
#include "app/WorkspaceBackground.hpp"
#include "app/ShortcutManager.hpp"
#include <QApplication>
#include <QClipboard>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QInputMethodEvent>
#include <QScrollBar>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>

namespace {
VTermModifier modifiers(Qt::KeyboardModifiers mods) {
    return static_cast<VTermModifier>((mods.testFlag(Qt::ShiftModifier) ? VTERM_MOD_SHIFT : 0)
        | (mods.testFlag(Qt::ControlModifier) ? VTERM_MOD_CTRL : 0)
        | (mods.testFlag(Qt::AltModifier) ? VTERM_MOD_ALT : 0));
}
QString cellText(const VTermScreenCell& cell) {
    if (cell.chars[0] == UINT32_MAX || cell.width == 0) return {};
    if (!cell.chars[0]) return QStringLiteral(" ");
    QString text;
    for (auto c : cell.chars) { if (!c || c == UINT32_MAX) break; char32_t value = c; text += QString::fromUcs4(&value, 1); }
    return text;
}
bool before(QPoint a, QPoint b) { return a.y() < b.y() || (a.y() == b.y() && a.x() < b.x()); }
}
TerminalWidget::TerminalWidget(QWidget* parent) : QAbstractScrollArea(parent) {
    setObjectName("terminalWidget");
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    viewport()->setMouseTracking(true);
    viewport()->setCursor(Qt::IBeamCursor);
    terminal_ = vterm_new(rows_, columns_);
    vterm_set_utf8(terminal_, 1);
    screen_ = vterm_obtain_screen(terminal_);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_SCREEN);
    static const VTermScreenCallbacks callbacks = {
        [](VTermRect, void* user) { static_cast<TerminalWidget*>(user)->viewport()->update(); return 1; },
        nullptr,
        [](VTermPos pos, VTermPos, int visible, void* user) {
            auto* self = static_cast<TerminalWidget*>(user);
            self->cursor_ = pos; self->cursorVisible_ = visible; self->viewport()->update(); return 1;
        },
        [](VTermProp prop, VTermValue* value, void* user) {
            auto* self = static_cast<TerminalWidget*>(user);
            switch (prop) {
            case VTERM_PROP_CURSORVISIBLE: self->cursorVisible_ = value->boolean; break;
            case VTERM_PROP_ALTSCREEN: self->alternate_ = value->boolean; self->clearSelection(); break;
            case VTERM_PROP_REVERSE: self->reverse_ = value->boolean; break;
            case VTERM_PROP_CURSORSHAPE: self->cursorShape_ = value->number; break;
            case VTERM_PROP_MOUSE: self->mouseMode_ = value->number; break;
            default: break;
            }
            self->viewport()->update(); return 1;
        },
        [](void*) { QApplication::beep(); return 1; },
        nullptr,
        [](int columns, const VTermScreenCell* cells, void* user) {
            auto* self = static_cast<TerminalWidget*>(user);
            self->history_.emplace_back(cells, cells + columns);
            self->historyBytes_ += columns * sizeof(VTermScreenCell);
            while (self->history_.size() > 10000 || self->historyBytes_ > 16 * 1024 * 1024) {
                self->historyBytes_ -= self->history_.front().size() * sizeof(VTermScreenCell);
                self->history_.pop_front();
                if (self->verticalScrollBar()->value() > 0) self->verticalScrollBar()->setValue(self->verticalScrollBar()->value() - 1);
            }
            return 1;
        },
        [](int columns, VTermScreenCell* cells, void* user) {
            auto* self = static_cast<TerminalWidget*>(user);
            if (self->history_.empty()) return 0;
            const auto& line = self->history_.back();
            for (int col = 0; col < columns; ++col) {
                if (col < static_cast<int>(line.size())) cells[col] = line[col];
                else {
                    cells[col] = {}; cells[col].width = 1;
                    vterm_state_get_default_colors(vterm_obtain_state(self->terminal_), &cells[col].fg, &cells[col].bg);
                }
            }
            self->historyBytes_ -= line.size() * sizeof(VTermScreenCell); self->history_.pop_back(); return 1;
        },
        [](void* user) {
            auto* self = static_cast<TerminalWidget*>(user);
            self->history_.clear(); self->historyBytes_ = 0; self->clearSelection(); return 1;
        }
    };
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_output_set_callback(terminal_, [](const char* bytes, size_t length, void* user) {
        auto* self = static_cast<TerminalWidget*>(user);
        if (self->batchingOutput_) self->batchedOutput_.append(bytes, length);
        else emit self->input(QByteArray(bytes, length));
    }, this);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &TerminalWidget::applyTheme);
    applyTheme();
    vterm_screen_reset(screen_, 1);
    applyFontSettings();
    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
}
TerminalWidget::~TerminalWidget() { vterm_free(terminal_); }
void TerminalWidget::feedOutput(const QByteArray& data) {
    const bool follow = verticalScrollBar()->value() == verticalScrollBar()->maximum();
    clearSelection();
    vterm_input_write(terminal_, data.constData(), data.size());
    vterm_screen_flush_damage(screen_);
    syncScroll(follow); viewport()->update();
}
void TerminalWidget::reset() {
    history_.clear(); historyBytes_ = 0; clearSelection(); preedit_.clear();
    vterm_screen_reset(screen_, 1); vterm_screen_flush_damage(screen_); syncScroll(true); viewport()->update();
}
void TerminalWidget::applyFontSettings() {
    setFont(FontSettings::font(FontSettings::Area::Terminal));
    const QFontMetrics metrics(font());
    cellWidth_ = std::max(1, metrics.horizontalAdvance(QLatin1Char('M')));
    cellHeight_ = std::max(1, metrics.height() + 1); ascent_ = metrics.ascent();
    updateSize(); viewport()->update();
}
void TerminalWidget::applyTheme() {
    if (!screen_) return;
    const auto& theme = ThemeManager::instance();
    foreground_ = theme.color("terminal.foreground"); background_ = theme.color("terminal.background");
    cursorColor_ = theme.color("terminal.cursor");
    selectionBackground_ = theme.color("terminal.selectionBackground");
    selectionForeground_ = theme.color("terminal.selectionForeground");
    for (int i = 0; i < 16; ++i) {
        const QColor color = theme.color(QString("terminal.ansi%1").arg(i));
        VTermColor entry;
        vterm_color_rgb(&entry, color.red(), color.green(), color.blue());
        vterm_state_set_palette_color(vterm_obtain_state(terminal_), i, &entry);
    }
    VTermColor fg, bg;
    vterm_color_rgb(&fg, foreground_.red(), foreground_.green(), foreground_.blue());
    vterm_color_rgb(&bg, background_.red(), background_.green(), background_.blue());
    vterm_screen_set_default_colors(screen_, &fg, &bg);
    viewport()->update();
}
void TerminalWidget::changeEvent(QEvent* event) {
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) applyTheme();
}
void TerminalWidget::syncScroll(bool follow) {
    verticalScrollBar()->setRange(0, alternate_ ? 0 : static_cast<int>(history_.size()));
    verticalScrollBar()->setPageStep(rows_);
    if (follow) verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}
void TerminalWidget::updateSize() {
    const int rows = std::clamp((viewport()->height() - 8) / cellHeight_, 1, 500);
    const int columns = std::clamp((viewport()->width() - 8) / cellWidth_, 2, 1000);
    if (rows == rows_ && columns == columns_) return;
    const bool follow = verticalScrollBar()->value() == verticalScrollBar()->maximum();
    rows_ = rows; columns_ = columns; clearSelection();
    vterm_set_size(terminal_, rows_, columns_); vterm_screen_flush_damage(screen_);
    syncScroll(follow); emit terminalSizeChanged(rows_, columns_);
}
void TerminalWidget::resizeEvent(QResizeEvent* event) { QAbstractScrollArea::resizeEvent(event); updateSize(); }
VTermScreenCell TerminalWidget::cell(int row, int column) const {
    VTermScreenCell result{}; result.width = 1;
    vterm_state_get_default_colors(vterm_obtain_state(terminal_), &result.fg, &result.bg);
    if (!alternate_ && row < static_cast<int>(history_.size())) {
        const auto& line = history_[row];
        if (column < static_cast<int>(line.size())) return line[column];
    } else {
        const int screenRow = row - (alternate_ ? 0 : static_cast<int>(history_.size()));
        if (screenRow >= 0 && screenRow < rows_) vterm_screen_get_cell(screen_, {screenRow, column}, &result);
    }
    return result;
}
QColor TerminalWidget::color(VTermColor value, bool foreground) const {
    if (VTERM_COLOR_IS_DEFAULT_FG(&value)) return foreground_;
    if (VTERM_COLOR_IS_DEFAULT_BG(&value)) return background_;
    // Zero-filled cells used to pad short history lines have the default background.
    Q_UNUSED(foreground);
    vterm_screen_convert_color_to_rgb(screen_, &value);
    return QColor(value.rgb.red, value.rgb.green, value.rgb.blue);
}
void TerminalWidget::paintEvent(QPaintEvent*) {
    QPainter painter(viewport());
    WorkspaceBackground::instance().paint(viewport(), painter, viewport()->rect(), reverse_ ? foreground_ : background_);
    QPoint first = selectionStart_, last = selectionEnd_;
    if (before(last, first)) std::swap(first, last);
    const int origin = alternate_ ? 0 : verticalScrollBar()->value();
    for (int row = 0; row < rows_; ++row) {
        for (int column = 0; column < columns_; ++column) {
            auto item = cell(origin + row, column);
            if (item.chars[0] == UINT32_MAX || item.width == 0) continue;
            const int span = std::max(1, static_cast<int>(item.width));
            const QRect rect(4 + column * cellWidth_, 4 + row * cellHeight_, span * cellWidth_, cellHeight_);
            QColor fg = color(item.fg, true), bg = color(item.bg, false);
            const bool reversed = bool(item.attrs.reverse) != reverse_;
            if (reversed) std::swap(fg, bg);
            bool paintBackground = reversed || !VTERM_COLOR_IS_DEFAULT_BG(&item.bg);
            const QPoint pos(column, origin + row);
            if (first.y() >= 0 && !before(pos, first) && !before(last, pos)) {
                bg = selectionBackground_; fg = selectionForeground_; paintBackground = true;
            }
            // Default cells reveal the shared wallpaper; explicit ANSI colors stay opaque.
            if (paintBackground) painter.fillRect(rect, bg);
            const bool cursor = cursorVisible_ && row == cursor_.row && column == cursor_.col
                && (alternate_ || verticalScrollBar()->value() == verticalScrollBar()->maximum());
            if (cursor && hasFocus() && cursorShape_ == VTERM_PROP_CURSORSHAPE_BLOCK) { painter.fillRect(rect, cursorColor_); fg = background_; }
            QFont styled = font(); styled.setBold(item.attrs.bold); styled.setItalic(item.attrs.italic);
            styled.setUnderline(item.attrs.underline != 0); styled.setStrikeOut(item.attrs.strike);
            painter.setFont(styled); painter.setPen(fg);
            if (!item.attrs.conceal) {
                painter.save(); painter.setClipRect(rect);
                painter.drawText(rect.left(), rect.top() + ascent_, cellText(item)); painter.restore();
            }
            if (cursor && (!hasFocus() || cursorShape_ != VTERM_PROP_CURSORSHAPE_BLOCK)) {
                painter.setPen(cursorColor_);
                if (!hasFocus()) painter.drawRect(rect.adjusted(0, 0, -1, -1));
                else if (cursorShape_ == VTERM_PROP_CURSORSHAPE_UNDERLINE) painter.fillRect(rect.left(), rect.bottom()-1, rect.width(), 2, cursorColor_);
                else painter.fillRect(rect.left(), rect.top(), 2, rect.height(), cursorColor_);
            }
        }
    }
    if (!preedit_.isEmpty()) {
        const int y = 4 + cursor_.row * cellHeight_;
        painter.setFont(font()); painter.setPen(foreground_);
        painter.drawText(4 + cursor_.col * cellWidth_, y + ascent_, preedit_);
    }
}
void TerminalWidget::clearSelection() { selectionStart_ = selectionEnd_ = QPoint(-1, -1); }
QString TerminalWidget::selectedText() const {
    if (selectionStart_.y() < 0) return {};
    QPoint first = selectionStart_, last = selectionEnd_;
    if (before(last, first)) std::swap(first, last);
    QString text;
    for (int row = first.y(); row <= last.y(); ++row) {
        QString line;
        for (int col = row == first.y() ? first.x() : 0; col <= (row == last.y() ? last.x() : columns_ - 1); ++col)
            line += cellText(cell(row, col));
        while (line.endsWith(QLatin1Char(' '))) line.chop(1);
        if (row != first.y()) text += QLatin1Char('\n');
        text += line;
    }
    return text;
}
QString TerminalWidget::screenText() const {
    QString text;
    for (int row = 0; row < rows_; ++row) {
        QString line;
        for (int col = 0; col < columns_; ++col) line += cellText(cell((alternate_ ? 0 : history_.size()) + row, col));
        while (line.endsWith(QLatin1Char(' '))) line.chop(1);
        text += line + QLatin1Char('\n');
    }
    return text;
}
void TerminalWidget::copySelection() {
    const auto text = selectedText(); if (!text.isEmpty()) QApplication::clipboard()->setText(text);
}
void TerminalWidget::pasteClipboard() {
    pasteText(QApplication::clipboard()->text());
}
void TerminalWidget::pasteText(QString text) {
    // Strip escape/control characters from pasted text; preserve tabs and line breaks.
    text.replace("\r\n", "\n"); text.replace('\r', '\n');
    QString clean;
    for (const auto ch : text) if ((ch.unicode() >= 0x20 && (ch.unicode() < 0x7f || ch.unicode() > 0x9f)) || ch == '\n' || ch == '\t') clean += ch;
    if (clean.toUtf8().size() > 1024 * 1024) { QApplication::beep(); return; }
    clearSelection(); verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    vterm_keyboard_start_paste(terminal_); sendText(clean); vterm_keyboard_end_paste(terminal_);
}
void TerminalWidget::sendText(const QString& text, VTermModifier mods) {
    batchingOutput_ = true;
    for (auto ch : text.toUcs4()) vterm_keyboard_unichar(terminal_, ch, mods);
    batchingOutput_ = false;
    if (!batchedOutput_.isEmpty()) { const auto output = std::move(batchedOutput_); batchedOutput_.clear(); emit input(output); }
}
bool TerminalWidget::event(QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab)
            && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            keyPressEvent(key); return true;
        }
    }
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        // Preserve the command palette, terminal toggle and MRU switching; shell keys such
        // as Ctrl+C/S/Z/F belong to the terminal while it owns keyboard focus.
        for (const auto& id : {"navigation.commands", "view.terminal", "navigation.tabNext", "navigation.tabPrevious"}) {
            const auto sequence = ShortcutManager::instance().sequence(id);
            if (!sequence.isEmpty() && sequence[0] == key->keyCombination()) return QAbstractScrollArea::event(event);
        }
        key->accept(); return true;
    }
    return QAbstractScrollArea::event(event);
}
void TerminalWidget::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt: case Qt::Key_Meta:
    case Qt::Key_AltGr: case Qt::Key_CapsLock: case Qt::Key_NumLock:
        event->accept(); return;
    default: break;
    }
    const auto mods = event->modifiers();
    if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_C) { copySelection(); return; }
    if ((mods == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_V)
        || (mods == Qt::ShiftModifier && event->key() == Qt::Key_Insert)) { pasteClipboard(); return; }
    if (mods == Qt::ShiftModifier && (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown)) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + (event->key() == Qt::Key_PageUp ? -rows_ : rows_)); return;
    }
    VTermKey key = VTERM_KEY_NONE;
    switch (event->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: key = VTERM_KEY_ENTER; break;
    case Qt::Key_Tab: case Qt::Key_Backtab: key = VTERM_KEY_TAB; break;
    case Qt::Key_Backspace: key = VTERM_KEY_BACKSPACE; break;
    case Qt::Key_Escape: key = VTERM_KEY_ESCAPE; break;
    case Qt::Key_Up: key = VTERM_KEY_UP; break; case Qt::Key_Down: key = VTERM_KEY_DOWN; break;
    case Qt::Key_Left: key = VTERM_KEY_LEFT; break; case Qt::Key_Right: key = VTERM_KEY_RIGHT; break;
    case Qt::Key_Insert: key = VTERM_KEY_INS; break; case Qt::Key_Delete: key = VTERM_KEY_DEL; break;
    case Qt::Key_Home: key = VTERM_KEY_HOME; break; case Qt::Key_End: key = VTERM_KEY_END; break;
    case Qt::Key_PageUp: key = VTERM_KEY_PAGEUP; break; case Qt::Key_PageDown: key = VTERM_KEY_PAGEDOWN; break;
    default:
        if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F35)
            key = static_cast<VTermKey>(VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1));
    }
    clearSelection(); verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    auto mapped = modifiers(mods);
    if (mods.testFlag(Qt::GroupSwitchModifier)) mapped = VTERM_MOD_NONE;
    if (event->key() == Qt::Key_Backtab) mapped = static_cast<VTermModifier>(mapped | VTERM_MOD_SHIFT);
    if (key != VTERM_KEY_NONE) vterm_keyboard_key(terminal_, key, mapped);
    else if (mods.testFlag(Qt::ControlModifier) && !mods.testFlag(Qt::GroupSwitchModifier)
        && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        QByteArray control;
        if (mods.testFlag(Qt::AltModifier)) control += '\x1b';
        control += char(event->key() - Qt::Key_A + 1);
        emit input(control);
    } else if (mods.testFlag(Qt::ControlModifier) && !mods.testFlag(Qt::GroupSwitchModifier)
        && ((event->key() >= Qt::Key_BracketLeft && event->key() <= Qt::Key_Underscore)
            || event->key() == Qt::Key_Space || event->key() == Qt::Key_At)) {
        QByteArray control;
        if (mods.testFlag(Qt::AltModifier)) control += '\x1b';
        control += char(event->key() == Qt::Key_Space ? 0 : event->key() & 0x1f);
        emit input(control);
    } else if (!event->text().isEmpty()) {
        // Qt already applies Shift/AltGr to printable text.
        sendText(event->text(), static_cast<VTermModifier>(mapped & ~VTERM_MOD_SHIFT));
    }
    event->accept(); viewport()->update();
}
void TerminalWidget::inputMethodEvent(QInputMethodEvent* event) {
    preedit_ = event->preeditString();
    if (!event->commitString().isEmpty()) { clearSelection(); sendText(event->commitString()); }
    event->accept(); viewport()->update();
}
QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImCursorRectangle) return QRect(4 + cursor_.col * cellWidth_, 4 + cursor_.row * cellHeight_, cellWidth_, cellHeight_);
    if (query == Qt::ImFont) return font();
    if (query == Qt::ImSurroundingText || query == Qt::ImCurrentSelection) return QString();
    return QAbstractScrollArea::inputMethodQuery(query);
}
QPoint TerminalWidget::cellAt(const QPointF& point) const {
    return {std::clamp(static_cast<int>((point.x() - 4) / cellWidth_), 0, columns_ - 1),
        std::clamp(static_cast<int>((point.y() - 4) / cellHeight_), 0, rows_ - 1) + (alternate_ ? 0 : verticalScrollBar()->value())};
}
bool TerminalWidget::reportMouse(QMouseEvent* event, bool pressed, bool motion) {
    if (mouseMode_ == VTERM_PROP_MOUSE_NONE || event->modifiers().testFlag(Qt::ShiftModifier)) return false;
    const auto position = cellAt(event->position());
    vterm_mouse_move(terminal_, std::clamp(position.y() - (alternate_ ? 0 : verticalScrollBar()->value()), 0, rows_ - 1), position.x(), modifiers(event->modifiers()));
    if (!motion) {
        int button = event->button() == Qt::LeftButton ? 1 : event->button() == Qt::MiddleButton ? 2 : event->button() == Qt::RightButton ? 3 : 0;
        if (button) vterm_mouse_button(terminal_, button, pressed, modifiers(event->modifiers()));
    }
    return true;
}
void TerminalWidget::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (reportMouse(event, true, false)) return;
    if (event->button() == Qt::LeftButton) { selecting_ = true; selectionStart_ = selectionEnd_ = cellAt(event->position()); viewport()->update(); }
    else if (event->button() == Qt::MiddleButton) {
        pasteText(QApplication::clipboard()->text(QClipboard::Selection));
    }
}
void TerminalWidget::mouseMoveEvent(QMouseEvent* event) {
    if (selecting_) { selectionEnd_ = cellAt(event->position()); viewport()->update(); }
    else reportMouse(event, false, true);
}
void TerminalWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (selecting_ && event->button() == Qt::LeftButton) {
        selectionEnd_ = cellAt(event->position()); selecting_ = false;
        if (QApplication::clipboard()->supportsSelection()) QApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
        viewport()->update();
    } else reportMouse(event, false, false);
}
void TerminalWidget::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (mouseMode_ != VTERM_PROP_MOUSE_NONE && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        const auto pos = cellAt(event->position());
        vterm_mouse_move(terminal_, pos.y() - (alternate_ ? 0 : verticalScrollBar()->value()), pos.x(), modifiers(event->modifiers()));
        for (int i = 0; i < std::abs(steps); ++i) {
            const int button = steps > 0 ? 4 : 5;
            vterm_mouse_button(terminal_, button, true, modifiers(event->modifiers())); vterm_mouse_button(terminal_, button, false, modifiers(event->modifiers()));
        }
    } else if (alternate_) {
        for (int i = 0; i < std::abs(steps) * 3; ++i) vterm_keyboard_key(terminal_, steps > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
    } else verticalScrollBar()->setValue(verticalScrollBar()->value() - steps * 3);
    event->accept();
}
void TerminalWidget::focusInEvent(QFocusEvent* event) { QAbstractScrollArea::focusInEvent(event); vterm_state_focus_in(vterm_obtain_state(terminal_)); viewport()->update(); }
void TerminalWidget::focusOutEvent(QFocusEvent* event) { QAbstractScrollArea::focusOutEvent(event); vterm_state_focus_out(vterm_obtain_state(terminal_)); viewport()->update(); }
