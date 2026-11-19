#include "EditorWidget.hpp"

#include <QCompleter>
#include <QKeyEvent>
#include <QSettings>
#include <QAbstractItemView>

namespace {
bool isModifier(const QKeyEvent& event) {
    switch (event.key()) {
    case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt: case Qt::Key_Meta: case Qt::Key_AltGr:
        return true;
    default:
        return false;
    }
}

bool isMappingCharacter(const QKeyEvent& event) {
    const auto text = event.text();
    return !(event.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        && !text.isEmpty() && QChar::isPrint(text.size() == 2 && text[0].isHighSurrogate() && text[1].isLowSurrogate()
            ? QChar::surrogateToUcs4(text[0], text[1]) : text.size() == 1 ? text[0].unicode() : 0);
}

QVector<VimState::Input> mappingKeys(const QString& text) {
    static const QHash<QString, Qt::Key> namedKeys{
        {"esc", Qt::Key_Escape}, {"insert", Qt::Key_Insert}, {"cr", Qt::Key_Return},
        {"tab", Qt::Key_Tab}, {"bs", Qt::Key_Backspace}, {"del", Qt::Key_Delete},
        {"left", Qt::Key_Left}, {"right", Qt::Key_Right}, {"up", Qt::Key_Up}, {"down", Qt::Key_Down},
        {"home", Qt::Key_Home}, {"end", Qt::Key_End}, {"space", Qt::Key_Space}, {"lt", Qt::Key_Less}
    };
    QVector<VimState::Input> keys;
    qsizetype start = 0;
    while (start < text.size()) {
        const auto open = text.indexOf('<', start);
        if (open < 0) {
            keys += VimState::keys(text.mid(start));
            break;
        }
        keys += VimState::keys(text.mid(start, open - start));
        const auto close = text.indexOf('>', open + 1);
        if (close < 0) return {};
        const auto name = text.mid(open + 1, close - open - 1).toLower();
        VimState::Input input;
        if (const auto named = namedKeys.constFind(name); named != namedKeys.cend()) {
            input.key = *named;
            if (name == "cr") input.text = "\r";
            else if (name == "tab") input.text = "\t";
            else if (name == "space") input.text = " ";
            else if (name == "lt") input.text = "<";
        } else if (name.size() == 3 && name.startsWith("c-") && name[2] >= 'a' && name[2] <= 'z') {
            input.key = Qt::Key_A + name[2].unicode() - 'a';
            input.modifiers = Qt::ControlModifier;
        } else return {};
        keys.append(input);
        start = close + 1;
    }
    return keys;
}

QKeyEvent keyEvent(const VimState::Input& input) {
    return QKeyEvent(QEvent::KeyPress, input.key, Qt::KeyboardModifiers(input.modifiers), input.text);
}
}

void EditorWidget::loadVimInsertMappings() {
    vimInsertMappings_.clear();
    const auto rows = QSettings().value("vimKeymaps/bindings").toString().split('\n');
    for (const auto& row : rows) {
        const auto separator = row.indexOf('=');
        if (separator < 0) continue;
        const auto lhs = row.left(separator).simplified();
        if (!lhs.startsWith("insert ")) continue;
        const auto trigger = lhs.mid(7);
        if (trigger.isEmpty() || trigger.contains(' ') || trigger.contains('<')) continue;
        const auto rhs = row.mid(separator + 1).trimmed();
        if (!rhs.startsWith("keys:")) continue;
        const auto keys = mappingKeys(rhs.mid(5));
        if (!keys.isEmpty()) vimInsertMappings_.insert(trigger, keys);
    }
}

bool EditorWidget::vimInsertMappingAllowed() const {
    return vim_.enabled() && vim_.acceptsText() && !vim_.hasPending() && !busy_
        && !vimSendingReplay_ && !vimExActive_ && !vimExExecuting_ && !vimSubConfirm_
        && vimLiteral_.isEmpty() && vimDigraph_.isEmpty() && !vimInsertCtrlG_ && !vimInsertCtrlX_
        && (!completer_ || !completer_->popup()->isVisible());
}

bool EditorWidget::reserveVimInsertMapping(const QKeyEvent& event) {
    if (isModifier(event) || vimSendingReplay_ || vimInsertMappings_.isEmpty()) return false;
    if (!vimInsertMappingAllowed() || !isMappingCharacter(event)) {
        resolveVimInsertMapping(false);
        return false;
    }
    if (!vimInsertMappingPending_.isEmpty()) return true;
    const auto first = vimInsertMappings_.lowerBound(event.text());
    return first != vimInsertMappings_.cend() && first.key().startsWith(event.text());
}

bool EditorWidget::handleVimInsertMapping(QKeyEvent& event) {
    if (!reserveVimInsertMapping(event)) return false;
    VimState::Input input;
    input.key = event.key();
    input.modifiers = int(event.modifiers());
    input.text = event.text();
    vimInsertMappingPending_.append(input);
    resolveVimInsertMapping(true);
    event.accept();
    return true;
}

void EditorWidget::resolveVimInsertMapping(bool waitForMore) {
    vimInsertMappingTimer_.stop();
    while (!vimInsertMappingPending_.isEmpty()) {
        if (!vimInsertMappingAllowed()) {
            flushVimInsertMapping();
            return;
        }
        QString prefix;
        QVector<VimState::Input> match;
        qsizetype matchedKeys = 0;
        for (qsizetype i = 0; i < vimInsertMappingPending_.size(); ++i) {
            prefix += vimInsertMappingPending_[i].text;
            const auto exact = vimInsertMappings_.constFind(prefix);
            if (exact != vimInsertMappings_.cend()) {
                matchedKeys = i + 1;
                match = exact.value();
            }
        }
        const auto longer = vimInsertMappings_.upperBound(prefix);
        if (waitForMore && longer != vimInsertMappings_.cend() && longer.key().startsWith(prefix)) {
            vimInsertMappingTimer_.start();
            return;
        }
        if (!matchedKeys) {
            match.append(vimInsertMappingPending_.front());
            matchedKeys = 1;
        }
        vimInsertMappingPending_.remove(0, matchedKeys);
        // Only the resolved keys reach the change and macro recorders.
        for (const auto& input : match) {
            auto event = keyEvent(input);
            processEditorKey(&event);
        }
    }
}

void EditorWidget::flushVimInsertMapping() {
    vimInsertMappingTimer_.stop();
    const auto pending = std::exchange(vimInsertMappingPending_, {});
    for (const auto& input : pending) {
        auto event = keyEvent(input);
        processEditorKey(&event);
    }
}
