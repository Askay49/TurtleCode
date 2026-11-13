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
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QFileDialog>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
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
}
ShortcutManager& ShortcutManager::instance() { static ShortcutManager manager; return manager; }
ShortcutManager::ShortcutManager() {
    auto add = [this](const char* id, const char* name, const char* category, const char* key = "", const char* scope = "Window") {
        commands_.push_back({id, I18n::text(name), I18n::text(category), I18n::text(scope), QKeySequence(QString::fromLatin1(key), QKeySequence::PortableText)});
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
    add("code.formatDocument", "Format document", "Code", "Ctrl+Shift+I");
    add("code.complete", "Complete", "Code", "Ctrl+Space");
    add("code.rename", "Rename symbol", "Code", "F2");
    add("code.problems", "Errors and warnings", "Code", "Ctrl+Shift+M");
    add("code.hover", "Symbol information", "Code", "F1");
    add("code.signature", "Function parameters", "Code", "Ctrl+Shift+Space");
    add("settings.lsp", "Language server", "Settings");
    add("code.declaration", "Go to Declaration", "Code");
    add("code.typeDefinition", "Go to Type Definition", "Code");
    add("code.references", "Find All References", "Code", "Shift+F12");
    add("code.definition", "Go to definition", "Code", "F12");
    add("navigation.quickOpen", "Quick open", "Navigation", "Ctrl+P");
    add("navigation.line", "Go to line", "Navigation", "Ctrl+G");
    add("navigation.tabNext", "Next recent tab", "Navigation", "Ctrl+Tab");
    add("navigation.tabPrevious", "Previous recent tab", "Navigation", "Ctrl+Shift+Tab");
    add("navigation.back", "Back", "Navigation", "Alt+Left");
    add("navigation.forward", "Forward", "Navigation", "Alt+Right");
    add("view.hidden", "Show hidden files", "View", "Ctrl+H");
    add("view.terminal", "Toggle terminal", "View", "Ctrl+`");
    add("view.debug", "Run and Debug", "View", "Ctrl+Shift+D");
    add("debug.start", "Start / Continue Debugging", "Run and Debug", "F5");
    add("debug.stop", "Stop Debugging", "Run and Debug", "Shift+F5");
    add("debug.next", "Step Over", "Run and Debug", "F10");
    add("debug.stepIn", "Step Into", "Run and Debug", "F11");
    add("debug.stepOut", "Step Out", "Run and Debug", "Shift+F11");
    add("debug.breakpoint", "Toggle breakpoint at cursor", "Run and Debug", "F9");
    add("view.git", "Git", "View", "Ctrl+Shift+G");
    add("view.explorer", "Toggle sidebar", "View", "Ctrl+B");
    add("settings.interfaceLanguage", "Interface language", "Settings");
    add("settings.theme", "Theme", "Settings");
    add("settings.languages", "Languages", "Settings");
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

    QSettings settings; settings.beginGroup("shortcuts");
    for (const auto& command : commands_) if (settings.contains(command.id))
        overrides_[command.id] = QKeySequence(settings.value(command.id).toString(), QKeySequence::PortableText);
    // Invalid settings must never break normal editing or introduce ambiguous shortcuts.
    QMap<QString, QKeySequence> values;
    for (const auto& command : commands_) values[command.id] = sequence(command.id);
    if (!validate(values).isEmpty()) overrides_.clear();
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
    QSettings settings; settings.beginGroup("shortcuts");
    QMap<QString, QKeySequence> overrides;
    for (const auto& command : commands_) {
        const auto key = values.value(command.id, command.standard);
        if (key == command.standard) settings.remove(command.id);
        else { settings.setValue(command.id, key.toString(QKeySequence::PortableText)); overrides[command.id] = key; }
    }
    settings.endGroup(); settings.sync();
    if (settings.status() != QSettings::NoError) { *error = I18n::text("Could not save shortcut settings."); return false; }
    overrides_ = overrides; refresh(); return true;
}
void ShortcutManager::showSettings(QWidget* parent) {
    QDialog dialog(parent); dialog.setWindowTitle(I18n::text("Keyboard shortcuts")); dialog.resize(850, 650);
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(I18n::text("Select a command and press a new shortcut below. Text input, clipboard and normal cursor navigation stay fixed."), &dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto* search = new QLineEdit(&dialog); search->setPlaceholderText(I18n::text("Search commands…")); layout->addWidget(search);
    auto* table = new QTableWidget(commands_.size(), 5, &dialog);
    table->setHorizontalHeaderLabels({I18n::text("Category"), I18n::text("Command"), I18n::text("Shortcut"), I18n::text("Default"), I18n::text("Scope")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); layout->addWidget(table);
    QMap<QString, QKeySequence> values;
    for (const auto& command : commands_) values[command.id] = sequence(command.id);
    auto refreshTable = [&] {
        for (int row = 0; row < commands_.size(); ++row) {
            const auto& c = commands_[row]; const QStringList columns{c.category, c.name, values[c.id].toString(), c.standard.toString(), c.scope};
            for (int col = 0; col < columns.size(); ++col) table->setItem(row, col, new QTableWidgetItem(columns[col]));
        }
    }; refreshTable();
    auto* edit = new QKeySequenceEdit(&dialog); layout->addWidget(edit);
    auto* status = new QLabel(&dialog); status->setWordWrap(true); layout->addWidget(status);
    auto* row = new QHBoxLayout; layout->addLayout(row);
    auto button = [&](const QString& label, const char* icon) { auto* b = new QPushButton(Codicons::icon(QString::fromLatin1(icon)), label, &dialog); row->addWidget(b); return b; };
    auto* assign = button(I18n::text("Assign"), "check"); auto* clear = button(I18n::text("Clear"), "clear-all"); auto* reset = button(I18n::text("Reset selected"), "discard"); auto* resetAll = button(I18n::text("Reset all"), "discard");
    auto* import = button(I18n::text("Import…"), "folder-opened"); auto* exportButton = button(I18n::text("Export…"), "export");
    auto update = [&] { refreshTable(); status->setText(validate(values)); };
    QObject::connect(table, &QTableWidget::currentCellChanged, &dialog, [&](int current, int, int, int) {
        if (current >= 0) edit->setKeySequence(values[commands_[current].id]);
    });
    QObject::connect(search, &QLineEdit::textChanged, &dialog, [&](const QString& text) {
        for (int i = 0; i < commands_.size(); ++i) table->setRowHidden(i, !(commands_[i].name + " " + commands_[i].category + " " + commands_[i].id).contains(text, Qt::CaseInsensitive));
    });
    auto setSelected = [&](const QKeySequence& key) {
        const int index = table->currentRow(); if (index < 0) return;
        values[commands_[index].id] = key; edit->setKeySequence(key); update();
    };
    QObject::connect(assign, &QPushButton::clicked, &dialog, [&] { setSelected(edit->keySequence()); });
    QObject::connect(clear, &QPushButton::clicked, &dialog, [&] { setSelected({}); });
    QObject::connect(reset, &QPushButton::clicked, &dialog, [&] { if (table->currentRow() >= 0) setSelected(commands_[table->currentRow()].standard); });
    QObject::connect(resetAll, &QPushButton::clicked, &dialog, [&] {
        if (IconDialogs::MessageBox::question(&dialog, I18n::text("Reset shortcuts"), I18n::text("Restore all default shortcuts?")) != QMessageBox::Yes) return;
        for (const auto& c : commands_) values[c.id] = c.standard; update();
    });
    QObject::connect(exportButton, &QPushButton::clicked, &dialog, [&] {
        const auto error = validate(values); if (!error.isEmpty()) { IconDialogs::MessageBox::warning(&dialog, I18n::text("Shortcuts"), error); return; }
        auto path = IconDialogs::FileDialog::getSaveFileName(&dialog, I18n::text("Export shortcuts"), "shortcuts.json", "JSON (*.json)"); if (path.isEmpty()) return;
        if (!path.endsWith(".json", Qt::CaseInsensitive)) path += ".json";
        QJsonObject shortcuts; for (auto it = values.cbegin(); it != values.cend(); ++it) shortcuts[it.key()] = it.value().toString(QKeySequence::PortableText);
        QSaveFile file(path); const auto data = QJsonDocument(QJsonObject{{"formatVersion", 1}, {"shortcuts", shortcuts}}).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) IconDialogs::MessageBox::warning(&dialog, I18n::text("Export failed"), file.errorString());
    });
    QObject::connect(import, &QPushButton::clicked, &dialog, [&] {
        const auto path = IconDialogs::FileDialog::getOpenFileName(&dialog, I18n::text("Import shortcuts"), {}, "JSON (*.json)"); if (path.isEmpty()) return;
        QFile file(path); if (!file.open(QIODevice::ReadOnly) || file.size() > 1024*1024) { IconDialogs::MessageBox::warning(&dialog, I18n::text("Import failed"), I18n::text("Cannot read file or file is too large.")); return; }
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(file.readAll(), &error); const auto object = doc.object();
        if (error.error != QJsonParseError::NoError || object.value("formatVersion").toInt() != 1 || !object.value("shortcuts").isObject()) { IconDialogs::MessageBox::warning(&dialog, I18n::text("Import failed"), I18n::text("Invalid shortcut file format.")); return; }
        auto imported = values; const auto keys = object.value("shortcuts").toObject(); QString problem;
        for (auto it = keys.begin(); it != keys.end(); ++it) {
            if (!imported.contains(it.key())) { problem = I18n::text("Unknown command: ") + it.key(); break; }
            if (!it.value().isString()) { problem = I18n::text("Shortcut must be a string: ") + it.key(); break; }
            const auto text = it.value().toString(); const QKeySequence key(text, QKeySequence::PortableText);
            if (!text.isEmpty() && (key.isEmpty() || key.toString(QKeySequence::PortableText).isEmpty() || key[0].key() == Qt::Key_unknown)) { problem = I18n::text("Invalid shortcut: ") + text; break; }
            imported[it.key()] = key;
        }
        if (problem.isEmpty()) problem = validate(imported);
        if (!problem.isEmpty()) { IconDialogs::MessageBox::warning(&dialog, I18n::text("Import failed"), problem); return; }
        values = imported; update();
    });
    auto* buttons = new IconDialogs::ButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QString error; if (save(values, &error)) dialog.accept(); else IconDialogs::MessageBox::warning(&dialog, I18n::text("Cannot save shortcuts"), error);
    });
    table->selectRow(0); dialog.exec();
}
