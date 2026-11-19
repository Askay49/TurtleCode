#pragma once
#include <QClipboard>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QMimeData>
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
    tree->setCurrentItem(pager(500));
    QTest::keyClick(tree, Qt::Key_Return);
    assert(waitFor([&]{return pager(0)!=nullptr && tree->topLevelItem(0)->childCount()==151;}));
    assert(gap<1000);window.grab().save("/tmp/turtle-large-project.png");
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
    QTest::keyClick(tree, Qt::Key_H);
    assert(!folderItem->isExpanded() && tabs->count() == 0);
    QTest::keyClick(tree, Qt::Key_L);
    QTest::keyClick(tree, Qt::Key_J);
    QTest::keyClick(tree, Qt::Key_Return);
    auto* editor = qobject_cast<EditorWidget*>(tabs->currentWidget());
    assert(editor && waitFor([&] { return !editor->isBusy() && editor->filePath() == original; }));
    auto* title = window.findChild<QToolButton*>("editorPaneTitle");
    assert(editor->hasFocus() && title && !title->accessibleName().startsWith("Preview of "));

    QSignalSpy symbolRenames(symbolRename, &QAction::triggered);
    QTest::keyClick(editor, Qt::Key_F2);
    assert(symbolRenames.count() == 1);
    QTest::keyClick(editor, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
    assert(tree->hasFocus() && tree->currentItem() == folderItem->child(0));
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
    assert(QDir().mkpath(source + "/nested") && QDir().mkpath(target));
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
    write(sourceFile, "text\n");
    write(source + "/.hidden", "hidden\n");
    assert(QFile::link(root.filePath("missing"), source + "/nested/link"));
    const auto privateReadOnly = QFile::ReadOwner | QFile::ExeOwner;
    assert(QFile::setPermissions(source + "/nested", privateReadOnly));

    MainWindow window;
    window.show();
    window.activateWindow();
    window.openPaths({root.path()});
    auto* tree = window.findChild<ProjectExplorer*>();
    auto* tabs = window.findChild<QTabWidget*>("editorTabs");
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
    closeDiscard(window);
    assert(QFile::setPermissions(sourceCopy + "/nested", privateReadOnly | QFile::WriteOwner));
    assert(QFile::setPermissions(QFileInfo(moved).absolutePath(), privateReadOnly | QFile::WriteOwner));
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
