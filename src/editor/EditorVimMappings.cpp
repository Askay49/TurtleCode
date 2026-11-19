#include "EditorWidget.hpp"
#include "app/I18n.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QKeyEvent>
#include <QLineEdit>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSettings>
#include <QTextBoundaryFinder>
#include <algorithm>

namespace {
using namespace VimKeymaps;

VimState::Input keyInput(const QKeyEvent& event) {
    VimState::Input input;
    input.key = event.key();
    input.modifiers = int(event.modifiers());
    input.text = event.text();
    return input;
}

struct Match {
    std::optional<Binding> binding;
    qsizetype length{};
    QString lhs;
    bool longer{};
};

Match findMapping(const Store& local, Mode mode, const QStringList& prefixes) {
    const auto& prefix = prefixes.back();
    Match result;
    const auto search = [&](const Map& table) {
        for (qsizetype i = result.length; i < prefixes.size(); ++i) {
            const auto it = table.constFind(prefixes[i]);
            if (it == table.cend()) continue;
            result.binding = it.value();
            result.length = i + 1;
            result.lhs = prefixes[i];
        }
        const auto longer = table.upperBound(prefix);
        result.longer |= longer != table.cend() && longer.key().startsWith(prefix);
    };
    const auto searchMode = [&](Mode selected) {
        search(map(local.mappings, selected));
        search(map(global().mappings, selected));
    };
    searchMode(mode);
    if (mode == Mode::Replace) searchMode(Mode::Insert);
    return result;
}

bool keyword(char32_t ch) { return ch == '_' || QChar::isLetterOrNumber(ch) || QChar::isMark(ch); }

bool validAbbreviation(const QString& text) {
    const auto chars = text.toUcs4();
    if (chars.isEmpty() || chars.size() > 128
        || std::any_of(chars.begin(), chars.end(), [](char32_t ch) {
            return ch == '<' || !QChar::isPrint(ch) || QChar::isSpace(ch);
        })) return false;
    if (!keyword(chars.back()) || chars.size() < 3) return true;
    const bool firstKeyword = keyword(chars.front());
    return std::all_of(chars.begin(), chars.end() - 1,
                       [firstKeyword](char32_t ch) { return keyword(ch) == firstKeyword; });
}

bool endsWithAbbreviation(const QString& text, const QString& lhs) {
    if (!text.endsWith(lhs)) return false;
    const auto before = text.size() - lhs.size();
    if (!before) return true;
    const auto previous = text.left(before).toUcs4().back();
    if (!keyword(lhs.toUcs4().back())) return QChar::isSpace(previous);
    if (keyword(lhs.toUcs4().front())) return !keyword(previous);
    return QChar::isSpace(previous) || keyword(previous);
}
}

void EditorWidget::setVimCommandHandlers(VimKeymaps::CommandValidator validator,
                                         VimKeymaps::CommandValidator executor) {
    vimValidCommand_ = std::move(validator);
    vimRunCommand_ = std::move(executor);
}

void EditorWidget::reloadVimMappings() {
    flushVimMapping();
    VimKeymaps::reloadSettings(vimValidCommand_);
}

std::optional<VimKeymaps::Mode> EditorWidget::vimMappingMode() const {
    using Mode = VimKeymaps::Mode;
    if (!vim_.enabled() || busy_ || vimBypassMappings_ || vimSendingInsertRepeat_
        || (isVimReplaying() && !vimRemapReplay_) || (vimExExecuting_ && !vimRemapReplay_)
        || vimSubConfirm_) return {};
    if (vimExActive_) return vimExRegisterPending_ ? std::nullopt : std::optional{Mode::Command};
    if (!vim_.allowsMapping() || !vimLiteral_.isEmpty() || !vimDigraph_.isEmpty()
        || vimInsertCtrlG_ || vimInsertCtrlX_ || (completer_ && completer_->popup()->isVisible())) return {};
    switch (vim_.mode()) {
    case VimController::Mode::Normal:
        return vim_.operatorPending() ? Mode::Operator : Mode::Normal;
    case VimController::Mode::Visual:
        return Mode::Visual;
    case VimController::Mode::Insert:
        return Mode::Insert;
    case VimController::Mode::Replace:
        return Mode::Replace;
    }
    return {};
}

bool EditorWidget::reserveVimMapping(const QKeyEvent& event) {
    const auto mode = vimMappingMode();
    if (!mode) return false;
    const auto token = VimKeymaps::token(event);
    if (token.isEmpty()) return false;
    if (!vimMappingPending_.isEmpty()) return true;
    if (*mode == VimKeymaps::Mode::Normal && event.text() == "q"
        && !VimState::Session::instance().recording.isNull()) return false;
    const auto match = findMapping(document_->vimKeymaps, *mode, {token});
    return match.binding || match.longer;
}

bool EditorWidget::handleVimMapping(QKeyEvent& event) {
    if (!reserveVimMapping(event)) {
        if (!vimBypassMappings_ && !VimKeymaps::token(event).isEmpty() && !vimMappingPending_.isEmpty())
            flushVimMapping();
        return false;
    }
    vimMappingPending_.append(keyInput(event));
    resolveVimMapping(true);
    event.accept();
    return true;
}

void EditorWidget::resolveVimMapping(bool waitForMore) {
    vimMappingTimer_.stop();
    if (!vimMappingDepth_) vimMappingSteps_ = 0;
    while (!vimMappingPending_.isEmpty()) {
        const auto mode = vimMappingMode();
        if (!mode) {
            flushVimMapping();
            return;
        }
        QStringList prefixes;
        QString prefix;
        for (const auto& input : vimMappingPending_) {
            const QKeyEvent event(QEvent::KeyPress, input.key, Qt::KeyboardModifiers(input.modifiers), input.text);
            prefix += VimKeymaps::token(event);
            prefixes.append(prefix);
        }
        const auto match = findMapping(document_->vimKeymaps, *mode, prefixes);
        if (waitForMore && match.longer) {
            const int timeout = vimMappingTimeout();
            if (timeout >= 0) vimMappingTimer_.start(timeout);
            return;
        }
        if (!match.binding) {
            const auto input = vimMappingPending_.takeFirst();
            QKeyEvent event(QEvent::KeyPress, input.key, Qt::KeyboardModifiers(input.modifiers), input.text);
            if (!expandVimAbbreviation(event)) sendVimMappingKey(input, false);
            continue;
        }
        vimMappingPending_.remove(0, match.length);
        auto tail = std::exchange(vimMappingPending_, {});
        if (++vimMappingSteps_ > 1000 || vimMappingDepth_ >= 100) {
            vimExMessage("E223: Recursive mapping");
            return;
        }
        QScopedValueRollback depth(vimMappingDepth_, vimMappingDepth_ + 1);
        const auto& binding = *match.binding;
        if (!binding.command.isEmpty()) runVimMappingCommand(binding.command, true);
        else {
            // Vim protects the first key of a recursive expansion beginning with its own LHS.
            const auto rhsPrefix = [&] {
                QString prefix;
                for (const auto& key : binding.keys) {
                    const QKeyEvent event(QEvent::KeyPress, key.key, Qt::KeyboardModifiers(key.modifiers), key.text);
                    prefix += VimKeymaps::token(event);
                    if (prefix.size() >= match.lhs.size()) break;
                }
                return prefix;
            };
            const bool startsWithSelf = binding.recursive && rhsPrefix() == match.lhs;
            for (qsizetype i = 0; i < binding.keys.size() && vimMappingSteps_ <= 1000; ++i) {
                if (busy_ || (vimExExecuting_ && !vimExNormal_ && !vimSubConfirm_)) {
                    replayVim(binding.keys.mid(i) + tail);
                    tail.clear();
                    break;
                }
                sendVimMappingKey(binding.keys[i], binding.recursive && (i > 0 || !startsWithSelf));
            }
        }
        vimMappingPending_ += tail;
    }
}

void EditorWidget::flushVimMapping() {
    vimMappingTimer_.stop();
    const auto pending = std::exchange(vimMappingPending_, {});
    for (const auto& input : pending) sendVimMappingKey(input, false);
}

void EditorWidget::sendVimMappingKey(const VimState::Input& input, bool remap) {
    QKeyEvent event(QEvent::KeyPress, input.key, Qt::KeyboardModifiers(input.modifiers), input.text);
    QScopedValueRollback bypass(vimBypassMappings_, !remap);
    if (vimExActive_) QApplication::sendEvent(vimExInput_, &event);
    else if (!remap) processEditorKey(&event);
    else keyPressEvent(&event);
}

void EditorWidget::runVimMappingCommand(const QString& id, bool record) {
    if (vimExActive_) closeVimCommandLine();
    vim_.resetPending();
    vimCommandKeys_.clear();
    if (!vimRunCommand_ || !vimRunCommand_(id)) {
        vimExMessage(I18n::text("Vim: command is unavailable: %1").arg(id));
        return;
    }
    auto& session = VimState::Session::instance();
    if (record && !vimSendingReplay_ && !session.recording.isNull() && session.recordingInputs.size() < 100000
        && session.recordingBytes + std::size_t(id.toUtf8().size()) <= 1024 * 1024) {
        VimState::Input input;
        input.kind = VimState::Input::ApplicationCommand;
        input.text = id;
        session.recordingInputs.append(input);
        session.recordingBytes += std::size_t(id.toUtf8().size());
    }
}

bool EditorWidget::expandVimAbbreviation(QKeyEvent& event) {
    const auto mode = vimMappingMode();
    if (!mode || (*mode != Mode::Insert && *mode != Mode::Replace && *mode != Mode::Command)
        || (!vimExActive_ && hasSelection())) return false;
    const bool force = event.key() == Qt::Key_BracketRight && event.modifiers() == Qt::ControlModifier;
    const bool enter = event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter;
    if (!force && !enter && (event.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) return false;
    const auto typed = event.text().toUcs4();
    if (!force && !enter && (typed.size() != 1 || keyword(typed.front()))) return false;
    const auto& local = map(document_->vimKeymaps.abbreviations, *mode == Mode::Replace ? Mode::Insert : *mode);
    const auto& globalTable = map(global().abbreviations, *mode == Mode::Replace ? Mode::Insert : *mode);
    if (local.isEmpty() && globalTable.isEmpty()) return false;
    QString text;
    if (vimExActive_) text = vimExInput_->text().left(vimExInput_->cursorPosition()).right(260);
    else {
        const auto begin = std::max(vimLineStart(currentLine()), cursorPosition_ - std::min<std::size_t>(520, cursorPosition_));
        const auto bytes = buffer_.slice(begin, cursorPosition_ - begin);
        text = QString::fromUtf8(bytes.data(), qsizetype(bytes.size()));
    }
    std::optional<Binding> match;
    for (const auto* table : {&local, &globalTable}) {
        for (const auto& binding : *table) {
            if ((!match || match->trigger.size() < binding.trigger.size()) && endsWithAbbreviation(text, binding.trigger))
                match = binding;
        }
    }
    if (!match) return false;
    const auto lhs = match->trigger;
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, lhs);
    VimState::Input backspace;
    backspace.key = Qt::Key_Backspace;
    while (boundaries.toNextBoundary() >= 0) sendVimMappingKey(backspace, false);
    for (const auto& key : match->keys) sendVimMappingKey(key, match->recursive);
    if (!force) sendVimMappingKey(keyInput(event), false);
    event.accept();
    return true;
}

QString EditorWidget::executeVimMappingCommand(const VimEx::Invocation& invocation, const VimKeymaps::Command& command) {
    using namespace VimKeymaps;
    auto arguments = invocation.arguments;
    bool local = false;
    if (arguments.startsWith("<buffer>", Qt::CaseInsensitive)
        && (arguments.size() == 8 || arguments[8].isSpace())) {
        local = true;
        arguments = arguments.mid(8).trimmed();
    }
    const auto firstSpace = arguments.indexOf(QRegularExpression(QStringLiteral("\\s")));
    const auto lhs = firstSpace < 0 ? arguments : arguments.left(firstSpace);
    auto rhs = firstSpace < 0 ? QString{} : arguments.mid(firstSpace).trimmed();
    rhs.replace(QStringLiteral("\\|"), QStringLiteral("<Bar>"));
    auto& store = local ? document_->vimKeymaps : global();
    auto& tables = command.abbreviation ? store.abbreviations : store.mappings;
    const auto leaders = readLeaders(QSettings{});
    const auto key = command.abbreviation ? lhs : trigger(lhs, leaders);
    if (command.operation == Command::Clear) {
        if (!arguments.isEmpty()) return "E488: Trailing characters";
        for (const auto mode : command.modes) map(tables, mode).clear();
    } else if (command.operation == Command::Remove) {
        if (lhs.isEmpty() || key.isEmpty() || !rhs.isEmpty()) return "E474: Invalid argument";
        bool removed = false;
        for (const auto mode : command.modes) removed |= map(tables, mode).remove(key) > 0;
        if (!removed) return command.abbreviation ? "E24: No such abbreviation" : "E31: No such mapping";
    } else if (rhs.isEmpty()) {
        QStringList rows;
        const auto list = [&](const Store& source, bool bufferLocal) {
            for (const auto mode : command.modes) {
                for (const auto& value : map(command.abbreviation ? source.abbreviations : source.mappings, mode)) {
                    if (!lhs.isEmpty() && !value.trigger.startsWith(lhs)) continue;
                    rows.append(modeName(mode) + (bufferLocal ? " <buffer> " : " ") + value.trigger + "  " + value.action);
                }
            }
        };
        list(document_->vimKeymaps, true);
        if (!local) list(global(), false);
        vimExMessage(rows.isEmpty() ? I18n::text("No Vim mappings") : rows.join('\n').left(4096));
    } else {
        if (command.abbreviation && (!validAbbreviation(lhs) || rhs.startsWith("command:")))
            return "E474: Invalid abbreviation";
        const auto value = binding(lhs, rhs, command.recursive, vimValidCommand_, leaders);
        if (!value || key.isEmpty()) return "E474: Invalid mapping";
        for (const auto mode : command.modes) map(tables, mode).insert(key, *value);
    }
    return {};
}
