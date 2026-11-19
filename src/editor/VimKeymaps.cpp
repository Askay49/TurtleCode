#include "VimKeymaps.hpp"
#include "app/I18n.hpp"

#include <QKeyEvent>
#include <QSettings>
#include <QRegularExpression>

namespace VimKeymaps {
namespace {

const QStringList modeNames{"normal", "visual", "operator", "insert", "replace", "command"};

const QHash<QString, int> namedKeys{
    {"esc", Qt::Key_Escape}, {"insert", Qt::Key_Insert}, {"cr", Qt::Key_Return},
    {"enter", Qt::Key_Return}, {"return", Qt::Key_Return}, {"tab", Qt::Key_Tab},
    {"bs", Qt::Key_Backspace}, {"del", Qt::Key_Delete}, {"left", Qt::Key_Left},
    {"right", Qt::Key_Right}, {"up", Qt::Key_Up}, {"down", Qt::Key_Down},
    {"home", Qt::Key_Home}, {"end", Qt::Key_End}, {"pageup", Qt::Key_PageUp},
    {"pagedown", Qt::Key_PageDown}, {"space", Qt::Key_Space}, {"lt", Qt::Key_Less},
    {"bar", Qt::Key_Bar}, {"bslash", Qt::Key_Backslash},
};

QString token(int key, Qt::KeyboardModifiers modifiers, const QString& text) {
    modifiers &= Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
    if (!(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        && !text.isEmpty() && QChar::isPrint(text.toUcs4().front()))
        return QStringLiteral("t") + text + QChar(0);
    if (key == Qt::Key_Enter) key = Qt::Key_Return;
    if (key == Qt::Key_Backtab) {
        key = Qt::Key_Tab;
        modifiers |= Qt::ShiftModifier;
    }
    if (!key || key == Qt::Key_unknown || key == Qt::Key_Shift || key == Qt::Key_Control
        || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr) return {};
    return QStringLiteral("k%1").arg(key | int(modifiers)) + QChar(0);
}

QVector<VimState::Input> keys(const QString& text, bool lhs, const Leaders& leaders) {
    QVector<VimState::Input> result;
    if (text.isEmpty() || text.size() > 65536) return {};
    for (qsizetype start = 0; start < text.size();) {
        const auto open = text.indexOf('<', start);
        const auto literals = text.mid(start, (open < 0 ? text.size() : open) - start);
        if (literals.contains(QChar(0))) return {};
        result += VimState::keys(literals);
        if (open < 0) break;
        const auto close = text.indexOf('>', open + 1);
        if (close < 0) return {};
        auto name = text.mid(open + 1, close - open - 1).toLower();
        VimState::Input input;
        if (name == "leader" || name == "localleader") {
            result += VimState::keys(name == "leader" ? leaders.leader : leaders.localLeader);
        } else {
            Qt::KeyboardModifiers modifiers;
            while (name.size() > 2 && name[1] == '-') {
                const auto prefix = name[0];
                if (prefix == 'c') modifiers |= Qt::ControlModifier;
                else if (prefix == 's') modifiers |= Qt::ShiftModifier;
                else if (prefix == 'm' || prefix == 'a') modifiers |= Qt::AltModifier;
                else if (prefix == 'd') modifiers |= Qt::MetaModifier;
                else return {};
                name.remove(0, 2);
            }
            const auto named = namedKeys.constFind(name);
            if (named != namedKeys.cend()) {
                input.key = *named;
                if (input.key == Qt::Key_Return) input.text = "\r";
                else if (input.key == Qt::Key_Tab) input.text = "\t";
                else if (input.key == Qt::Key_Space || input.key == Qt::Key_Less
                         || input.key == Qt::Key_Bar || input.key == Qt::Key_Backslash)
                    input.text = QChar(input.key);
            } else if (name.size() == 1 && QChar::isPrint(name[0].unicode()) && name[0].unicode() < 128) {
                input.key = name[0].toUpper().unicode();
                input.text = modifiers.testFlag(Qt::ShiftModifier) ? name.toUpper() : name;
            } else if (name.startsWith('f')) {
                bool ok = false;
                const auto index = name.mid(1).toInt(&ok);
                if (!ok || index < 1 || index > 35) return {};
                input.key = Qt::Key_F1 + index - 1;
            } else return {};
            if (input.key == Qt::Key_Tab && (modifiers & Qt::ShiftModifier)) input.key = Qt::Key_Backtab;
            input.modifiers = int(modifiers);
            if (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) input.text.clear();
            result.append(input);
        }
        start = close + 1;
    }
    if (lhs && result.size() > 128) return {};
    return result;
}

}

bool validLeader(const QString& key) {
    const auto codepoints = key.toUcs4();
    return codepoints.size() == 1 && QChar::isPrint(codepoints.front());
}

Leaders readLeaders(const QSettings& settings) {
    const auto read = [&](const char* name, const QString& fallback) {
        const auto value = settings.value(name).toString();
        return validLeader(value) ? value : fallback;
    };
    return {read("vimKeymaps/leader", QStringLiteral(" ")),
            read("vimKeymaps/localLeader", QStringLiteral("\\"))};
}

QString modeName(Mode value) { return modeNames.value(int(value)); }

std::optional<Mode> mode(const QString& name) {
    const auto index = modeNames.indexOf(name);
    if (index < 0) return {};
    return Mode(index);
}

QString token(const QKeyEvent& event) {
    return token(event.key(), event.modifiers(), event.text());
}

QString trigger(const QString& text, const Leaders& leaders) {
    QString result;
    for (const auto& input : keys(text, true, leaders)) {
        const auto part = token(input.key, Qt::KeyboardModifiers(input.modifiers), input.text);
        if (part.isEmpty()) return {};
        result += part;
    }
    return result;
}

std::optional<Binding> binding(const QString& lhs, const QString& rhs, bool recursive,
                               const CommandValidator& validCommand, const Leaders& leaders) {
    if (trigger(lhs, leaders).isEmpty()) return {};
    Binding result;
    result.trigger = lhs;
    result.recursive = recursive;
    result.action = rhs;
    if (rhs.startsWith("command:")) {
        result.command = rhs.mid(8);
        if (result.command.isEmpty() || (validCommand && !validCommand(result.command))) return {};
    } else {
        const auto sequence = rhs.startsWith("keys:") ? rhs.mid(5) : rhs;
        if (sequence.compare("<Nop>", Qt::CaseInsensitive) != 0) {
            result.keys = keys(sequence, false, leaders);
            if (result.keys.isEmpty()) return {};
        }
    }
    return result;
}

qsizetype settingsSeparator(const QString& line) {
    static const QRegularExpression separator(QStringLiteral("^\\s*\\S+\\s+\\S+\\s+(=)"));
    const auto match = separator.match(line);
    return match.hasMatch() ? match.capturedStart(1) : line.indexOf('=');
}

QString parseSettings(const QString& text, Tables& result, const CommandValidator& validCommand,
                      const Leaders& leaders) {
    Tables parsed;
    const auto lines = text.split('\n');
    for (qsizetype row = 0; row < lines.size(); ++row) {
        const auto line = lines[row].trimmed();
        if (line.isEmpty()) continue;
        const auto separator = settingsSeparator(line);
        const auto lhs = separator >= 0 ? line.left(separator).simplified().split(' ') : QStringList{};
        const auto selectedMode = lhs.size() == 2 ? mode(lhs[0]) : std::nullopt;
        const auto rhs = line.mid(separator + 1).trimmed();
        const auto error = [row](const char* message) {
            return I18n::text("Line %1. %2").arg(QString::number(row + 1), I18n::text(message));
        };
        if (!selectedMode || !(rhs.startsWith("keys:") || rhs.startsWith("command:")))
            return error("Expected a mode, keys and a keys: or command: action.");
        if (rhs.startsWith("command:") && validCommand && !validCommand(rhs.mid(8)))
            return error("Unknown Turtle Code command ID.");
        const auto value = binding(lhs[1], rhs, false, validCommand, leaders);
        if (!value) return error("Invalid Vim key sequence.");
        auto& table = map(parsed, *selectedMode);
        const auto key = trigger(lhs[1], leaders);
        if (table.contains(key)) return error("Duplicate Vim mapping in this mode.");
        table.insert(key, *value);
    }
    result = std::move(parsed);
    return {};
}

std::optional<Command> exCommand(const QString& name, bool bang) {
    Command result;
    auto op = name;
    const auto isOperation = [](const QString& suffix) {
        return QStringList{"map", "noremap", "unmap", "mapclear", "abbreviate", "noreabbrev", "unabbreviate", "abclear"}.contains(suffix);
    };
    if (isOperation(op)) {
        result.modes = bang ? QVector<Mode>{Mode::Insert, Mode::Command}
                            : QVector<Mode>{Mode::Normal, Mode::Visual, Mode::Operator};
    } else {
        op = name.mid(1);
        if (!isOperation(op)) return {};
        const auto index = QStringLiteral("nvoric").indexOf(name[0] == 'x' ? 'v' : name[0]);
        if (index < 0) return {};
        constexpr std::array modes{Mode::Normal, Mode::Visual, Mode::Operator, Mode::Replace, Mode::Insert, Mode::Command};
        result.modes = {modes[index]};
    }
    result.abbreviation = op.contains("ab");
    if (result.abbreviation && result.modes.size() > 1) result.modes = {Mode::Insert, Mode::Command};
    result.recursive = op == "map" || op == "abbreviate";
    if (op.startsWith("un")) result.operation = Command::Remove;
    if (op.endsWith("clear")) result.operation = Command::Clear;
    return result;
}

Store& global() {
    static Store store;
    return store;
}

void reloadSettings(const CommandValidator& validCommand) {
    static QString loadedSource;
    static std::optional<Leaders> loadedLeaders;
    const QSettings settings;
    const auto source = settings.value("vimKeymaps/bindings").toString();
    const auto leaders = readLeaders(settings);
    if (loadedSource == source && loadedLeaders == leaders) return;
    Tables parsed;
    if (!parseSettings(source, parsed, validCommand, leaders).isEmpty()) return;
    global().mappings = std::move(parsed);
    loadedSource = source;
    loadedLeaders = leaders;
}

Map& map(Tables& tables, Mode mode) { return tables[static_cast<std::size_t>(mode)]; }
const Map& map(const Tables& tables, Mode mode) { return tables[static_cast<std::size_t>(mode)]; }

}
