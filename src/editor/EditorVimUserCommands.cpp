#include "EditorWidget.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <utility>

namespace {

QMap<QString, VimEx::UserCommand>& userCommands() {
    static QMap<QString, VimEx::UserCommand> commands;
    return commands;
}

QString takeWord(QString& text) {
    static const QRegularExpression space(QStringLiteral("\\s"));
    const auto end = text.indexOf(space);
    if (end < 0) return std::exchange(text, {});
    const auto word = text.left(end);
    text = text.mid(end).trimmed();
    return word;
}

bool validName(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Z][A-Za-z0-9]*$"));
    return name.size() <= 128 && pattern.match(name).hasMatch();
}

QString expandUserCommand(const VimEx::UserCommand& user, const VimEx::Invocation& call) {
    static const QRegularExpression placeholder(QStringLiteral("<(args|q-args|bang|line1|line2|range|lt)>"),
                                                QRegularExpression::CaseInsensitiveOption);
    const auto quoteArgs = [&] {
        auto quoted = call.arguments;
        quoted.replace("\\", "\\\\").replace("\"", "\\\"");
        return QStringLiteral("\"") + quoted + '"';
    };
    const QHash<QString, QString> values{
        {"args", call.arguments}, {"q-args", quoteArgs()}, {"bang", call.bang ? "!" : ""},
        {"line1", QString::number(call.first)}, {"line2", QString::number(call.last)},
        {"range", call.explicitRange ? (call.first == call.last ? "1" : "2") : "0"}, {"lt", "<"},
    };
    QString result;
    qsizetype offset = 0;
    auto matches = placeholder.globalMatch(user.body);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += user.body.mid(offset, match.capturedStart() - offset);
        result += values.value(match.captured(1).toLower());
        offset = match.capturedEnd();
    }
    return result + user.body.mid(offset);
}

struct Autocmd {
    QString group, event, pattern, body;
    QRegularExpression matcher;
    QPointer<Document> buffer;
    bool local{};

    bool matches(Document* document, const QString& path) const {
        if (local) return buffer == document;
        if (pattern.contains('/')) return matcher.match(QDir::fromNativeSeparators(path)).hasMatch();
        return matcher.match(QFileInfo(path).fileName()).hasMatch();
    }
};

struct Autocmds {
    QList<Autocmd> entries;
    QSet<QString> groups;
    QString selected;
    std::weak_ptr<int> executing;
};

Autocmds& autocmds() {
    static Autocmds values;
    return values;
}

QString eventName(const QString& name) {
    if (name.compare("BufRead", Qt::CaseInsensitive) == 0) return "BufReadPost";
    for (const auto& event : QStringList{"BufEnter", "BufLeave", "BufReadPost", "BufWritePost", "BufNewFile"})
        if (event.compare(name, Qt::CaseInsensitive) == 0) return event;
    return {};
}

QStringList autocmdPrograms(Document* document, const QString& group, const QString& event, const QString& path) {
    QStringList result;
    auto& state = autocmds();
    state.entries.removeIf([](const auto& entry) { return entry.local && entry.buffer.isNull(); });
    for (const auto& entry : state.entries) {
        if ((!group.isEmpty() && group != entry.group) || entry.event != event || !entry.matches(document, path)) continue;
        result.append(entry.body);
    }
    return result;
}

}

VimEx::Registry EditorWidget::vimExRegistry() const {
    static const VimEx::Registry builtins;
    auto registry = builtins;
    for (const auto& command : userCommands()) registry.add(command.definition);
    for (const auto& command : document_->vimCommands) registry.add(command.definition);
    return registry;
}

QString EditorWidget::executeVimUserCommand(const VimEx::Invocation& invocation) {
    const auto name = invocation.definition.name;
    auto& global = userCommands();
    auto& local = document_->vimCommands;
    auto args = invocation.arguments;
    if (name == "comclear") {
        global.clear();
        local.clear();
        return {};
    }
    if (name == "delcommand") {
        if (args.startsWith("-buffer ")) {
            args = args.mid(8).trimmed();
            if (!local.remove(args)) return "E184: No such user-defined command: " + args;
        } else if (!local.remove(args) && !global.remove(args)) return "E184: No such user-defined command: " + args;
        return {};
    }
    if (name == "command") {
        VimEx::UserCommand command;
        command.definition.minimum = 1;
        command.definition.arguments = VimEx::Arguments::Raw;
        command.definition.comments = false;
        command.definition.bars = false;
        bool bufferLocal = false;
        while (args.startsWith('-')) {
            const auto attribute = takeWord(args);
            if (attribute == "-buffer") bufferLocal = true;
            else if (attribute == "-bang") command.definition.bang = true;
            else if (attribute == "-bar") command.definition.bars = true;
            else if (attribute == "-range" || attribute == "-range=%") {
                command.definition.range = true;
                if (attribute == "-range=%") command.definition.defaultRange = VimEx::DefaultRange::Whole;
            } else if (attribute.startsWith("-nargs=") && attribute.size() == 8
                       && QStringLiteral("01*?+").contains(attribute.back())) command.nargs = attribute.back();
            else return "E181: Invalid attribute: " + attribute;
        }
        const auto commandName = takeWord(args);
        auto& commands = bufferLocal ? local : global;
        if (args.isEmpty()) {
            QStringList listing;
            const auto append = [&](const auto& source, const QString& scope) {
                for (const auto& item : source)
                    if (item.definition.name.startsWith(commandName)) listing.append(scope + item.definition.name + "  " + item.body);
            };
            append(local, "-buffer ");
            if (!bufferLocal) append(global, {});
            vimExMessage(listing.isEmpty() ? QStringLiteral("No user-defined commands") : listing.join('\n').left(4096));
            return {};
        }
        if (!validName(commandName)) return "E183: Invalid user command name: " + commandName;
        if (commands.contains(commandName) && !invocation.bang) return "E174: Command already exists: " + commandName;
        if (!commands.contains(commandName) && commands.size() >= 1024) return "E169: Too many user commands";
        command.definition.name = commandName;
        command.body = args;
        commands.insert(commandName, std::move(command));
        return {};
    }
    const auto scoped = local.constFind(name);
    const auto globalCommand = global.constFind(name);
    if (scoped == local.cend() && globalCommand == global.cend()) return "E184: No such user-defined command: " + name;
    const auto& command = scoped != local.cend() ? scoped.value() : globalCommand.value();
    if ((command.nargs == '1' || command.nargs == '+') && args.isEmpty()) return "E471: Argument required";
    if (command.nargs == '0' && !args.isEmpty()) return "E488: Trailing characters: " + args;
    if (vimExFrames_.size() >= 100) return "E169: Command too recursive";
    const auto body = expandUserCommand(command, invocation);
    if (body.size() > 1024 * 1024) return "E169: Expanded command too large";
    beginVimExProgram({body});
    return {};
}

bool EditorWidget::vimExHasAutocmd() const {
    return std::any_of(vimExFrames_.begin(), vimExFrames_.end(),
                       [](const auto& frame) { return bool(frame.autocmdGuard); });
}

void EditorWidget::beginVimExProgram(QStringList lines, std::shared_ptr<int> autocmdGuard) {
    if (!vimExExecuting_) {
        history_.beginCompoundGroup();
        vimExExecuting_ = true;
        ++vimExGeneration_;
        vimExSteps_ = 0;
    }
    ExFrame frame;
    frame.kind = ExFrame::Commands;
    frame.lines = std::move(lines);
    frame.after = std::exchange(vimExRemaining_, {});
    frame.scope = std::exchange(vimExScope_, {});
    frame.autocmdGuard = std::move(autocmdGuard);
    vimExFrames_.push_back(std::move(frame));
}

QString EditorWidget::executeVimAutocmd(const VimEx::Invocation& invocation) {
    auto& state = autocmds();
    auto args = invocation.arguments;
    if (invocation.definition.name == "augroup") {
        if (args.isEmpty()) {
            auto groups = state.groups.values();
            groups.sort();
            vimExMessage(groups.join(' '));
        } else if (args == "END" && !invocation.bang) state.selected.clear();
        else {
            static const QRegularExpression groupName(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
            if (!groupName.match(args).hasMatch() || !eventName(args).isEmpty())
                return "E474: Invalid autocommand group: " + args;
            if (invocation.bang) {
                if (!state.groups.remove(args)) return "E367: No such group: " + args;
                state.entries.removeIf([&](const auto& entry) { return entry.group == args; });
                if (state.selected == args) state.selected.clear();
            } else {
                state.groups.insert(args);
                state.selected = args;
            }
        }
        return {};
    }
    QString group;
    auto events = takeWord(args);
    if (state.groups.contains(events)) {
        group = events;
        events = takeWord(args);
    }
    if (invocation.definition.name == "doautocmd") {
        const auto event = eventName(events);
        if (event.isEmpty()) return "E216: No such event: " + events;
        if (!state.executing.expired()) return {};
        const auto programs = autocmdPrograms(document_.get(), group, event, args.isEmpty() ? filePath_ : args);
        if (!programs.isEmpty()) {
            if (vimExFrames_.size() >= 100) return "E169: Command too recursive";
            auto guard = std::make_shared<int>();
            state.executing = guard;
            beginVimExProgram(programs, std::move(guard));
        }
        return {};
    }

    QStringList names;
    if (!events.isEmpty() && events != "*") {
        for (const auto& item : events.split(',')) {
            const auto event = eventName(item);
            if (event.isEmpty()) return "E216: No such event: " + item;
            names.append(event);
        }
        names.removeDuplicates();
    }
    const auto patterns = takeWord(args);
    auto split = patterns.split(',', Qt::SkipEmptyParts);
    split.removeDuplicates();
    if (!args.isEmpty() && (names.isEmpty() || split.isEmpty())) return "E474: Event and pattern required";
    if (group.isEmpty()) group = state.selected;
    const auto selected = [&](const Autocmd& entry) {
        return (group.isEmpty() ? (!invocation.bang || entry.group.isEmpty()) : entry.group == group)
            && (names.isEmpty() || names.contains(entry.event))
            && (split.isEmpty() || split.contains(entry.pattern))
            && (entry.pattern != "<buffer>" || split.isEmpty() || entry.buffer == document_.get());
    };

    state.entries.removeIf([](const auto& entry) { return entry.local && entry.buffer.isNull(); });
    const auto removed = invocation.bang ? std::count_if(state.entries.cbegin(), state.entries.cend(), selected) : 0;
    if (!args.isEmpty() && state.entries.size() - removed + names.size() * split.size() > 1024)
        return "E169: Too many autocommands";
    if (invocation.bang) state.entries.removeIf(selected);
    if (args.isEmpty()) {
        if (!invocation.bang) {
            QStringList listing;
            for (const auto& entry : state.entries)
                if (selected(entry)) listing.append(entry.group + ' ' + entry.event + ' ' + entry.pattern + "  " + entry.body);
            vimExMessage(listing.isEmpty() ? QStringLiteral("No autocommands") : listing.join('\n').left(4096));
        }
        return {};
    }
    for (const auto& event : names) {
        for (const auto& pattern : split) {
            Autocmd entry;
            entry.group = group;
            entry.event = event;
            entry.pattern = pattern;
            entry.body = args;
            entry.local = pattern == "<buffer>";
            if (entry.local) entry.buffer = document_.get();
            else entry.matcher = QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern));
            state.entries.append(std::move(entry));
        }
    }
    return {};
}

void EditorWidget::queueVimAutocmd(const QString& event, const QString& path) {
    if (!vim_.enabled() || !autocmds().executing.expired()) return;
    const auto programs = autocmdPrograms(document_.get(), {}, event, path.isEmpty() ? filePath_ : path);
    if (vimPendingAutocmds_.size() + programs.size() > 1024) return;
    vimPendingAutocmds_.append(programs);
    scheduleVimAutocmd();
}

bool EditorWidget::afterVimAutocmds(std::function<void()> completed) {
    if (vimPendingAutocmds_.isEmpty()) return false;
    vimAfterAutocmds_ = std::move(completed);
    return true;
}

bool EditorWidget::startPendingVimAutocmd() {
    auto& state = autocmds();
    if (vimPendingAutocmds_.isEmpty() || !state.executing.expired() || vimExFrames_.size() >= 100) return false;
    auto guard = std::make_shared<int>();
    state.executing = guard;
    beginVimExProgram(std::exchange(vimPendingAutocmds_, {}), std::move(guard));
    return true;
}

void EditorWidget::scheduleVimAutocmd() {
    if (!vim_.enabled() || vimPendingAutocmds_.isEmpty() || vimAutocmdScheduled_
        || busy_ || vimExActive_ || vimExExecuting_ || isVimReplaying()) return;
    vimAutocmdScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        vimAutocmdScheduled_ = false;
        if (!vim_.enabled() || busy_ || vimExActive_ || vimExExecuting_ || isVimReplaying()) return;
        if (!autocmds().executing.expired()) {
            // An event already queued by another editor still runs after the current handler.
            QTimer::singleShot(50, this, &EditorWidget::scheduleVimAutocmd);
            return;
        }
        if (startPendingVimAutocmd()) continueVimEx();
    });
}
