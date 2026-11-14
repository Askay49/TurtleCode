#pragma once
#include "app/CommandPalette.hpp"
#include <QTreeWidget>
#include <QLineEdit>

static void commandPaletteTests(QTemporaryDir& directory) {
    assert(CommandPalette::matchScore("Файл: Сохранить всё Save all files", "сох всё") >= 0);
    assert(CommandPalette::matchScore("Сохранить все файлы", "сох всё") >= 0);
    assert(CommandPalette::matchScore("Code: Format document", "fmt doc") >= 0);
    assert(CommandPalette::matchScore("Файл: Сохранить всё Save all files", "save all") >= 0);
    assert(CommandPalette::matchScore("Format document", "xyz") < 0);
    QSettings().setValue("editor/recoveryEnabled", false);
    const auto path = directory.filePath("palette.cpp");
    { QFile file(path); assert(file.open(QIODevice::WriteOnly)); file.write("int value;\n"); }
    MainWindow first, second;
    first.show(); second.show();
    first.openPaths({path}); second.openPaths({path});
    assert(waitFor([&] { auto* a=first.findChild<EditorWidget*>(); auto* b=second.findChild<EditorWidget*>(); return a && b && !a->isBusy() && !b->isBusy(); }));
    auto* editor = first.findChild<EditorWidget*>();
    auto* other = second.findChild<EditorWidget*>();
    auto* action = first.findChild<QAction*>("navigation.commands"); assert(action);
    assert(action->shortcut() == QKeySequence("Ctrl+Shift+P"));
    auto run = [&](const QString& id, bool cancel = false) {
        bool visited = false;
        first.activateWindow(); editor->setFocus(); QTest::qWait(30);
        QTimer::singleShot(0, [&] {
            auto* dialog = dynamic_cast<CommandPalette*>(QApplication::activeModalWidget()); assert(dialog);
            auto* input = dialog->findChild<QLineEdit*>("commandPaletteInput");
            auto* list = dialog->findChild<QTreeWidget*>("commandPaletteResults"); assert(input && list);
            input->setText("no-such-command-xyz"); assert(list->topLevelItemCount() == 0);
            input->setText(id); assert(list->topLevelItemCount() > 0);
            assert(list->topLevelItem(0)->data(0, Qt::UserRole + 1).toString() == id);
            if (id == "view.minimap") {
                assert(!list->topLevelItem(0)->text(1).isEmpty());
                dialog->grab().save("/tmp/turtle-command-palette.png");
            }
            visited = true;
            QTest::keyClick(input, cancel ? Qt::Key_Escape : Qt::Key_Return);
        });
        QTest::keySequence(editor, ShortcutManager::instance().sequence("navigation.commands"));
        assert(visited);
    };
    const auto original = editor->document()->buffer().text();
    run("editor.toggleLineComment", true);
    assert(editor->document()->buffer().text() == original);
    assert(waitFor([&] { return editor->hasFocus(); }));
    run("editor.toggleLineComment");
    assert(editor->document()->buffer().text() != original);
    assert(other->document()->buffer().text() == original);
    assert(QSettings().value("commands/recent").toStringList().front() == "editor.toggleLineComment");
    editor->documentUndo(); assert(editor->document()->buffer().text() == original);
    const bool minimap = editor->miniMapEnabled();
    run("view.minimap"); assert(editor->miniMapEnabled() != minimap);
    QTimer::singleShot(0, [&] {
        auto* dialog = dynamic_cast<CommandPalette*>(QApplication::activeModalWidget()); assert(dialog);
        auto* input = dialog->findChild<QLineEdit*>("commandPaletteInput");
        input->setText("debug.stop");
        QTest::keyClick(input, Qt::Key_Return); assert(dialog->isVisible());
        QTest::keyClick(input, Qt::Key_Escape);
    });
    action->trigger();
    QTimer::singleShot(0, [&] {
        auto* dialog = dynamic_cast<CommandPalette*>(QApplication::activeModalWidget()); assert(dialog);
        auto* input = dialog->findChild<QLineEdit*>("commandPaletteInput");
        auto* list = dialog->findChild<QTreeWidget*>("commandPaletteResults");
        assert(list->topLevelItem(0)->data(0, Qt::UserRole + 1).toString() == "view.minimap");
        QTest::keyClick(input, Qt::Key_Down); assert(list->currentItem() == list->topLevelItem(1));
        QTest::keyClick(input, Qt::Key_Up); assert(list->currentItem() == list->topLevelItem(0));
        QTest::keyClick(input, Qt::Key_Escape);
    });
    action->trigger();
    QMap<QString, QKeySequence> shortcuts;
    auto& manager = ShortcutManager::instance();
    for (const auto& command : manager.commands()) shortcuts[command.id] = manager.sequence(command.id);
    shortcuts["navigation.commands"] = QKeySequence("Ctrl+Alt+P");
    QString error; assert(manager.save(shortcuts, &error));
    assert(action->shortcut() == QKeySequence("Ctrl+Alt+P"));
    run("editor.toggleLineComment", true);
    first.findChild<QAction*>("view.terminal")->trigger();
    auto* terminal = first.findChild<TerminalPanel*>()->terminal();
    terminal->setFocus(); first.activateWindow(); QTest::qWait(30);
    bool fromTerminal = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = dynamic_cast<CommandPalette*>(QApplication::activeModalWidget()); assert(dialog);
        fromTerminal = true;
        QTest::keyClick(dialog->findChild<QLineEdit*>("commandPaletteInput"), Qt::Key_Escape);
    });
    QTest::keySequence(terminal, manager.sequence("navigation.commands")); assert(fromTerminal);
    assert(waitFor([&] { return terminal->hasFocus(); }));
    MainWindow empty; empty.show();
    QTimer::singleShot(0, [&] {
        auto* dialog = dynamic_cast<CommandPalette*>(QApplication::activeModalWidget()); assert(dialog);
        auto* input = dialog->findChild<QLineEdit*>("commandPaletteInput");
        input->setText("editor.cursorAbove");
        QTest::keyClick(input, Qt::Key_Return); assert(dialog->isVisible());
        QTest::keyClick(input, Qt::Key_Escape);
    });
    empty.findChild<QAction*>("navigation.commands")->trigger();
    assert(QSettings().value("commands/recent").toStringList().front() == "view.minimap");
}
