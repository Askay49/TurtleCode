#pragma once
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QStatusBar>
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
    Q_EMIT tree->itemClicked(pager(500),0);
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
