#include "EditorWidget.hpp"
#include "app/I18n.hpp"

#include <QApplication>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <algorithm>
#include <utility>

namespace {
enum class Scope { Global, Buffer, BufferGlobal, Window };
struct Option {
    QString name, alias;
    QVariant initial;
    Scope scope{Scope::Global};
    int minimum{}, maximum{};
    bool commaList{};
};

const QList<Option>& options() {
    static const QList<Option> list{
        {"dictionary", "dict", QString{}, Scope::BufferGlobal, 0, 0, true},
        {"thesaurus", "tsr", QString{}, Scope::BufferGlobal, 0, 0, true},
        {"tags", "tag", QStringLiteral("./tags,tags"), Scope::BufferGlobal, 0, 0, true},
        {"nrformats", "nf", QStringLiteral("bin,hex"), Scope::Buffer, 0, 0, true},
        {"textwidth", "tw", 0, Scope::Buffer, 0, 1000000},
        {"joinspaces", "js", false},
        {"ignorecase", "ic", false},
        {"smartcase", "scs", false},
        {"magic", "", true},
        {"hlsearch", "hls", true},
        {"incsearch", "is", true},
        {"wrapscan", "ws", true},
        {"gdefault", "gd", false},
        {"timeout", "to", true},
        {"timeoutlen", "tm", 1000, Scope::Global, 0, 60000},
        {"wrap", "", false, Scope::Window},
        {"foldenable", "fen", true, Scope::Window},
        {"foldmethod", "fdm", QStringLiteral("manual"), Scope::Window},
        {"tabstop", "ts", 4, Scope::Buffer, 1, 256},
        {"shiftwidth", "sw", 4, Scope::Buffer, 0, 256},
        {"expandtab", "et", true, Scope::Buffer},
    };
    return list;
}

QVariant parseValue(const Option& option, const QString& value) {
    if (option.initial.typeId() == QMetaType::Bool) {
        if (value == "true" || value == "1") return true;
        if (value == "false" || value == "0") return false;
        return {};
    }
    if (option.initial.typeId() == QMetaType::Int) {
        bool ok = false;
        const int number = value.toInt(&ok);
        return ok && number >= option.minimum && number <= option.maximum ? QVariant(number) : QVariant{};
    }
    if (option.name == "foldmethod" && value != "manual") return {};
    if (option.name == "nrformats") {
        for (const auto& format : value.split(',', Qt::SkipEmptyParts))
            if (!QStringList{"bin", "hex", "octal", "alpha", "unsigned"}.contains(format)) return {};
    }
    return value;
}

void applySessionOption(const QString& name, const QVariant& value) {
    auto& state = VimState::Session::instance();
    if (name == "joinspaces") state.joinSpaces = value.toBool();
    else if (name == "ignorecase") state.searchOptions.ignoreCase = value.toBool();
    else if (name == "smartcase") state.searchOptions.smartCase = value.toBool();
    else if (name == "magic") state.searchOptions.magic = value.toBool();
    else if (name == "hlsearch") state.highlightSearch = value.toBool();
    else if (name == "incsearch") state.incrementalSearch = value.toBool();
    else if (name == "wrapscan") state.wrapSearch = value.toBool();
    else if (name == "gdefault") state.globalSubstitute = value.toBool();
}

QMap<QString, QVariant>& globalOptions() {
    static auto values = [] {
        QMap<QString, QVariant> result;
        QSettings settings;
        settings.beginGroup("vimOptions");
        for (const auto& option : options()) {
            if (!settings.contains(option.name)) continue;
            const auto value = parseValue(option, settings.value(option.name).toString());
            if (!value.isValid()) continue;
            result.insert(option.name, value);
            if (option.scope == Scope::Global) applySessionOption(option.name, value);
        }
        return result;
    }();
    return values;
}

QString optionText(const Option& option, const QVariant& value) {
    if (option.initial.typeId() == QMetaType::Bool) return value.toBool() ? option.name : "no" + option.name;
    return option.name + '=' + value.toString();
}

QStringList optionArguments(const QString& text) {
    QStringList result;
    QString token;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const auto ch = text[i];
        if (ch == '\\' && i + 1 < text.size() && (text[i + 1].isSpace() || text[i + 1] == '\\'
                                                   || text[i + 1] == '|' || text[i + 1] == '"')) {
            token += text[++i];
        } else if (ch.isSpace()) {
            if (!token.isEmpty()) result.append(std::exchange(token, {}));
        } else token += ch;
    }
    if (!token.isEmpty()) result.append(token);
    return result;
}

QVariant editValue(const Option& option, const QVariant& current, const QString& operation,
                   const QString& argument) {
    auto value = parseValue(option, argument);
    if (!value.isValid() || operation == "=" || operation == ":") return value;
    if (option.initial.typeId() == QMetaType::Int) {
        const auto old = current.toLongLong();
        const auto operand = value.toLongLong();
        const auto result = operation == "+=" ? old + operand : operation == "-=" ? old - operand : old * operand;
        return parseValue(option, QString::number(result));
    }
    auto result = current.toString();
    if (option.commaList) {
        auto list = result.split(',', Qt::SkipEmptyParts);
        const auto parts = argument.split(',', Qt::SkipEmptyParts);
        if (operation == "-=") for (const auto& part : parts) list.removeAll(part);
        else if (operation == "+=") list += parts;
        else list = parts + list;
        list.removeDuplicates();
        result = list.join(',');
    } else if (operation == "+=") result += argument;
    else if (operation == "^=") result.prepend(argument);
    else result.remove(argument);
    return parseValue(option, result);
}
}

void EditorWidget::initializeVimOptions() {
    const auto& values = globalOptions();
    if (!vimViewOptionsInitialized_) {
        vimViewOptionsInitialized_ = true;
        vimWrap_ = values.value("wrap", false).toBool();
        vimFoldEnabled_ = values.value("foldenable", true).toBool();
    }
    if (!document_->vimOptionsInitialized) {
        document_->vimOptionsInitialized = true;
        for (const auto& option : options())
            if (option.scope == Scope::Buffer && values.contains(option.name))
                document_->vimOptions.insert(option.name, values.value(option.name));
    }
}

void EditorWidget::applyVimIndentOptions() {
    const auto& values = document_->vimOptions;
    if (!values.contains("tabstop") && !values.contains("shiftwidth") && !values.contains("expandtab")) return;
    const auto tabWidth = values.value("tabstop", editorConfig_.tabSize()).toInt();
    const auto shiftWidth = values.value("shiftwidth", editorConfig_.indentation()).toInt();
    editorConfig_.tabWidth = tabWidth;
    editorConfig_.indentSize = shiftWidth ? shiftWidth : tabWidth;
    if (values.contains("expandtab")) editorConfig_.tabs = !values.value("expandtab").toBool();
}

QVariant EditorWidget::vimBufferOption(const QString& name) const {
    if (const auto value = document_->vimOptions.constFind(name); value != document_->vimOptions.cend())
        return value.value();
    for (const auto& option : options()) {
        if (option.name != name) continue;
        return option.scope == Scope::BufferGlobal ? globalOptions().value(name, option.initial) : option.initial;
    }
    return {};
}

int EditorWidget::vimMappingTimeout() const {
    const auto& values = globalOptions();
    return values.value("timeout", true).toBool() ? values.value("timeoutlen", 1000).toInt() : -1;
}

QStringList EditorWidget::completeVimOptions(const QString& prefix) {
    QStringList result;
    for (const auto& option : options()) {
        if (option.name.startsWith(prefix) || (!prefix.isEmpty() && option.alias.startsWith(prefix)))
            result.append(option.name);
        if (option.initial.typeId() == QMetaType::Bool) {
            if (("no" + option.name).startsWith(prefix) && prefix.startsWith("no")) result.append("no" + option.name);
            if (("inv" + option.name).startsWith(prefix) && prefix.startsWith("inv")) result.append("inv" + option.name);
        }
    }
    return result;
}

QString EditorWidget::executeVimSet(const VimEx::Invocation& command) {
    const bool globalOnly = command.definition.name == "setglobal";
    const bool localOnly = command.definition.name == "setlocal";
    auto& global = globalOptions();
    auto globals = global;
    auto buffer = document_->vimOptions;
    QMap<QString, QVariant> window{{"wrap", vimWrap_}, {"foldenable", vimFoldEnabled_}};
    QSet<QString> globalChanged;
    QStringList reports;
    const auto base = EditorConfig::resolve(filePath_);
    const auto localDefault = [&](const Option& option) -> QVariant {
        if (option.name == "tabstop") return base.tabSize();
        if (option.name == "shiftwidth") return base.indentation();
        if (option.name == "expandtab") return !base.tabs.value_or(false);
        return option.initial;
    };
    const auto current = [&](const Option& option) {
        if (globalOnly || option.scope == Scope::Global) return globals.value(option.name, option.initial);
        if (option.scope == Scope::Window) return window.value(option.name, localDefault(option));
        const auto fallback = option.scope == Scope::BufferGlobal ? globals.value(option.name, option.initial) : localDefault(option);
        return buffer.value(option.name, fallback);
    };
    const auto arguments = optionArguments(command.arguments);
    if (arguments.isEmpty() || arguments == QStringList{"all"}) {
        for (const auto& option : options()) {
            const auto value = current(option);
            const auto initial = globalOnly ? option.initial : localDefault(option);
            if (value != initial || !arguments.isEmpty()) reports.append(optionText(option, value));
        }
        if (!reports.isEmpty()) vimExMessage(reports.join("  "));
        return {};
    }
    static const QRegularExpression syntax(QStringLiteral("^([a-z]+)([?!&]|[-+^]?=|:|<)?(.*)$"));
    for (const auto& argument : arguments) {
        const auto match = syntax.match(argument);
        if (!match.hasMatch()) return "E474: Invalid argument: " + argument;
        auto name = match.captured(1);
        const auto operation = match.captured(2);
        const auto valueText = match.captured(3);
        const bool negated = name.startsWith("no");
        const bool inverted = name.startsWith("inv");
        if (negated || inverted) name.remove(0, negated ? 2 : 3);
        const auto it = std::find_if(options().cbegin(), options().cend(), [&](const auto& option) {
            return option.name == name || (!option.alias.isEmpty() && option.alias == name);
        });
        if (it == options().cend()) return "E518: Unknown option: " + argument;
        const auto& option = *it;
        const bool boolean = option.initial.typeId() == QMetaType::Bool;
        const auto old = current(option);
        if ((negated || inverted) && (!boolean || !operation.isEmpty())) return "E474: Invalid argument: " + argument;
        if (!valueText.isEmpty() && !operation.endsWith('=') && operation != ":")
            return "E474: Invalid argument: " + argument;
        if (operation == "?" || (operation.isEmpty() && !boolean)) {
            reports.append(optionText(option, old));
            continue;
        }

        QVariant value;
        if (operation == "&") value = globalOnly || option.scope == Scope::Global ? option.initial : localDefault(option);
        else if (operation == "<") value = globals.value(option.name, option.initial);
        else if (boolean && (operation.isEmpty() || operation == "!"))
            value = inverted || operation == "!" ? !old.toBool() : !negated;
        else if (!boolean && (operation.endsWith('=') || operation == ":"))
            value = editValue(option, old, operation, valueText);
        if (!value.isValid()) return "E474: Invalid argument: " + argument;

        if (option.scope == Scope::Global || (!localOnly && operation != "<")) {
            if (operation == "&") globals.remove(option.name);
            else globals.insert(option.name, value);
            globalChanged.insert(option.name);
        }
        if (!globalOnly && (option.scope == Scope::Buffer || option.scope == Scope::BufferGlobal)) {
            if ((option.scope == Scope::BufferGlobal && !localOnly)
                || operation == "&" || (operation == "<" && (option.scope == Scope::BufferGlobal
                                                             || !globals.contains(option.name)))) buffer.remove(option.name);
            else buffer.insert(option.name, value);
        }
        if (!globalOnly && option.scope == Scope::Window) window.insert(option.name, value);
    }

    if (!globalChanged.isEmpty()) {
        QSettings settings;
        settings.beginGroup("vimOptions");
        for (const auto& option : options()) {
            if (!globalChanged.contains(option.name)) continue;
            if (globals.contains(option.name)) settings.setValue(option.name, globals.value(option.name));
            else settings.remove(option.name);
        }
        settings.sync();
        if (settings.status() != QSettings::NoError) return I18n::text("Cannot save Vim options.");
        global = std::move(globals);
        for (const auto& option : options())
            if (option.scope == Scope::Global && globalChanged.contains(option.name))
                applySessionOption(option.name, global.value(option.name, option.initial));
    }
    if (buffer != document_->vimOptions) {
        document_->vimOptions = std::move(buffer);
        for (auto* widget : QApplication::allWidgets())
            if (auto* editor = qobject_cast<EditorWidget*>(widget); editor && editor->document_ == document_)
                editor->refreshEditorConfig();
    }
    const bool wrap = window.value("wrap").toBool(), fold = window.value("foldenable").toBool();
    if (wrap != vimWrap_ || fold != vimFoldEnabled_) {
        const int top = documentLineForVisualRow(verticalScrollBar()->value());
        vimWrap_ = wrap;
        vimFoldEnabled_ = fold;
        refreshScreenLayout(top);
        const int line = documentLineForVisualRow(visualRowForLine(int(currentLine())));
        if (line != int(currentLine())) moveCursor(VimMotions::firstText(buffer_, line), false);
        ensureCursorVisible();
    }
    if (globalChanged.contains("ignorecase") || globalChanged.contains("smartcase")
        || globalChanged.contains("magic") || globalChanged.contains("hlsearch")) {
        for (auto* widget : QApplication::allWidgets()) {
            if (auto* editor = qobject_cast<EditorWidget*>(widget)) {
                editor->vimHighlightHidden_ = false;
                editor->invalidateVimSearch();
            }
        }
    }
    if (!reports.isEmpty()) vimExMessage(reports.join("  "));
    return {};
}
