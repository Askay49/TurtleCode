#pragma once

#include <QPlainTextEdit>

static void vimStage11WindowTests(QTemporaryDir& directory) {
    using Mode = VimController::Mode;
    QSettings().setValue("editor/vimMode", true);
    QSettings().setValue("editor/recoveryEnabled", false);
    QSettings().setValue("editor/autoPairs", false);
    const QString originalMappings = "normal <Leader>w = command:file.save\nnormal ,w = keys:<Nop>";
    QSettings().setValue("vimKeymaps/bindings", originalMappings);
    const auto firstPath = directory.filePath("stage11-first.txt");
    const auto secondPath = directory.filePath("stage11-second.txt");
    for (const auto& path : {firstPath, secondPath}) {
        QFile file(path);
        assert(file.open(QIODevice::WriteOnly) && file.write("alpha\nbeta\n") == 11);
    }

    MainWindow window;
    window.show();
    window.openPaths({firstPath});
    auto* first = window.findChild<EditorWidget*>();
    assert(first && waitFor([&] { return !first->isBusy(); }));
    window.activateWindow();
    first->setFocus();
    assert(waitFor([&] { return first->hasFocus(); }));
    const auto settled = [](EditorWidget* editor) {
        return !editor->vimCommandExecuting() && !editor->isVimReplaying() && !editor->isBusy();
    };
    const auto focus = [&](EditorWidget* editor) {
        window.activateWindow();
        editor->setFocus();
        assert(waitFor([&] { return editor->hasFocus(); }));
    };
    const auto ex = [&](EditorWidget* editor, const QString& command) {
        focus(editor);
        QTest::keyClicks(editor, ":");
        auto* input = editor->findChild<QLineEdit*>("vimCommandInput");
        assert(input && input->hasFocus());
        input->setText(command);
        QTest::keyClick(input, Qt::Key_Return);
        assert(waitFor([&] { return settled(editor); }));
    };

    auto* settingsAction = window.findChild<QAction*>("settings.shortcuts");
    assert(settingsAction);
    const QString savedMappings =
        "normal <Leader>w = command:file.save\nnormal <Leader>a = keys:ggVG\ninsert jk = keys:<Esc>";
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        assert(dialog);
        auto* tabs = dialog->findChild<QTabWidget*>("shortcutSettingsTabs");
        auto* table = dialog->findChild<QTableWidget*>("vimShortcutTable");
        auto* leader = dialog->findChild<QLineEdit*>("vimLeaderKey");
        auto* add = dialog->findChild<QPushButton*>("vimAddMapping");
        auto* save = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
        assert(tabs && tabs->count() == 2 && table && leader && add && save);
        QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, {}, tabs->tabBar()->tabRect(1).center());
        assert(tabs->currentIndex() == 1 && table->isVisible() && leader->isVisible());
        assert(!tabs->widget(1)->findChild<QTabWidget*>() && !tabs->widget(1)->findChild<QPlainTextEdit*>());
        assert(table->rowCount() == 2 && table->item(0, 1)->text() == "<Leader>w");
        assert(leader->text() == "<Space>");
        const auto rejectSave = [&](const QString& fragment) {
            QTimer::singleShot(0, [&] {
                auto* error = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                assert(error && error->text().contains(fragment));
                assert(!QSettings().contains("vimKeymaps/leader"));
                assert(QSettings().value("vimKeymaps/bindings").toString() == originalMappings);
                error->accept();
            });
            save->click();
            dialog->activateWindow();
        };

        dialog->activateWindow();
        assert(waitFor([&] { return dialog->isActiveWindow(); }));
        QTest::mouseClick(leader, Qt::LeftButton);
        assert(waitFor([&] { return leader->hasFocus(); }));
        QTest::keyClick(leader, Qt::Key_F6);
        assert(leader->text() == "<Space>");
        QTest::keyClick(leader, Qt::Key_Comma);
        assert(leader->text() == ",");
        rejectSave(I18n::text("Duplicate Vim mapping in this mode."));
        QTest::mouseClick(leader, Qt::LeftButton);
        QTest::keyClick(leader, Qt::Key_Space);
        assert(leader->text() == "<Space>");
        table->item(1, 1)->setText("<Leader>a");
        table->item(1, 2)->setText("keys:ggVG");

        QTest::mouseClick(add, Qt::LeftButton);
        assert(table->rowCount() == 3);
        auto* keys = table->findChild<QLineEdit*>();
        assert(keys && keys->isVisible() && waitFor([&] { return keys->hasFocus(); }));
        QTest::keyClicks(keys, "jk");
        QTest::keyClick(keys, Qt::Key_Return);
        auto* mode = qobject_cast<QComboBox*>(table->cellWidget(2, 0));
        assert(mode && mode->isVisible() && mode->count() == 6 && mode->currentData() == "normal");
        assert(table->viewport()->rect().contains(mode->geometry().center()));
        mode->setFocus();
        for (int i = 0; i < 3; ++i) QTest::keyClick(mode, Qt::Key_Down);
        assert(mode->currentData() == "insert");
        table->item(2, 2)->setText("command:not.a.command");
        rejectSave(I18n::text("Unknown Turtle Code command ID."));
        table->item(2, 2)->setText("keys:<Esc>");
        save->click();
    });
    settingsAction->trigger();
    focus(first);
    assert(!QSettings().contains("vimKeymaps/leader"));
    assert(QSettings().value("vimKeymaps/bindings").toString() == savedMappings);

    QTest::keyClicks(first, " a");
    assert(first->vimMode() == Mode::Visual && first->selectedText().contains("alpha\nbeta"));
    QTest::keyClick(first, Qt::Key_Escape);
    first->setNavigationPosition(1, 1);
    QTest::keyClicks(first, "iXjk");
    assert(first->vimMode() == Mode::Normal && first->document()->buffer().text() == "Xalpha\nbeta\n");
    QTest::keyClicks(first, " w");
    assert(waitFor([&] { return !first->isModified() && settled(first); }));
    {
        QFile file(firstPath);
        assert(file.open(QIODevice::ReadOnly) && file.readAll() == "Xalpha\nbeta\n");
    }

    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        assert(dialog);
        auto* tabs = dialog->findChild<QTabWidget*>("shortcutSettingsTabs");
        auto* leader = dialog->findChild<QLineEdit*>("vimLeaderKey");
        assert(tabs && leader && leader->text() == "<Space>");
        tabs->setCurrentIndex(1);
        QTest::mouseClick(leader, Qt::LeftButton);
        QTest::keyClicks(leader, "x,");
        assert(leader->text() == ",");
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    settingsAction->trigger();
    focus(first);
    assert(QSettings().value("vimKeymaps/leader").toString() == ",");
    assert(QSettings().value("vimKeymaps/bindings").toString() == savedMappings);
    QTest::keyClicks(first, ",a");
    assert(first->vimMode() == Mode::Visual);
    QTest::keyClick(first, Qt::Key_Escape);

    ex(first, "nnoremap <buffer> zs iZ<Esc>");
    ex(first, "nmap <F7> zs");
    ex(first, "nnoremap <F8> zs");
    first->setNavigationPosition(1, 1);
    QTest::keyClick(first, Qt::Key_F7);
    QTest::keyClick(first, Qt::Key_F8);
    assert(first->document()->buffer().text().starts_with("ZXalpha"));
    ex(first, "normal zs");
    assert(first->document()->buffer().text().starts_with("ZZXalpha"));
    ex(first, "normal! zs");
    assert(first->document()->buffer().text().starts_with("ZZXalpha"));
    ex(first, "nunmap <buffer> zs");
    QTest::keyClick(first, Qt::Key_F7);
    assert(first->document()->buffer().text().starts_with("ZZXalpha"));

    ex(first, "set timeoutlen=35");
    ex(first, "inoremap jo J");
    first->setNavigationPosition(1, 1);
    QTest::keyClicks(first, "ij");
    assert(first->document()->buffer().text().starts_with("ZZ"));
    assert(waitFor([&] { return first->document()->buffer().text().starts_with("jZZ"); }));
    QTest::keyClick(first, Qt::Key_Escape);
    ex(first, "inoreabbrev <buffer> btw by the way");
    first->setNavigationPosition(1, 1);
    QTest::keyClicks(first, "ibtw ");
    QTest::keyClick(first, Qt::Key_Escape);
    assert(first->document()->buffer().text().starts_with("by the way jZZ"));
    QTest::keyClick(first, Qt::Key_U);
    assert(first->document()->buffer().text().starts_with("jZZ"));

    ex(first, "setlocal shiftwidth=2");
    ex(first, "setglobal tabstop=7");
    assert(first->tabSize() == 4 && first->indentationSize() == 2);
    assert(QSettings().value("vimOptions/tabstop").toInt() == 7);
    assert(!QSettings().contains("vimOptions/shiftwidth"));
    QTest::keyClicks(first, ":setlocal shif");
    auto* input = first->findChild<QLineEdit*>("vimCommandInput");
    QTest::keyClick(input, Qt::Key_Tab);
    assert(input->text() == "setlocal shiftwidth");
    QTest::keyClick(input, Qt::Key_Escape);

    ex(first, "command -buffer -nargs=1 -bar Prefix normal! i<args>");
    QTest::keyClicks(first, ":Pre");
    QTest::keyClick(input, Qt::Key_Tab);
    assert(input->text() == "Prefix");
    input->setText("Prefix Q | normal! G");
    QTest::keyClick(input, Qt::Key_Return);
    assert(waitFor([&] { return settled(first); }));
    assert(first->navigationPosition().x() == 2 && first->document()->buffer().text().find('Q') != std::string::npos);

    ex(first, "normal! gg");
    QTest::keyClicks(first, "q:");
    auto* history = first->findChild<QPlainTextEdit*>("vimCommandHistoryWindow");
    auto* bar = first->findChild<QFrame*>("vimCommandBar");
    assert(history && history->hasFocus() && first->viewport()->rect().contains(bar->geometry()));
    QTest::keyClicks(history, "kAG");
    QTest::keyClick(history, Qt::Key_Return);
    assert(waitFor([&] { return settled(first); }) && first->hasFocus() && first->navigationPosition().x() == 2);
    first->setNavigationPosition(1, 1);
    QTest::keyClicks(first, "/beta");
    QTest::keyClick(first, Qt::Key_Return);
    assert(waitFor([&] { return settled(first); }));
    first->setNavigationPosition(1, 1);
    QTest::keyClicks(first, "q/");
    assert(history->hasFocus());
    QTest::keyClicks(history, "k");
    QTest::keyClick(history, Qt::Key_Return);
    assert(waitFor([&] { return settled(first); }) && first->navigationPosition().x() == 2);

    ex(first, "augroup Stage");
    ex(first, "autocmd BufReadPost *.txt %s/alpha/loaded/");
    ex(first, "autocmd BufEnter *.txt setlocal shiftwidth=3");
    ex(first, "autocmd BufWritePost <buffer> %s/beta/saved/");
    ex(first, "augroup END");
    ex(first, "write | %s/saved/after/");
    assert(first->document()->buffer().text().ends_with("after\n"));
    window.openPaths({secondPath});
    EditorWidget* second = nullptr;
    assert(waitFor([&] {
        for (auto* editor : window.findChildren<EditorWidget*>())
            if (editor->filePath() == secondPath) second = editor;
        return second && settled(second) && second->document()->buffer().text() == "loaded\nbeta\n"
            && second->indentationSize() == 3;
    }));
    second->setFocus();
    assert(second->tabSize() == 7);
    ex(second, "Prefix Q");
    assert(second->document()->buffer().text() == "loaded\nbeta\n");
    ex(second, "augroup! Stage");
    ex(second, "autocmd BufWritePost <buffer> setglobal ignorecase");
    QPointer<EditorWidget> closing(second);
    QTest::keyClicks(second, ":wq");
    QTest::keyClick(second, Qt::Key_Return);
    assert(waitFor([&] { return closing.isNull() && QSettings().value("vimOptions/ignorecase").toBool(); }));
    closeDiscard(window);
}
