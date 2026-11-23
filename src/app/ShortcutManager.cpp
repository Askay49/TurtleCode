#include "app/IconDialogs.hpp"
#include "app/I18n.hpp"
#include "ShortcutManager.hpp"
#include <QAction>
#include <QShortcut>
#include <QKeyEvent>
#include <QSettings>
#include <QDialog>
#include <QDialogButtonBox>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QFileDialog>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
namespace {
bool overlaps(const QKeySequence& a, const QKeySequence& b) {
    return !a.isEmpty() && !b.isEmpty() &&
        (a.matches(b) != QKeySequence::NoMatch || b.matches(a) != QKeySequence::NoMatch);
}
bool reserved(const QKeySequence& sequence) {
    if (sequence.isEmpty()) return false;
    const auto first = sequence[0];
    const auto key = first.key();
    const auto mods = first.keyboardModifiers();
    // Keep text input, clipboard, undo, line selection and normal navigation intact.
    if (!(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
        !(key >= Qt::Key_F1 && key <= Qt::Key_F35)) return true;
    const QStringList fixed{"Ctrl+A", "Ctrl+C", "Ctrl+X", "Ctrl+V", "Ctrl+Z", "Ctrl+Y", "Ctrl+Shift+Z", "Ctrl+L",
        "Ctrl+Left", "Ctrl+Right", "Ctrl+Up", "Ctrl+Down", "Ctrl+Shift+Left", "Ctrl+Shift+Right",
        "Ctrl+Home", "Ctrl+End", "Ctrl+Shift+Home", "Ctrl+Shift+End", "Ctrl+Backspace", "Ctrl+Delete", "Shift+Delete", "Ctrl+Insert", "Shift+Insert"};
    const QKeySequence prefix(first);
    for (const auto& value : fixed) if (prefix == QKeySequence(value)) return true;
    return key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter;
}

class NavigationPrefixEdit final : public QLineEdit {
public:
    explicit NavigationPrefixEdit(QWidget* parent) : QLineEdit(parent) {
        setObjectName("navigationPrefixKey");
        setReadOnly(true);
        setPlaceholderText(I18n::text("Disabled"));
        setToolTip(I18n::text("Press one key combination. Backspace clears it."));
    }

    QKeySequence sequence() const { return sequence_; }
    void setSequence(const QKeySequence& sequence) {
        sequence_ = sequence;
        setText(sequence.toString(QKeySequence::NativeText));
    }

protected:
    bool event(QEvent* event) override {
        if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress)
            return QLineEdit::event(event);
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt: case Qt::Key_Meta:
        case Qt::Key_AltGr: case Qt::Key_unknown:
        case Qt::Key_Escape: case Qt::Key_Return: case Qt::Key_Enter:
            return QLineEdit::event(event);
        default:
            break;
        }
        if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
            return QLineEdit::event(event);
        if (event->type() == QEvent::KeyPress) {
            const bool clear = key->modifiers() == Qt::NoModifier &&
                (key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete);
            setSequence(clear ? QKeySequence{} : QKeySequence(key->keyCombination()));
        }
        event->accept();
        return true;
    }

private:
    QKeySequence sequence_;
};
}
ShortcutManager& ShortcutManager::instance() { static ShortcutManager manager; return manager; }
ShortcutManager::ShortcutManager() {
    auto add = [this](const char* id, const char* name, const char* category, const char* key = "", const char* scope = "Window") {
        commands_.push_back({id, I18n::text(name), I18n::text(category), I18n::text(scope), QString::fromLatin1(name), QKeySequence(QString::fromLatin1(key), QKeySequence::PortableText)});
    };
    add("file.open", "Open file", "File", "Ctrl+O");
    add("file.openFolder", "Open folder", "File", "Ctrl+K, Ctrl+O");
    add("file.save", "Save file", "File", "Ctrl+S");
    add("file.saveAll", "Save all files", "File", "Ctrl+Alt+S");
    add("file.saveAs", "Save file as", "File", "Ctrl+Shift+S");
    add("file.quit", "Quit", "File", "Ctrl+Q");
    add("search.project", "Search in files", "Search", "Ctrl+Shift+F");
    add("search.find", "Find in current file", "Search", "Ctrl+F");
    add("search.next", "Next match", "Search", "F3");
    add("search.previous", "Previous match", "Search", "Shift+F3");
    add("search.replace", "Replace in current file", "Search");
    add("code.formatDocument", "Format document or selection", "Code", "Ctrl+Shift+I");
    add("code.complete", "Complete", "Code", "Ctrl+Space");
    add("code.rename", "Rename symbol", "Code", "F2");
    add("code.problems", "Errors and warnings", "Code", "Ctrl+Shift+M");
    add("code.hover", "Symbol information", "Code", "F1");
    add("code.signature", "Function parameters", "Code", "Ctrl+Shift+Space");
    add("settings.lsp", "Language servers", "Settings");
    add("code.declaration", "Go to Declaration", "Code");
    add("code.typeDefinition", "Go to Type Definition", "Code");
    add("code.references", "Find All References", "Code", "Shift+F12");
    add("code.definition", "Go to definition", "Code", "F12");
    add("navigation.quickOpen", "Quick open", "Navigation", "Ctrl+P");
    add("navigation.documentSymbols", "Go to Symbol in File", "Navigation", "Ctrl+Shift+O");
    add("navigation.projectSymbols", "Go to Symbol in Project", "Navigation", "Ctrl+Alt+P");
    add(navigationPrefixId, "Focus navigation prefix", "Navigation", "Ctrl+'");
    add("navigation.line", "Go to line", "Navigation", "Ctrl+G");
    add("navigation.tabNext", "Next recent tab", "Navigation", "Ctrl+Tab");
    add("navigation.tabPrevious", "Previous recent tab", "Navigation", "Ctrl+Shift+Tab");
    add("navigation.back", "Back", "Navigation", "Alt+Left");
    add("navigation.forward", "Forward", "Navigation", "Alt+Right");
    add("view.hidden", "Show hidden files", "Files and search", "Ctrl+H");
    add("view.focusEditor", "Focus Editor", "View", "F6");
    add("view.terminal", "Toggle terminal", "View", "Ctrl+`");
    add("view.focusTerminal", "Toggle Terminal Focus", "View", "Ctrl+Shift+T");
    add("view.debug", "Run and Debug", "View", "Ctrl+Shift+D");
    add("debug.start", "Start / Continue Debugging", "Run and Debug", "F5");
    add("debug.stop", "Stop Debugging", "Run and Debug", "Shift+F5");
    add("debug.next", "Step Over", "Run and Debug", "F10");
    add("debug.stepIn", "Step Into", "Run and Debug", "F11");
    add("debug.stepOut", "Step Out", "Run and Debug", "Shift+F11");
    add("debug.breakpoint", "Toggle breakpoint at cursor", "Run and Debug", "F9");
    add("view.git", "Git", "View", "Ctrl+Shift+G");
    add("view.explorer", "Toggle sidebar", "View", "Ctrl+B");
    add("view.focusProjectTree", "Toggle Project Tree Focus", "View", "Ctrl+Shift+E");
    add("view.revealActiveFile", "Reveal Active File in Project Tree", "View");
    add("settings.interfaceLanguage", "Interface language", "Settings");
    add("settings.theme", "Theme", "Settings");
    add("settings.languages", "Syntax highlighting", "Settings");
    add("settings.shortcuts", "Keyboard shortcuts", "Settings");
    add("editor.toggleLineComment", "Toggle line comment", "Editor", "Ctrl+/", "Editor");
    add("editor.toggleBlockComment", "Toggle block comment", "Editor", "Ctrl+Shift+A", "Editor");
    add("editor.copyLineUp", "Copy line up", "Editor", "Alt+Shift+Up", "Editor");
    add("editor.copyLineDown", "Copy line down", "Editor", "Alt+Shift+Down", "Editor");
    add("editor.moveLineUp", "Move line up", "Editor", "Alt+Up", "Editor");
    add("editor.moveLineDown", "Move line down", "Editor", "Alt+Down", "Editor");
    add("editor.cursorAbove", "Add cursor above", "Editor", "Ctrl+Shift+Up", "Editor");
    add("editor.cursorBelow", "Add cursor below", "Editor", "Ctrl+Shift+Down", "Editor");
    add("editor.nextOccurrence", "Select next occurrence", "Editor", "Ctrl+D", "Editor");
    add("editor.splitBelow", "Split Editor Below", "Editor");
    add("editor.splitRight", "Split Editor Right", "Editor");
    add("editor.closePane", "Close Editor Pane", "Editor");

    add("navigation.commands", "Command palette", "Navigation", "Ctrl+Shift+P");
    add("settings.open", "Open Settings", "Settings");
    add("settings.editor", "Editor settings", "Settings");
    add("settings.vimMode", "Vim mode", "Settings");
    add("view.minimap", "Minimap", "View");
    add("view.outline", "Outline", "View");
    add("view.ignored", "Include ignored files", "Files and search");

    QSettings settings; settings.beginGroup("shortcuts");
    for (const auto& command : commands_) if (settings.contains(command.id))
        overrides_[command.id] = QKeySequence(settings.value(command.id).toString(), QKeySequence::PortableText);
    // Keep shortcuts assigned before these navigation defaults were added.
    for (const auto id : {navigationPrefixId, "navigation.documentSymbols", "navigation.projectSymbols"}) {
        if (overrides_.contains(id)) continue;
        const auto key = sequence(id);
        if (std::any_of(overrides_.cbegin(), overrides_.cend(),
                        [&key](const QKeySequence& assigned) { return overlaps(key, assigned); }))
            overrides_.insert(id, {});
    }
    // Invalid settings must never break normal editing or introduce ambiguous shortcuts.
    QMap<QString, QKeySequence> values;
    for (const auto& command : commands_) values[command.id] = sequence(command.id);
    if (!validate(values).isEmpty()) overrides_.clear();
}
bool ShortcutManager::hasCommand(const QString& id) const {
    return std::any_of(commands_.begin(), commands_.end(), [&](const auto& command) { return command.id == id; });
}
QKeySequence ShortcutManager::sequence(const QString& id) const {
    if (overrides_.contains(id)) return overrides_.value(id);
    for (const auto& command : commands_) if (command.id == id) return command.standard;
    return {};
}
void ShortcutManager::bind(const QString& id, QAction* action) {
    bindings_.push_back({id, action, {}}); action->setShortcut(sequence(id));
}
void ShortcutManager::bind(const QString& id, QShortcut* shortcut) {
    bindings_.push_back({id, {}, shortcut}); shortcut->setKey(sequence(id));
}
QObject* ShortcutManager::target(const QString& id, QWidget* window, QWidget* editor) const {
    auto belongs = [](QObject* object, QObject* root) {
        for (; object; object = object->parent()) if (object == root) return true;
        return false;
    };
    for (const auto& binding : bindings_) {
        if (binding.id != id) continue;
        if (binding.action && belongs(binding.action, window)) return binding.action;
        if (binding.shortcut && (binding.shortcut->parent() == window ||
            (editor && belongs(binding.shortcut, editor)))) return binding.shortcut;
    }
    return nullptr;
}
bool ShortcutManager::matches(const QString& id, const QKeyEvent* event) const {
    const auto key = sequence(id);
    return key.count() == 1 && key[0] == event->keyCombination();
}
void ShortcutManager::refresh() {
    for (int i = bindings_.size()-1; i >= 0; --i) {
        auto& binding = bindings_[i];
        if (binding.action) binding.action->setShortcut(sequence(binding.id));
        else if (binding.shortcut) binding.shortcut->setKey(sequence(binding.id));
        else bindings_.removeAt(i);
    }
}
QString ShortcutManager::validate(const QMap<QString, QKeySequence>& values) const {
    for (int i = 0; i < commands_.size(); ++i) {
        const auto& a = commands_[i]; const auto key = values.value(a.id, a.standard);
        if (a.id == navigationPrefixId && key.count() > 1)
            return I18n::text("The focus navigation prefix requires one key combination.");
        if (reserved(key)) return a.name + I18n::text(": this shortcut is reserved for text editing or dialog navigation.");
        for (int j = i+1; j < commands_.size(); ++j) {
            const auto& b = commands_[j];
            // All editor commands can coexist with window commands; they must not share a prefix.
            if (overlaps(key, values.value(b.id, b.standard))) return I18n::text("Conflict: ") + a.name + " / " + b.name + " (" + key.toString() + ")";
        }
    }
    return {};
}
bool ShortcutManager::save(const QMap<QString, QKeySequence>& values, QString* error) {
    *error = validate(values); if (!error->isEmpty()) return false;
    QSettings settings;
    writeSettings(settings, values);
    settings.sync();
    if (settings.status() != QSettings::NoError) { *error = I18n::text("Could not save shortcut settings."); return false; }
    applySettings(values);
    return true;
}
void ShortcutManager::writeSettings(QSettings& settings, const QMap<QString, QKeySequence>& values) const {
    settings.beginGroup("shortcuts");
    for (const auto& command : commands_) {
        const auto key = values.value(command.id, command.standard);
        if (key == command.standard) settings.remove(command.id);
        else settings.setValue(command.id, key.toString(QKeySequence::PortableText));
    }
    settings.endGroup();
}
void ShortcutManager::applySettings(const QMap<QString, QKeySequence>& values) {
    overrides_.clear();
    for (const auto& command : commands_) {
        const auto key = values.value(command.id, command.standard);
        if (key != command.standard) overrides_.insert(command.id, key);
    }
    refresh();
}
void ShortcutManager::showSettings(QWidget* parent) {
    QMap<QString, QKeySequence> values;
    QDialog dialog(parent);
    dialog.setWindowTitle(I18n::text("Keyboard shortcuts"));
    dialog.resize(850, 650);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(createSettingsPage(&dialog, values));
    auto* buttons = new IconDialogs::ButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QString error;
        if (save(values, &error)) dialog.accept();
        else IconDialogs::MessageBox::warning(&dialog, I18n::text("Cannot save shortcuts"), error);
    });
    dialog.exec();
}
QWidget* ShortcutManager::createSettingsPage(QWidget* parent, QMap<QString, QKeySequence>& values) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    for (const auto& command : commands_) values[command.id] = sequence(command.id);
    auto* prefixRow = new QHBoxLayout;
    auto* prefixLabel = new QLabel(I18n::text("Focus navigation prefix"), page);
    auto* prefix = new NavigationPrefixEdit(page);
    prefix->setSequence(values[navigationPrefixId]);
    prefixLabel->setBuddy(prefix);
    prefixRow->addWidget(prefixLabel);
    prefixRow->addWidget(prefix);
    prefixRow->addStretch();
    layout->addLayout(prefixRow);
    auto* note = new QLabel(I18n::text("Select a command and press a new shortcut below. Text input, clipboard and normal cursor navigation stay fixed."), page);
    note->setWordWrap(true); layout->addWidget(note);
    auto* search = new QLineEdit(page); search->setPlaceholderText(I18n::text("Search commands…")); layout->addWidget(search);
    auto listedCommands = commands_;
    listedCommands.removeIf([](const ShortcutCommand& command) { return command.id == navigationPrefixId; });
    auto* table = new QTableWidget(listedCommands.size(), 5, page);
    table->setObjectName("shortcutSettingsTable");
    table->setHorizontalHeaderLabels({I18n::text("Category"), I18n::text("Command"), I18n::text("Shortcut"), I18n::text("Default"), I18n::text("Scope")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); layout->addWidget(table);
    auto refreshTable = [listedCommands, table, &values] {
        for (int row = 0; row < listedCommands.size(); ++row) {
            const auto& c = listedCommands[row]; const QStringList columns{c.category, c.name, values[c.id].toString(), c.standard.toString(), c.scope};
            for (int col = 0; col < columns.size(); ++col) table->setItem(row, col, new QTableWidgetItem(columns[col]));
        }
    }; refreshTable();
    auto* edit = new QKeySequenceEdit(page); layout->addWidget(edit);
    auto* status = new QLabel(page); status->setWordWrap(true); layout->addWidget(status);
    auto* row = new QHBoxLayout; layout->addLayout(row);
    auto button = [page](QHBoxLayout* layout, const QString& label, const char* icon) {
        auto* button = new QPushButton(Codicons::icon(QString::fromLatin1(icon)), label, page);
        layout->addWidget(button);
        return button;
    };
    auto* assign = button(row, I18n::text("Assign"), "check");
    auto* clear = button(row, I18n::text("Clear"), "clear-all");
    auto* reset = button(row, I18n::text("Reset selected"), "discard");
    auto* resetAll = button(row, I18n::text("Reset all"), "discard");
    row->addStretch();
    auto* transfer = new QHBoxLayout;
    layout->addLayout(transfer);
    auto* import = button(transfer, I18n::text("Import…"), "folder-opened");
    auto* exportButton = button(transfer, I18n::text("Export…"), "export");
    transfer->addStretch();
    auto update = [this, refreshTable, prefix, status, &values] {
        const QSignalBlocker blocker(prefix);
        prefix->setSequence(values[navigationPrefixId]);
        refreshTable();
        status->setText(validate(values));
    };
    QObject::connect(prefix, &QLineEdit::textChanged, page, [this, prefix, status, &values] {
        values[navigationPrefixId] = prefix->sequence();
        status->setText(validate(values));
    });
    QObject::connect(table, &QTableWidget::currentCellChanged, page, [listedCommands, edit, &values](int current, int, int, int) {
        if (current >= 0) edit->setKeySequence(values[listedCommands[current].id]);
    });
    QObject::connect(search, &QLineEdit::textChanged, page, [listedCommands, table](const QString& text) {
        for (int i = 0; i < listedCommands.size(); ++i)
            table->setRowHidden(i, !(listedCommands[i].name + " " + listedCommands[i].category + " " + listedCommands[i].id).contains(text, Qt::CaseInsensitive));
    });
    auto setSelected = [listedCommands, table, edit, update, &values](const QKeySequence& key) {
        const int index = table->currentRow(); if (index < 0) return;
        values[listedCommands[index].id] = key; edit->setKeySequence(key); update();
    };
    QObject::connect(assign, &QPushButton::clicked, page, [setSelected, edit] { setSelected(edit->keySequence()); });
    QObject::connect(clear, &QPushButton::clicked, page, [setSelected] { setSelected({}); });
    QObject::connect(reset, &QPushButton::clicked, page, [listedCommands, table, setSelected] { if (table->currentRow() >= 0) setSelected(listedCommands[table->currentRow()].standard); });
    QObject::connect(resetAll, &QPushButton::clicked, page, [this, page, update, &values] {
        if (IconDialogs::MessageBox::question(page, I18n::text("Reset shortcuts"), I18n::text("Restore all default shortcuts?")) != QMessageBox::Yes) return;
        for (const auto& c : commands_) values[c.id] = c.standard; update();
    });
    QObject::connect(exportButton, &QPushButton::clicked, page, [this, page, &values] {
        const auto error = validate(values); if (!error.isEmpty()) { IconDialogs::MessageBox::warning(page, I18n::text("Shortcuts"), error); return; }
        auto path = IconDialogs::FileDialog::getSaveFileName(page, I18n::text("Export shortcuts"), "shortcuts.json", "JSON (*.json)"); if (path.isEmpty()) return;
        if (!path.endsWith(".json", Qt::CaseInsensitive)) path += ".json";
        QJsonObject shortcuts; for (auto it = values.cbegin(); it != values.cend(); ++it) shortcuts[it.key()] = it.value().toString(QKeySequence::PortableText);
        QSaveFile file(path); const auto data = QJsonDocument(QJsonObject{{"formatVersion", 1}, {"shortcuts", shortcuts}}).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) IconDialogs::MessageBox::warning(page, I18n::text("Export failed"), file.errorString());
    });
    QObject::connect(import, &QPushButton::clicked, page, [this, page, update, &values] {
        const auto path = IconDialogs::FileDialog::getOpenFileName(page, I18n::text("Import shortcuts"), {}, "JSON (*.json)"); if (path.isEmpty()) return;
        QFile file(path); if (!file.open(QIODevice::ReadOnly) || file.size() > 1024*1024) { IconDialogs::MessageBox::warning(page, I18n::text("Import failed"), I18n::text("Cannot read file or file is too large.")); return; }
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(file.readAll(), &error); const auto object = doc.object();
        if (error.error != QJsonParseError::NoError || object.value("formatVersion").toInt() != 1 || !object.value("shortcuts").isObject()) { IconDialogs::MessageBox::warning(page, I18n::text("Import failed"), I18n::text("Invalid shortcut file format.")); return; }
        auto imported = values; const auto keys = object.value("shortcuts").toObject(); QString problem;
        for (auto it = keys.begin(); it != keys.end(); ++it) {
            if (!imported.contains(it.key())) { problem = I18n::text("Unknown command: ") + it.key(); break; }
            if (!it.value().isString()) { problem = I18n::text("Shortcut must be a string: ") + it.key(); break; }
            const auto text = it.value().toString(); const QKeySequence key(text, QKeySequence::PortableText);
            if (!text.isEmpty() && (key.isEmpty() || key.toString(QKeySequence::PortableText).isEmpty() || key[0].key() == Qt::Key_unknown)) { problem = I18n::text("Invalid shortcut: ") + text; break; }
            imported[it.key()] = key;
        }
        if (problem.isEmpty()) problem = validate(imported);
        if (!problem.isEmpty()) { IconDialogs::MessageBox::warning(page, I18n::text("Import failed"), problem); return; }
        values = imported; update();
    });
    table->selectRow(0);
    return page;
}
