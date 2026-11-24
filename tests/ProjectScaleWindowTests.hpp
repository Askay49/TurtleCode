#pragma once
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QMimeData>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStatusBar>
#include "debug/DebugPanel.hpp"
#include "search/SearchPanel.hpp"
static void projectScaleWindowTests(QTemporaryDir& temporary) {
    const auto root=temporary.filePath("many-files");assert(QDir().mkpath(root));
    for(int i=0;i<650;++i){QFile file(root+QString("/file%1.txt").arg(i,4,10,QChar('0')));assert(file.open(QIODevice::WriteOnly));file.write("foo\n");}
    MainWindow window;window.show();window.openPaths({root});
    auto* tree=window.findChild<ProjectExplorer*>();assert(tree);
    QElapsedTimer heartbeat;heartbeat.start();qint64 gap=0;QTimer timer;timer.setInterval(5);
    QObject::connect(&timer,&QTimer::timeout,&window,[&]{gap=std::max(gap,heartbeat.restart());});timer.start();
    const auto pager=[&](qint64 offset)->QTreeWidgetItem* {
        auto* parent=tree->topLevelItem(0);if(!parent)return nullptr;
        for(int i=0;i<parent->childCount();++i){auto* item=parent->child(i);if(item->data(0,Qt::UserRole+10).isValid() && item->data(0,Qt::UserRole+10).toLongLong()==offset)return item;}return nullptr;
    };
    assert(waitFor([&]{return pager(500)!=nullptr;}));
    assert(tree->topLevelItem(0)->childCount()==501);
    tree->setFocus();
    tree->setVimEnabled(true);
    QTest::keyClick(tree, Qt::Key_G, Qt::ShiftModifier);
    assert(tree->currentItem() == pager(500));
    QTest::keyClick(tree, Qt::Key_Delete);
    assert(tree->currentItem() == pager(500) && QFileInfo(root).isDir());
    QTest::keyClick(tree, Qt::Key_Return);
    assert(waitFor([&]{return pager(0)!=nullptr && tree->topLevelItem(0)->childCount()==151;}));
    assert(gap<1000);window.grab().save("/tmp/turtle-large-project.png");
    window.findChild<QToolButton*>("searchActivity")->click();
    auto* panel=window.findChild<SearchPanel*>();assert(panel);
    auto* input=panel->findChild<QLineEdit*>("projectSearchInput");
    auto* results=panel->findChild<QTreeWidget*>("projectSearchResults");
    input->setText("foo");
    assert(waitFor([&]{return results->topLevelItemCount()==501;}));
    auto* first=results->topLevelItem(0);first->setCheckState(0,Qt::Unchecked);
    auto* next=results->topLevelItem(500);assert(next->data(0,Qt::UserRole+6).isValid());
    Q_EMIT results->itemClicked(next,0);
    assert(waitFor([&]{return results->topLevelItemCount()==151;}));
    auto* previous=results->topLevelItem(150);assert(previous->data(0,Qt::UserRole+6).toLongLong()==0);
    Q_EMIT results->itemClicked(previous,0);
    assert(waitFor([&]{return results->topLevelItemCount()==501;}));
    assert(results->topLevelItem(0)->checkState(0)==Qt::Unchecked);
    closeDiscard(window);
}

static void explorerRevealTests(QTemporaryDir& temporary) {
    const QString root = temporary.filePath("reveal-project");
    for (int i = 0; i <= 510; ++i)
        assert(QDir().mkpath(root + QString("/dir%1").arg(i, 4, 10, QChar('0'))));
    const QString lateFolder = root + "/dir0510";
    const auto writeFile = [](const QString& path) {
        QFile file(path);
        assert(file.open(QIODevice::WriteOnly) && file.write("sample\n") == 7);
    };
    for (int i = 0; i <= 510; ++i)
        writeFile(lateFolder + QString("/file%1.txt").arg(i, 4, 10, QChar('0')));
    const QString lateFile = lateFolder + "/file0510.txt";
    const QString earlyFile = root + "/dir0000/early.txt";
    const QString hiddenFile = lateFolder + "/.hidden.txt";
    writeFile(earlyFile);
    writeFile(hiddenFile);

    MainWindow window;
    window.show();
    window.openPaths({root});
    auto* tree = window.findChild<ProjectExplorer*>();
    auto* tabs = window.findChild<QTabWidget*>("editorTabs");
    auto* reveal = window.findChild<QAction*>("view.revealActiveFile");
    auto* sidebar = window.findChild<QAction*>("view.explorer");
    auto* search = window.findChild<QToolButton*>("searchActivity");
    assert(tree && tabs && reveal && sidebar && search);
    const auto selectedPath = [&] {
        const auto* item = tree->currentItem();
        return item ? item->data(0, Qt::UserRole).toString() : QString{};
    };
    const auto activeFile = [&] {
        const auto* editor = qobject_cast<EditorWidget*>(tabs->currentWidget());
        return editor && !editor->isBusy() ? editor->filePath() : QString{};
    };
    const auto setAutoReveal = [&](bool enabled) {
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            assert(dialog);
            auto* setting = dialog->findChild<QCheckBox*>("settingsAutoRevealActiveFile");
            assert(setting && setting->isChecked() != enabled);
            setting->setChecked(enabled);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        });
        window.findChild<QAction*>("settings.open")->trigger();
        assert(QSettings().value("view/autoRevealActiveFile").toBool() == enabled);
    };

    window.openPaths({lateFile});
    assert(waitFor([&] { return activeFile() == lateFile; }));
    assert(selectedPath().isEmpty());
    search->click();
    sidebar->trigger();
    assert(!tree->isVisible());
    reveal->trigger();
    assert(waitFor([&] { return selectedPath() == lateFile; }));
    assert(tree->isVisible() && tree->currentItem()->parent()->isExpanded());
    assert(tree->topLevelItem(0)->isExpanded() && tabs->count() == 1);

    setAutoReveal(true);
    sidebar->trigger();
    window.openPaths({earlyFile});
    assert(waitFor([&] { return activeFile() == earlyFile; }));
    assert(!tree->isVisible() && selectedPath() == lateFile);
    sidebar->trigger();
    assert(waitFor([&] { return selectedPath() == earlyFile; }));

    window.findChild<QAction*>("editor.splitRight")->trigger();
    auto* workspace = window.findChild<EditorPaneWorkspace*>();
    auto* left = workspace->panes().front();
    assert(left && left != workspace->activePane());
    window.openPaths({lateFile});
    assert(waitFor([&] { return selectedPath() == lateFile; }));
    window.activateWindow();
    left->editor()->setFocus();
    assert(waitFor([&] { return selectedPath() == earlyFile; }));
    assert(left->editor()->hasFocus());

    search->click();
    window.openPaths({lateFile});
    assert(waitFor([&] { return activeFile() == lateFile; }));
    assert(!tree->isVisible() && selectedPath() == earlyFile);
    reveal->trigger();
    assert(waitFor([&] { return selectedPath() == lateFile; }));

    window.openPaths({hiddenFile});
    assert(waitFor([&] { return activeFile() == hiddenFile; }));
    reveal->trigger();
    assert(waitFor([&] {
        return window.statusBar()->currentMessage() == I18n::text("The active file is unavailable in the project tree.");
    }));
    assert(selectedPath() == lateFile);

    setAutoReveal(false);
    window.openPaths({earlyFile});
    assert(waitFor([&] { return activeFile() == earlyFile; }));
    assert(selectedPath() == lateFile);
    closeDiscard(window);
}

static void explorerFolderSearchTests(QTemporaryDir& temporary) {
    const QDir root(temporary.filePath("folder-search"));
    const QString folder = root.filePath("src [old], backup");
    const QString nested = folder + "/nested/deep.txt";
    const QString inside = folder + "/inside.txt";
    const QString outside = root.filePath("src [old], backup-other/outside.txt");
    assert(QDir().mkpath(QFileInfo(nested).absolutePath()));
    assert(QDir().mkpath(QFileInfo(outside).absolutePath()));
    for (const auto& path : {nested, inside, outside}) {
        QFile file(path);
        assert(file.open(QIODevice::WriteOnly) && file.write("needle\n") == 7);
    }

    MainWindow window;
    window.show();
    window.activateWindow();
    assert(waitFor([&] { return window.isActiveWindow(); }));
    window.openPaths({root.path()});
    auto* tree = window.findChild<ProjectExplorer*>();
    assert(waitFor([&] { return window.findChild<SearchPanel*>(); }));
    auto* panel = window.findChild<SearchPanel*>();
    auto* query = panel->findChild<QLineEdit*>("projectSearchInput");
    auto* results = panel->findChild<QTreeWidget*>("projectSearchResults");
    auto* scope = panel->findChild<QLabel*>("projectSearchScope");
    auto* clearScope = panel->findChild<QToolButton*>("projectSearchClearScope");
    auto* replace = panel->findChild<QToolButton*>("projectReplaceButton");
    assert(tree && query && results && scope && clearScope && replace);
    const auto resultPaths = [&] {
        QSet<QString> paths;
        for (int i = 0; i < results->topLevelItemCount(); ++i)
            paths.insert(results->topLevelItem(i)->data(0, Qt::UserRole).toString());
        return paths;
    };
    const auto openFolderSearch = [&] {
        const auto name = QFileInfo(folder).fileName();
        const auto findFolder = [&] { return tree->findItems(name, Qt::MatchExactly | Qt::MatchRecursive).value(0); };
        assert(waitFor([&] { return findFolder() && tree->visualItemRect(findFolder()).isValid(); }));
        tree->setFocus();
        const auto position = tree->visualItemRect(findFolder()).center();
        QTimer::singleShot(0, &window, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            assert(menu);
            const auto actions = menu->actions();
            const auto action = std::find_if(actions.begin(), actions.end(), [](const QAction* item) {
                return item->text() == I18n::text("Search in This Folder");
            });
            assert(action != actions.end());
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(*action).center());
        });
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, tree->viewport()->mapToGlobal(position));
        QApplication::sendEvent(tree->viewport(), &event);
        assert(waitFor([&] { return panel->isVisible() && scope->isVisible(); }));
        assert(scope->toolTip() == QDir::toNativeSeparators(folder));
        assert(window.focusWidget() == query);
        // The offscreen platform can leave the window inactive after a popup closes.
        if (!window.isActiveWindow()) window.activateWindow();
        assert(waitFor([&] { return query->hasFocus(); }));
    };

    query->setText("needle");
    assert(waitFor([&] { return resultPaths() == QSet<QString>{nested, inside, outside}; }));
    openFolderSearch();
    assert(query->selectedText() == "needle");
    assert(waitFor([&] { return resultPaths() == QSet<QString>{nested, inside}; }));
    clearScope->click();
    assert(!scope->isVisible());
    assert(waitFor([&] { return resultPaths() == QSet<QString>{nested, inside, outside}; }));

    window.findChild<QToolButton*>("explorerActivity")->click();
    openFolderSearch();
    panel->findChild<QToolButton*>("projectReplaceToggle")->click();
    panel->findChild<QLineEdit*>("projectReplaceInput")->setText("changed");
    assert(waitFor([&] { return replace->isEnabled(); }));
    replace->click();
    const auto read = [](const QString& path) {
        QFile file(path);
        assert(file.open(QIODevice::ReadOnly));
        return file.readAll();
    };
    assert(waitFor([&] { return read(inside) == "changed\n" && read(nested) == "changed\n"; }));
    assert(read(outside) == "needle\n");
    assert(waitFor([&] { return results->topLevelItemCount() == 0 && query->isEnabled(); }));
    QTest::keyClick(query, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
    assert(!scope->isVisible());
    assert(waitFor([&] { return resultPaths() == QSet<QString>{outside}; }));
    closeDiscard(window);
}

static void explorerKeyboardTests(QTemporaryDir& temporary) {
    const QDir root(temporary.filePath("tree-keys"));
    const QString folder = root.filePath("src");
    const QString original = folder + "/old.txt";
    const QString renamed = folder + "/renamed.txt";
    assert(QDir().mkpath(folder));
    {
        QFile file(original);
        assert(file.open(QIODevice::WriteOnly) && file.write("sample\n") == 7);
    }
    QSettings settings;
    settings.setValue("editor/previewFiles", true);

    MainWindow window;
    window.show();
    window.activateWindow();
    window.openPaths({root.path()});
    auto* tree = window.findChild<ProjectExplorer*>();
    auto* tabs = window.findChild<QTabWidget*>("editorTabs");
    auto* search = window.findChild<QToolButton*>("searchActivity");
    auto* focusTree = window.findChild<QAction*>("view.focusProjectTree");
    auto* sidebar = window.findChild<QAction*>("view.explorer");
    auto* vim = window.findChild<QAction*>("settings.vimMode");
    auto* symbolRename = window.findChild<QAction*>("code.rename");
    assert(tree && tabs && search && focusTree && sidebar && vim && symbolRename);
    assert(focusTree->shortcut() == QKeySequence("Ctrl+Shift+E"));
    const auto findFolder = [&] { return tree->topLevelItem(0)->child(0); };
    assert(waitFor([&] { return window.isActiveWindow() && findFolder(); }));

    search->click();
    sidebar->trigger();
    assert(!tree->isVisible());
    QTimer::singleShot(0, &window, [] {
        auto* dialog = QApplication::activeModalWidget();
        assert(dialog);
        auto* input = dialog->findChild<QLineEdit*>("commandPaletteInput");
        assert(input);
        input->setText("view.focusProjectTree");
        QTest::keyClick(input, Qt::Key_Return);
    });
    window.findChild<QAction*>("navigation.commands")->trigger();
    assert(waitFor([&] { return tree->isVisible() && tree->hasFocus(); }));

    auto* folderItem = findFolder();
    tree->setCurrentItem(folderItem);
    QTest::keyClick(tree, Qt::Key_L);
    assert(!folderItem->isExpanded());
    QTest::keyClick(tree, Qt::Key_Return);
    assert(waitFor([&] { return folderItem->isExpanded() && folderItem->childCount() == 1; }));
    QTest::keyClick(tree, Qt::Key_Enter, Qt::KeypadModifier);
    assert(!folderItem->isExpanded());

    vim->trigger();
    assert(vim->isChecked());
    QTest::keyClick(tree, Qt::Key_L);
    assert(folderItem->isExpanded());
    QTest::keyClick(tree, Qt::Key_J);
    assert(tree->currentItem() == folderItem->child(0));
    QTest::keyClick(tree, Qt::Key_K);
    assert(tree->currentItem() == folderItem);
    QTest::keyClick(tree, Qt::Key_G, Qt::ShiftModifier);
    assert(tree->currentItem() == folderItem->child(0));
    QTest::keyClick(tree, Qt::Key_G);
    assert(tree->currentItem() == folderItem->child(0));
    QTest::keyClick(tree, Qt::Key_Escape);
    assert(tree->hasFocus() && tree->currentItem() == folderItem->child(0));
    QTest::keyClicks(tree, "gg");
    assert(tree->currentItem() == tree->topLevelItem(0));
    QTest::keyClick(tree, Qt::Key_L);
    assert(tree->currentItem() == folderItem);
    QTest::keyClick(tree, Qt::Key_L);
    assert(tree->currentItem() == folderItem->child(0));
    QTest::keyClick(tree, Qt::Key_H);
    assert(tree->currentItem() == folderItem);
    QTest::keyClick(tree, Qt::Key_H);
    assert(!folderItem->isExpanded() && tabs->count() == 0);
    QTest::keyClick(tree, Qt::Key_L);
    QTest::keyClick(tree, Qt::Key_J);
    QTest::keyClick(tree, Qt::Key_L);
    auto* editor = qobject_cast<EditorWidget*>(tabs->currentWidget());
    assert(editor && waitFor([&] { return !editor->isBusy() && editor->filePath() == original; }));
    auto* title = window.findChild<QToolButton*>("editorPaneTitle");
    assert(editor->hasFocus() && title && !title->accessibleName().startsWith("Preview of "));

    QSignalSpy symbolRenames(symbolRename, &QAction::triggered);
    QTest::keyClick(editor, Qt::Key_F2);
    assert(symbolRenames.count() == 1);
    QTest::keyClick(editor, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
    assert(tree->hasFocus() && tree->currentItem() == folderItem->child(0));
    QTest::keyClick(tree, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
    assert(editor->hasFocus() && tree->isVisible());
    QTest::keyClick(editor, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
    assert(tree->hasFocus() && tree->currentItem() == folderItem->child(0));
    QTest::keyClick(tree, Qt::Key_Escape);
    assert(editor->hasFocus() && tree->isVisible());
    QTest::keyClick(editor, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
    assert(tree->hasFocus());
    bool renamedFromTree = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        assert(dialog && dialog->textValue() == "old.txt");
        dialog->setTextValue("renamed.txt");
        renamedFromTree = true;
        dialog->accept();
    });
    QTest::keyClick(tree, Qt::Key_F2);
    assert(renamedFromTree && symbolRenames.count() == 1);
    assert(QFileInfo::exists(renamed) && !QFileInfo::exists(original));
    assert(editor->filePath() == renamed && tree->currentItem()->text(0) == "renamed.txt");
    window.activateWindow();
    tree->setFocus();
    assert(waitFor([&] { return tree->hasFocus(); }));

    const auto selectedPath = [&] {
        return tree->currentItem() ? tree->currentItem()->data(0, Qt::UserRole).toString() : QString{};
    };
    const auto checkTreeFocus = [&] {
        assert(window.focusWidget() == tree);
        // The offscreen platform has no window manager to reactivate the parent of a modal dialog.
        window.activateWindow();
        assert(waitFor([&] { return tree->hasFocus(); }));
    };
    const auto create = [&](const QString& directory, const QString& name, bool isFolder) {
        QTimer::singleShot(0, &window, [=] {
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            assert(dialog && dialog->labelText().contains(QDir::toNativeSeparators(directory)));
            dialog->setTextValue(name);
            dialog->accept();
        });
        QTest::keyClick(tree, Qt::Key_A, isFolder ? Qt::ShiftModifier : Qt::NoModifier);
        const auto path = QDir(directory).filePath(name);
        assert(QFileInfo::exists(path));
        assert(waitFor([&] { return selectedPath() == path; }));
        checkTreeFocus();
        assert(tabs->count() == 1 && tabs->currentWidget() == editor);
        return path;
    };
    const auto newFolder = create(folder, "scratch", true);
    const auto newFile = create(newFolder, "note.txt", false);
    QTimer::singleShot(0, &window, [] {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        assert(dialog && dialog->textValue() == "note.txt");
        auto* input = dialog->findChild<QLineEdit*>();
        assert(input && waitFor([&] { return input->selectedText() == "note"; }));
        QTest::keyClicks(input, "draft");
        QTest::keyClick(input, Qt::Key_Return);
    });
    QTest::keyClick(tree, Qt::Key_R);
    const auto draft = newFolder + "/draft.txt";
    assert(waitFor([&] { return selectedPath() == draft; }));
    checkTreeFocus();
    assert(QFileInfo::exists(draft) && !QFileInfo::exists(newFile));

    const auto deleteKey = [&](bool permanent, bool accept) {
        const auto path = selectedPath();
        QTimer::singleShot(0, &window, [=] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            assert(dialog && dialog->text().contains(QDir::toNativeSeparators(path)));
            assert(dialog->windowTitle() == I18n::text(permanent ? "Delete Permanently" : "Move to Trash"));
            assert(dialog->defaultButton() == dialog->button(permanent ? QMessageBox::No : QMessageBox::Yes));
            if (accept) dialog->button(QMessageBox::Yes)->click();
            else QTest::keyClick(dialog, permanent ? Qt::Key_Return : Qt::Key_Escape);
        });
        QTest::keyClick(tree, Qt::Key_D, permanent ? Qt::ShiftModifier : Qt::NoModifier);
    };
    deleteKey(false, false);
    checkTreeFocus();
    deleteKey(true, false);
    checkTreeFocus();
    assert(QFileInfo::exists(draft) && selectedPath() == draft);
    deleteKey(true, true);
    assert(waitFor([&] { return !QFileInfo::exists(draft) && selectedPath() == newFolder; }));
    checkTreeFocus();
    deleteKey(true, true);
    assert(waitFor([&] { return !QFileInfo::exists(newFolder) && selectedPath() == renamed; }));
    checkTreeFocus();
    QTest::keyClicks(tree, "gg");
    QTest::keyClick(tree, Qt::Key_D, Qt::ShiftModifier);
    assert(tree->roots() == QStringList{root.path()} && QFileInfo::exists(renamed));

    vim->trigger();
    assert(!vim->isChecked());
    tree->setCurrentItem(folderItem);
    QTest::keyClick(tree, Qt::Key_H);
    assert(folderItem->isExpanded());
    closeDiscard(window);
    settings.remove("editor/previewFiles");
}

static void explorerHeaderTests(QTemporaryDir& temporary) {
    const QDir root(temporary.filePath("tree-header"));
    assert(QDir().mkpath(root.filePath("src/sub")));
    {
        QFile hidden(root.filePath(".hidden.txt"));
        assert(hidden.open(QIODevice::WriteOnly));
    }

    MainWindow window;
    window.show();
    window.activateWindow();
    auto* tree = window.findChild<ProjectExplorer*>();
    auto* header = window.findChild<QFrame*>("explorerHeading");
    assert(tree && header);
    auto* newFile = header->findChild<QToolButton*>("explorerNewFile");
    auto* newFolder = header->findChild<QToolButton*>("explorerNewFolder");
    auto* collapse = header->findChild<QToolButton*>("explorerCollapseAll");
    auto* tabs = window.findChild<QTabWidget*>("editorTabs");
    assert(newFile && newFolder && collapse && tabs);
    tree->setRoots({});
    assert(!newFile->isEnabled() && !newFolder->isEnabled() && !collapse->isEnabled());
    window.openPaths({root.path()});
    const auto findItem = [&](const QString& name) {
        return tree->findItems(name, Qt::MatchExactly | Qt::MatchRecursive).value(0);
    };
    assert(waitFor([&] { return window.isActiveWindow() && findItem("src"); }));
    for (auto* button : {newFile, newFolder, collapse}) {
        assert(button->isEnabled() && button->isVisible());
        assert(header->rect().contains(button->geometry()) && !button->icon().isNull());
        assert(!button->accessibleName().isEmpty() && button->accessibleName() == button->toolTip());
    }
    assert(newFile->geometry().right() < newFolder->geometry().left());
    assert(newFolder->geometry().right() < collapse->geometry().left());

    const auto create = [&](QToolButton* button, const QString& title, const QString& name) {
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            assert(dialog && dialog->windowTitle() == title);
            dialog->setTextValue(name);
            dialog->accept();
        });
        QTest::mouseClick(button, Qt::LeftButton);
    };
    assert(!tree->currentItem());
    create(newFolder, I18n::text("New Folder"), "assets");
    assert(waitFor([&] { return findItem("assets"); }));
    assert(QFileInfo(root.filePath("assets")).isDir());

    auto* src = findItem("src");
    tree->setCurrentItem(src);
    tree->setFocus();
    QTest::keyClick(tree, Qt::Key_Return);
    assert(waitFor([&] { return findItem("sub"); }));
    create(newFile, I18n::text("New File"), "created.txt");
    const QString created = root.filePath("src/created.txt");
    assert(waitFor([&] {
        const auto* editor = qobject_cast<EditorWidget*>(tabs->currentWidget());
        return findItem("created.txt") && editor && !editor->isBusy() && editor->filePath() == created;
    }));
    tree->setCurrentItem(findItem("created.txt"));
    create(newFolder, I18n::text("New Folder"), "generated");
    assert(waitFor([&] { return findItem("generated"); }));
    assert(QFileInfo(root.filePath("src/generated")).isDir());

    auto* sub = findItem("sub");
    auto* rootItem = tree->topLevelItem(0);
    if (!window.isActiveWindow()) window.activateWindow();
    tree->setCurrentItem(sub);
    tree->setFocus();
    assert(waitFor([&] { return tree->hasFocus(); }));
    QTest::keyClick(tree, Qt::Key_Return);
    assert(rootItem->isExpanded() && src->isExpanded() && sub->isExpanded());
    tree->setShowHidden(true); // A refresh has saved the current expanded paths.
    QTest::mouseClick(collapse, Qt::LeftButton);
    assert(tree->currentItem() == rootItem);
    assert(waitFor([&] { return findItem(".hidden.txt"); }));
    assert(!rootItem->isExpanded() && !src->isExpanded() && !sub->isExpanded());
    assert(tree->hasFocus());

    QTest::keyClick(tree, Qt::Key_Return);
    assert(rootItem->isExpanded() && !src->isExpanded());
    closeDiscard(window);
}

static void explorerFileTransferTests(QTemporaryDir& temporary) {
    const QDir root(temporary.filePath("tree-transfer"));
    const QString original = root.filePath("report.tar.gz");
    const QString source = root.filePath("source");
    const QString sourceFile = source + "/nested/main.txt";
    const QString target = root.filePath("target");
    const QString targetFile = target + "/report.tar.gz";
    const QString notesFile = target + "/notes.txt";
    const QString compactTarget = root.filePath("archive/assets");
    assert(QDir().mkpath(source + "/nested") && QDir().mkpath(target) && QDir().mkpath(compactTarget));
    const auto write = [](const QString& path, const QByteArray& text) {
        QFile file(path);
        assert(file.open(QIODevice::WriteOnly) && file.write(text) == text.size());
    };
    const auto read = [](const QString& path) {
        QFile file(path);
        assert(file.open(QIODevice::ReadOnly));
        return file.readAll();
    };
    write(original, "original\n");
    write(targetFile, "existing\n");
    write(notesFile, "notes\n");
    write(sourceFile, "text\n");
    write(source + "/.hidden", "hidden\n");
    assert(QFile::link(root.filePath("missing"), source + "/nested/link"));
    const auto privateReadOnly = QFile::ReadOwner | QFile::ExeOwner;
    assert(QFile::setPermissions(source + "/nested", privateReadOnly));

    MainWindow window;
    window.resize(1100, 720);
    window.show();
    window.activateWindow();
    window.openPaths({root.path()});
    auto* tree = window.findChild<ProjectExplorer*>();
    auto* tabs = window.findChild<QTabWidget*>("editorTabs");
    assert(waitFor([&] { return window.findChild<DebugPanel*>(); }));
    auto* debug = window.findChild<DebugPanel*>();
    auto* clipboard = QApplication::clipboard();
    assert(tree && tabs && debug);
    const auto select = [&](const QString& path) {
        tree->revealPath(path, true);
        if (!window.isActiveWindow()) window.activateWindow();
        tree->setFocus();
        assert(waitFor([&] {
            return tree->hasFocus() && tree->currentItem() && tree->currentItem()->toolTip(0) == path;
        }));
    };
    const auto key = [&](Qt::Key shortcut) { QTest::keyClick(tree, shortcut, Qt::ControlModifier); };
    const auto dragData = [&] {
        return std::unique_ptr<QMimeData>(tree->model()->mimeData({tree->currentIndex()}));
    };
    const auto emptyPoint = [&] {
        const auto rect = tree->viewport()->rect();
        const QPoint point(rect.center().x(), rect.bottom() - tree->fontMetrics().height());
        assert(rect.contains(point) && !tree->itemAt(point));
        return point;
    };
    const auto dragTo = [&](const QMimeData* data, const QPoint& point, bool canDrop,
                            QTreeWidgetItem* folderToExpand = nullptr) {
        if (folderToExpand) assert(!folderToExpand->isExpanded());
        QDragEnterEvent enter(point, Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        if (!enter.isAccepted()) {
            assert(!canDrop);
            return false;
        }
        QDragMoveEvent move(point, Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &move);
        assert(move.isAccepted() == canDrop);
        if (folderToExpand) assert(waitFor([&] { return folderToExpand->isExpanded(); }));
        QDropEvent drop(point, Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &drop);
        return drop.isAccepted();
    };
    const auto dragToSelected = [&](const QMimeData* data, bool canDrop, bool hoverToExpand = false) {
        auto* item = tree->currentItem();
        tree->scrollToItem(item);
        const QPoint point = tree->visualItemRect(item).center();
        assert(tree->itemAt(point) == item);
        return dragTo(data, point, canDrop, hoverToExpand ? item : nullptr);
    };
    const auto menuAction = [&](const QString& name) {
        const QPoint point = tree->visualItemRect(tree->currentItem()).center();
        QTimer::singleShot(0, &window, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            assert(menu);
            for (auto* action : menu->actions()) {
                if (action->text() != name) continue;
                assert(action->isEnabled());
                QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
                return;
            }
            assert(false);
        });
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, tree->viewport()->mapToGlobal(point));
        QApplication::sendEvent(tree->viewport(), &event);
    };
    const auto resolveConflict = [&](bool keepBoth) {
        QTimer::singleShot(0, &window, [keepBoth] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            assert(dialog && dialog->windowTitle() == I18n::text("Name conflict"));
            if (!keepBoth) {
                QTest::keyClick(dialog, Qt::Key_Escape);
                return;
            }
            for (auto* button : dialog->buttons()) {
                if (dialog->buttonRole(button) != QMessageBox::AcceptRole) continue;
                button->click();
                return;
            }
            assert(false);
        });
    };

    select(original);
    key(Qt::Key_C);
    assert(clipboard->mimeData()->urls() == QList<QUrl>{QUrl::fromLocalFile(original)});
    select(targetFile);
    resolveConflict(false);
    key(Qt::Key_V);
    const QString targetCopy = target + "/report copy.tar.gz";
    assert(!QFileInfo::exists(targetCopy) && read(targetFile) == "existing\n");
    select(targetFile);
    resolveConflict(true);
    key(Qt::Key_V);
    assert(waitFor([&] { return QFileInfo::exists(targetCopy); }));
    assert(read(targetCopy) == "original\n" && read(targetFile) == "existing\n");

    select(original);
    menuAction(I18n::text("Duplicate"));
    const QString duplicate = root.filePath("report copy.tar.gz");
    assert(waitFor([&] { return QFileInfo::exists(duplicate); }));
    assert(read(duplicate) == "original\n");

    select(notesFile);
    key(Qt::Key_C);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, emptyPoint());
    auto* rootItem = tree->topLevelItem(0);
    assert(tree->hasFocus() && tree->currentItem() == rootItem && rootItem->isSelected());
    assert(rootItem->isExpanded());
    key(Qt::Key_V);
    const QString notesCopy = root.filePath("notes.txt");
    assert(waitFor([&] { return QFileInfo::exists(notesCopy); }));
    assert(read(notesCopy) == "notes\n" && read(notesFile) == "notes\n");

    window.openPaths({sourceFile});
    assert(waitFor([&] {
        const auto* editor = qobject_cast<EditorWidget*>(tabs->currentWidget());
        return editor && !editor->isBusy() && editor->filePath() == sourceFile;
    }));
    auto* editor = qobject_cast<EditorWidget*>(tabs->currentWidget());
    editor->setFocus();
    clipboard->setText("unsaved ");
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    assert(editor->isModified() && editor->lspText().contains("unsaved "));
    debug->toggleBreakpoint(sourceFile, 1);

    select(source);
    menuAction(I18n::text("Cut"));
    select(target);
    editor->setBusy(true);
    QSignalSpy errors(tree, &ProjectExplorer::backgroundError);
    key(Qt::Key_V);
    assert(errors.count() == 1 && !QFileInfo::exists(target + "/source"));
    assert(clipboard->mimeData()->urls() == QList<QUrl>{QUrl::fromLocalFile(source)});
    editor->setBusy(false);

    select(source);
    menuAction(I18n::text("Copy"));
    select(source + "/nested");
    QTimer::singleShot(0, &window, [] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        assert(dialog && dialog->text() == I18n::text("A folder cannot be copied or moved into itself."));
        dialog->button(QMessageBox::Ok)->click();
    });
    key(Qt::Key_V);
    assert(!QFileInfo::exists(source + "/nested/source"));
    select(target);
    menuAction(I18n::text("Paste"));
    const QString sourceCopy = target + "/source";
    assert(waitFor([&] { return QFileInfo::exists(sourceCopy + "/nested/main.txt"); }));
    assert(read(sourceCopy + "/nested/main.txt") == "text\n");
    assert(read(sourceCopy + "/.hidden") == "hidden\n");
    assert(QFileInfo(sourceCopy + "/nested/link").isSymLink());
    assert(QFileInfo(sourceCopy + "/nested").permissions() == QFileInfo(source + "/nested").permissions());

    select(source);
    key(Qt::Key_X);
    select(target);
    resolveConflict(true);
    key(Qt::Key_V);
    const QString moved = target + "/source copy/nested/main.txt";
    assert(waitFor([&] { return editor->filePath() == moved; }));
    assert(read(moved) == "text\n" && !QFileInfo::exists(source));
    assert(!clipboard->mimeData() || clipboard->mimeData()->urls().isEmpty());
    assert(editor->isModified() && editor->lspText().contains("unsaved "));
    assert(debug->breakpoints(sourceFile).isEmpty() && debug->breakpoints(moved) == QSet<int>{1});
    select(moved);

    select(root.path());
    assert(!dragData());
    QMimeData external;
    external.setUrls({QUrl::fromLocalFile(targetFile)});
    assert(!dragToSelected(&external, false));
    assert(!dragTo(&external, emptyPoint(), false));
    select(original);
    const auto originalDrag = dragData();
    assert(originalDrag && !dragTo(originalDrag.get(), emptyPoint(), false)); // Already in the root.

    select(sourceCopy);
    const auto folderDrag = dragData();
    assert(folderDrag);
    assert(!dragToSelected(folderDrag.get(), false)); // Current parent.
    select(sourceCopy + "/nested");
    assert(!dragToSelected(folderDrag.get(), false));
    select(targetFile);
    assert(!dragToSelected(folderDrag.get(), false));

    const QString movedFolder = target + "/source copy";
    select(movedFolder);
    const auto movedFolderDrag = dragData();
    assert(movedFolderDrag);
    assert(!dragToSelected(movedFolderDrag.get(), false));

    const QString blocker = root.filePath("source copy");
    assert(QDir().mkpath(blocker));
    select(root.path());
    clipboard->setText("clipboard contents");
    resolveConflict(false);
    assert(!dragToSelected(movedFolderDrag.get(), true));
    assert(QFileInfo::exists(moved) && editor->filePath() == moved);
    select(movedFolder);
    resolveConflict(true);
    assert(dragTo(movedFolderDrag.get(), emptyPoint(), true));
    assert(tree->currentItem() == rootItem && rootItem->isSelected());
    window.activateWindow();
    assert(waitFor([&] { return tree->hasFocus(); }));
    const QString draggedFolder = root.filePath("source copy copy");
    const QString draggedFile = draggedFolder + "/nested/main.txt";
    assert(editor->filePath() == draggedFile && !QFileInfo::exists(movedFolder));
    assert(editor->isModified() && editor->lspText().contains("unsaved "));
    assert(debug->breakpoints(moved).isEmpty() && debug->breakpoints(draggedFile) == QSet<int>{1});
    assert(clipboard->text() == "clipboard contents");

    tree->setCompactFolders(true);
    select(targetFile);
    const auto fileDrag = dragData();
    assert(fileDrag);
    select(compactTarget);
    assert(tree->currentItem()->text(0) == "archive/assets");
    assert(dragToSelected(fileDrag.get(), true, true));
    assert(!QFileInfo::exists(targetFile) && read(compactTarget + "/report.tar.gz") == "existing\n");
    closeDiscard(window);
    assert(QFile::setPermissions(sourceCopy + "/nested", privateReadOnly | QFile::WriteOwner));
    assert(QFile::setPermissions(QFileInfo(draggedFile).absolutePath(), privateReadOnly | QFile::WriteOwner));
}

static void explorerRemovalTests(QTemporaryDir& temporary) {
    const QDir root(temporary.filePath("tree-removal"));
    const QString fixtureName = QFileInfo(temporary.path()).fileName();
    const QString folderName = fixtureName + "-discard-folder";
    const QString fileName = fixtureName + "-trash-file.txt";
    const QString permanentName = fixtureName + "-permanent.txt";
    const QString folder = root.filePath(folderName);
    const QString nested = folder + "/nested/draft.txt";
    const QString file = root.filePath(fileName);
    const QString permanent = root.filePath(permanentName);
    const QString keep = root.filePath("keep.txt");
    assert(QDir().mkpath(QFileInfo(nested).absolutePath()));
    for (const auto& path : {file, nested, permanent, keep}) {
        QFile output(path);
        assert(output.open(QIODevice::WriteOnly) && output.write("sample\n") == 7);
    }

    MainWindow window;
    window.show();
    window.activateWindow();
    window.openPaths({root.path()});
    auto* tree = window.findChild<ProjectExplorer*>();
    auto* tabs = window.findChild<QTabWidget*>("editorTabs");
    assert(tree && tabs);
    QSignalSpy removed(tree, &ProjectExplorer::pathDeleted);
    const auto select = [&](const QString& path) {
        tree->revealPath(path, true);
        window.activateWindow();
        tree->setFocus();
        assert(waitFor([&] {
            return tree->hasFocus() && tree->currentItem() &&
                tree->currentItem()->data(0, Qt::UserRole).toString() == path;
        }));
        tree->scrollToItem(tree->currentItem());
    };
    const auto answerConfirmation = [&](const QString& title, const QString& path, QMessageBox::StandardButton answer) {
        QTimer::singleShot(0, &window, [title, path, answer] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            assert(dialog && dialog->windowTitle() == title && dialog->text().contains(path));
            const bool trashPrompt = title == I18n::text("Move to Trash");
            assert(dialog->defaultButton() == dialog->button(trashPrompt ? QMessageBox::Yes : QMessageBox::No));
            if (trashPrompt) {
                auto* yes = dialog->defaultButton();
                assert(dialog->focusWidget() == yes);
                assert(waitFor([&] { return yes->hasFocus(); }));
                QTest::keyClick(yes, answer == QMessageBox::Yes ? Qt::Key_Return : Qt::Key_Escape);
            } else {
                dialog->button(answer)->click();
            }
        });
    };
    const auto chooseMenuAction = [&](const QString& text, const QString& title, const QString& path,
                                      QMessageBox::StandardButton answer, bool isRoot = false) {
        const QPoint position = tree->visualItemRect(tree->currentItem()).center();
        assert(tree->itemAt(position) == tree->currentItem());
        QTimer::singleShot(0, &window, [&, text, title, path, answer, isRoot] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            assert(menu);
            const auto actions = menu->actions();
            const auto findAction = [&](const QString& label) {
                return std::find_if(actions.begin(), actions.end(), [&](const QAction* action) {
                    return action->text() == label;
                });
            };
            const auto action = findAction(text);
            assert(action != actions.end());
            if (isRoot) {
                assert(findAction(I18n::text("Move to Trash")) == actions.end());
                assert(findAction(I18n::text("Delete Permanently...")) == actions.end());
            }
            answerConfirmation(title, path, answer);
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(*action).center());
        });
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, tree->viewport()->mapToGlobal(position));
        QApplication::sendEvent(tree->viewport(), &event);
    };

    select(file);
    answerConfirmation(I18n::text("Move to Trash"), file, QMessageBox::No);
    QTest::keyClick(tree, Qt::Key_Delete);
    assert(removed.count() == 0 && QFileInfo::exists(file));

#ifdef Q_OS_LINUX
    const auto setTrashConfirmation = [&](bool enabled) {
        QTimer::singleShot(0, &window, [enabled] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            assert(dialog);
            auto* sections = dialog->findChild<QListWidget*>("settingsSections");
            assert(sections);
            const auto files = sections->findItems(I18n::text("Files and search"), Qt::MatchExactly);
            assert(files.size() == 1);
            sections->setCurrentItem(files.front());
            auto* box = dialog->findChild<QCheckBox*>("settingsConfirmMoveToTrash");
            assert(box && box->isVisible() && box->isChecked() != enabled);
            box->click();
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        });
        window.findChild<QAction*>("settings.open")->trigger();
        assert(QSettings().value("project/confirmMoveToTrash").toBool() == enabled);
    };
    // Qt can use a volume's own trash when the fixtures live on a separate mount.
    assert(QDir().mkpath(temporary.filePath("data")));
    QFile probe(root.filePath(fixtureName + "-trash-probe.txt"));
    assert(probe.open(QIODevice::WriteOnly));
    probe.close();
    assert(probe.moveToTrash());
    const QDir trash(QFileInfo(probe.fileName()).absolutePath());
    const auto cleanupTrash = qScopeGuard([&] {
        QFile::remove(probe.fileName());
        QFile::remove(trash.filePath(fileName));
        QDir(trash.filePath(folderName)).removeRecursively();
        for (const auto& name : {QFileInfo(probe.fileName()).fileName(), fileName, folderName})
            QFile::remove(trash.filePath("../info/" + name + ".trashinfo"));
    });
    window.openPaths({file});
    auto* editor = qobject_cast<EditorWidget*>(tabs->currentWidget());
    assert(editor && waitFor([&] { return !editor->isBusy() && editor->filePath() == file; }));
    window.activateWindow();
    editor->setFocus();
    assert(waitFor([&] { return editor->hasFocus(); }));
    QTest::keyClick(editor, Qt::Key_Delete);
    assert(editor->isModified() && QFileInfo::exists(file) && removed.isEmpty());
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    assert(!editor->isModified());

    select(file);
    tree->setVimEnabled(true);
    answerConfirmation(I18n::text("Move to Trash"), file, QMessageBox::Yes);
    QTest::keyClick(tree, Qt::Key_D);
    tree->setVimEnabled(false);
    assert(waitFor([&] { return removed.count() == 1 && tabs->count() == 0; }));
    assert(removed.takeFirst().front().toString() == file && !QFileInfo::exists(file));
    assert(QFileInfo::exists(trash.filePath(fileName)));
    assert(waitFor([&] { return tree->findItems(fileName, Qt::MatchExactly | Qt::MatchRecursive).isEmpty(); }));

    window.openPaths({nested});
    editor = qobject_cast<EditorWidget*>(tabs->currentWidget());
    assert(editor && waitFor([&] { return !editor->isBusy() && editor->filePath() == nested; }));
    window.activateWindow();
    editor->setFocus();
    assert(waitFor([&] { return editor->hasFocus(); }));
    QTest::keyClicks(editor, "unsaved ");
    const auto draft = editor->document()->buffer().text();
    assert(editor->isModified() && draft.find("unsaved ") != std::string::npos);
    select(folder);
    chooseMenuAction(I18n::text("Move to Trash"), I18n::text("Move to Trash"), folder, QMessageBox::No);
    assert(removed.isEmpty() && QFileInfo::exists(folder));
    setTrashConfirmation(false);
    select(folder);
    QTimer::singleShot(0, &window, [] {
        assert(!qobject_cast<QMessageBox*>(QApplication::activeModalWidget()));
    });
    QTest::keyClick(tree, Qt::Key_Delete);
    assert(waitFor([&] { return removed.count() == 1; }));
    assert(removed.takeFirst().front().toString() == folder && !QFileInfo::exists(folder));
    assert(QFileInfo::exists(trash.filePath(folderName + "/nested/draft.txt")));
    assert(tabs->count() == 1 && tabs->widget(0) == editor && editor->filePath().isEmpty() &&
        editor->isModified() && editor->document()->buffer().text() == draft);
#endif

    select(permanent);
    chooseMenuAction(I18n::text("Delete Permanently..."), I18n::text("Delete Permanently"), permanent, QMessageBox::Yes);
    assert(waitFor([&] { return removed.count() == 1; }));
    assert(removed.takeFirst().front().toString() == permanent && !QFileInfo::exists(permanent));
#ifdef Q_OS_LINUX
    assert(!QFileInfo::exists(trash.filePath(permanentName)));
#endif

    select(root.path());
    chooseMenuAction(I18n::text("Remove Folder from Workspace"), I18n::text("Remove Folder"),
                     root.path(), QMessageBox::No, true);
    assert(tree->roots() == QStringList{root.path()});
    select(root.path());
    answerConfirmation(I18n::text("Remove Folder"), root.path(), QMessageBox::Yes);
    QTest::keyClick(tree, Qt::Key_Delete);
    assert(tree->roots().isEmpty() && tree->topLevelItemCount() == 0);
    assert(QFileInfo::exists(keep) && removed.isEmpty());
#ifdef Q_OS_LINUX
    setTrashConfirmation(true);
#endif
    closeDiscard(window);
}

static void explorerCompactFolderTests(QTemporaryDir& temporary) {
    const QDir root(temporary.filePath("compact-folders"));
    const QString src = root.filePath("src");
    const QString middle = src + "/core";
    const QString end = middle + "/text";
    assert(QDir().mkpath(end));
    const auto writeFile = [](const QString& path) {
        QFile file(path);
        assert(file.open(QIODevice::WriteOnly) && file.write("sample\n") == 7);
    };
    {
        QFile ignore(root.filePath(".turtleignore"));
        assert(ignore.open(QIODevice::WriteOnly));
        assert(ignore.write("src/ignored.txt\n") == 16);
    }
    writeFile(src + "/ignored.txt");
    writeFile(middle + "/.hidden.txt");
    for (int i = 0; i <= 500; ++i)
        writeFile(end + QString("/file%1.txt").arg(i, 4, 10, QChar('0')));

    MainWindow window;
    window.show();
    window.activateWindow();
    window.openPaths({root.path()});
    auto* tree = window.findChild<ProjectExplorer*>();
    auto* tabs = window.findChild<QTabWidget*>("editorTabs");
    auto* reveal = window.findChild<QAction*>("view.revealActiveFile");
    assert(tree && tabs && reveal);
    const auto row = [&](const QString& name) {
        return tree->findItems(name, Qt::MatchExactly | Qt::MatchRecursive).value(0);
    };
    const auto selectedPath = [&] {
        const auto* selected = tree->currentItem();
        return selected ? selected->data(0, Qt::UserRole).toString() : QString{};
    };
    const auto setCompact = [&](bool enabled) {
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            assert(dialog);
            auto* setting = dialog->findChild<QCheckBox*>("settingsCompactFolders");
            assert(setting && setting->isChecked() != enabled);
            setting->setChecked(enabled);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        });
        window.findChild<QAction*>("settings.open")->trigger();
        assert(QSettings().value("view/compactFolders").toBool() == enabled);
    };

    assert(waitFor([&] { return window.isActiveWindow() && row("src"); }));
    assert(!row("src/core/text"));
    setCompact(true);
    assert(waitFor([&] { return row("src/core/text"); }));
    auto* compact = row("src/core/text");
    assert(compact->parent() == tree->topLevelItem(0) && compact->toolTip(0) == end);
    assert(!compact->isExpanded() && compact->childCount() == 0);

    window.activateWindow();
    assert(waitFor([&] { return window.isActiveWindow(); }));
    tree->scrollToItem(compact);
    const QRect rect = tree->visualItemRect(compact);
    const QPoint rowLabel(rect.left() + 24, rect.center().y());
    assert(rect.isValid() && tree->itemAt(rowLabel) == compact);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rowLabel);
    assert(waitFor([&] { return tree->hasFocus() && compact->isExpanded() && row("file0000.txt"); }));
    assert(selectedPath() == end);

    const QString lateFile = end + "/file0500.txt";
    window.openPaths({lateFile});
    assert(waitFor([&] {
        const auto* editor = qobject_cast<EditorWidget*>(tabs->currentWidget());
        return editor && !editor->isBusy() && editor->filePath() == lateFile;
    }));
    reveal->trigger();
    assert(waitFor([&] { return selectedPath() == lateFile; }));
    assert(tree->currentItem()->parent() == compact);

    const QString branchFile = middle + "/sibling.txt";
    writeFile(branchFile);
    assert(waitFor([&] {
        return row("src/core") && row("text") && selectedPath() == lateFile;
    }));
    assert(QFile::remove(branchFile));
    assert(waitFor([&] { return row("src/core/text") && selectedPath() == lateFile; }));

    writeFile(branchFile);
    tree->revealPath(branchFile, true); // Reveal before the watcher has refreshed the chain.
    assert(waitFor([&] { return row("src/core") && selectedPath() == branchFile; }));
    assert(QFile::remove(branchFile));
    reveal->trigger();
    assert(waitFor([&] { return row("src/core/text") && selectedPath() == lateFile; }));

    tree->setShowHidden(true);
    assert(waitFor([&] { return row("src/core") && row(".hidden.txt"); }));
    tree->setShowHidden(false);
    assert(waitFor([&] { return row("src/core/text") && selectedPath() == lateFile; }));
    tree->setIncludeIgnored(true);
    assert(waitFor([&] { return row("src") && row("ignored.txt"); }));
    tree->setIncludeIgnored(false);
    assert(waitFor([&] { return row("src/core/text") && selectedPath() == lateFile; }));

    setCompact(false);
    assert(waitFor([&] {
        return row("src") && row("core") && row("text") && selectedPath() == lateFile;
    }));
    setCompact(true);
    assert(waitFor([&] { return row("src/core/text") && selectedPath() == lateFile; }));

    tree->setCurrentItem(row("src/core/text"));
    tree->setFocus();
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        assert(dialog && dialog->textValue() == "text");
        dialog->setTextValue("buffer");
        dialog->accept();
    });
    QTest::keyClick(tree, Qt::Key_F2);
    assert(waitFor([&] {
        const auto* editor = qobject_cast<EditorWidget*>(tabs->currentWidget());
        return row("src/core/buffer") && selectedPath() == middle + "/buffer" &&
               editor && editor->filePath() == middle + "/buffer/file0500.txt";
    }));

    MainWindow fresh;
    fresh.show();
    fresh.openPaths({root.path()});
    auto* freshTree = fresh.findChild<ProjectExplorer*>();
    assert(freshTree && waitFor([&] { return !freshTree->findItems("src/core/buffer", Qt::MatchRecursive).isEmpty(); }));
    closeDiscard(fresh);
    closeDiscard(window);
}
