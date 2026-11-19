#pragma once
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
