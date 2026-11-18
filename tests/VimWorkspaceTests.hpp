#pragma once

#include "app/CommandPalette.hpp"
#include "editor/EditorPaneWorkspace.hpp"
#include <QTabBar>
#include <cstdlib>

static void vimWorkspaceTests(QTemporaryDir& directory) {
    QSettings().setValue("editor/vimMode", false);
    QSettings().setValue("editor/recoveryEnabled", false);
    ThemeManager::instance().initialize();
    const auto write = [&](const QString& name, const QByteArray& text) {
        const auto path = directory.filePath(name);
        QFile file(path);
        assert(file.open(QIODevice::WriteOnly));
        assert(file.write(text) == text.size());
        return path;
    };
    const auto firstPath = write("split-first.txt", "start\nleft\nsame\nend\n");
    const auto secondPath = write("split-second.txt", "start\nright\nextra\nsame\nlast\n");
    MainWindow window;
    window.resize(1100, 740);
    window.show();
    window.openPaths({firstPath});
    auto* tabs = window.findChild<QTabWidget*>("editorTabs");
    auto* primary = qobject_cast<EditorWidget*>(tabs->currentWidget());
    auto* workspace = dynamic_cast<EditorPaneWorkspace*>(window.findChild<QStackedWidget*>("editorPaneWorkspace"));
    assert(primary && workspace && waitFor([&] { return !primary->isBusy(); }));
    const auto active = [&] { return workspace->activeEditor(); };
    const auto text = [](const EditorWidget* view) { return QByteArray::fromStdString(view->snapshotBuffer().text()); };
    const auto ready = [&] {
        for (auto* editor : window.findChildren<EditorWidget*>())
            if (editor->isBusy() || editor->vimCommandExecuting() || editor->isVimReplaying()) return false;
        return true;
    };
    const auto ex = [&](const QString& command) {
        auto* editor = active();
        assert(editor);
        QTest::keyClick(editor, Qt::Key_Colon);
        auto* input = editor->findChild<QLineEdit*>("vimCommandInput");
        assert(input && input->isVisible());
        input->setText(command);
        QTest::keyClick(input, Qt::Key_Return);
        assert(waitFor(ready));
    };
    const auto windowKey = [&](Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        auto* editor = active();
        QTest::keyClick(editor, Qt::Key_W, Qt::ControlModifier);
        QTest::keyClick(editor, key, modifiers);
        QApplication::processEvents();
    };
    const auto assertEvenSplit = [](QSplitter* splitter) {
        assert(splitter);
        QApplication::processEvents();
        const auto sizes = splitter->sizes();
        assert(sizes.size() == 2 && sizes[0] > 0 && std::abs(sizes[0] - sizes[1]) <= 1);
    };

    window.activateWindow();
    primary->setFocus();
    QTimer::singleShot(0, [&] {
        auto* palette = dynamic_cast<CommandPalette*>(QApplication::activeModalWidget());
        assert(palette);
        auto* input = palette->findChild<QLineEdit*>("commandPaletteInput");
        input->setText("editor.splitRight");
        QTest::keyClick(input, Qt::Key_Return);
    });
    QTest::keyClick(primary, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    assert(workspace->panes().size() == 2);
    auto* left = workspace->panes().front()->editor();
    auto* right = active();
    auto* splitter = qobject_cast<QSplitter*>(right->parentWidget()->parentWidget());
    assert(splitter && splitter->orientation() == Qt::Horizontal);
    assertEvenSplit(splitter);
    assert(left != right && left->document() == right->document() && right->document() == primary->document());
    assert(!right->vimEnabled());
    assert(waitFor([&] { return right->hasFocus(); }));
    left->setNavigationPosition(3, 2);
    right->setNavigationPosition(1, 1);
    QTest::keyClicks(right, "X");
    assert(text(left).startsWith("Xstart") && primary->isModified() && left->isModified());
    assert(left->navigationPosition() == QPoint(3, 2) && right->navigationPosition() == QPoint(1, 2));
    left->setFocus();
    QTest::keyClick(left, Qt::Key_Z, Qt::ControlModifier);
    assert(text(right).startsWith("start") && !right->isModified());
    assert(window.findChild<QAction*>("editor.closePane")->isEnabled());
    window.findChild<QAction*>("editor.closePane")->trigger();
    assert(workspace->panes().size() == 1 && active() == right);

    window.findChild<QAction*>("settings.vimMode")->trigger();
    const auto firstId = primary->property("vimBufferId").toULongLong();
    ex("vsplit " + secondPath);
    assert(workspace->panes().size() == 2 && active()->filePath() == secondPath);
    auto* second = active();
    auto* secondDocument = qobject_cast<EditorWidget*>(tabs->currentWidget());
    const auto secondId = secondDocument->property("vimBufferId").toULongLong();
    auto* sideBySide = qobject_cast<QSplitter*>(second->parentWidget()->parentWidget());
    assertEvenSplit(sideBySide);
    const int originalWidth = sideBySide->sizes().last();
    windowKey(Qt::Key_Greater);
    assert(sideBySide->sizes().last() > originalWidth);
    windowKey(Qt::Key_X);
    assert(workspace->panes().front()->editor() == second);
    windowKey(Qt::Key_X);
    windowKey(Qt::Key_Equal);
    assert(workspace->panes().back()->editor() == second);
    windowKey(Qt::Key_H);
    assert(active()->filePath() == firstPath);
    windowKey(Qt::Key_L, Qt::ControlModifier);
    assert(active() == second);
    QTest::keyClicks(second, "iX");
    QTest::keyClick(second, Qt::Key_Escape);
    ex("hide");
    assert(workspace->panes().size() == 1 && active()->filePath() == firstPath);
    ex(QStringLiteral("buffer %1").arg(secondId));
    assert(active()->filePath() == secondPath && text(active()).startsWith("Xstart"));
    ex("bdelete");
    assert(active()->filePath() == secondPath && active()->isModified());
    ex("bdelete!");
    assert(active()->filePath() == firstPath && tabs->count() == 1);
    ex(QStringLiteral("buffer %1").arg(secondId));
    assert(active()->filePath() == secondPath && text(active()).startsWith("start"));
    assert(tabs->currentWidget()->property("vimBufferId").toULongLong() == secondId);
    active()->setNavigationPosition(2, 1);
    QTest::keyClicks(active(), "ma");
    ex("split");
    assert(workspace->panes().size() == 2 && active()->filePath() == secondPath);
    ex("bwipeout | 1delete");
    assert(active()->filePath() == firstPath && text(active()) == "left\nsame\nend\n");
    QTest::keyClicks(active(), "u");
    assert(text(active()) == "start\nleft\nsame\nend\n");
    ex(QStringLiteral("buffer %1").arg(secondId));
    assert(active()->filePath() == firstPath && tabs->count() == 1);

    windowKey(Qt::Key_S);
    assert(workspace->panes().size() == 2 && active()->filePath() == firstPath);
    auto* stacked = qobject_cast<QSplitter*>(active()->parentWidget()->parentWidget());
    assert(stacked && stacked->orientation() == Qt::Vertical);
    assertEvenSplit(stacked);
    ex("tabnew " + secondPath + " | 1delete | 1delete");
    assert(workspace->count() == 2 && workspace->panes().size() == 1 && active()->filePath() == secondPath);
    assert(tabs->currentWidget()->property("vimBufferId").toULongLong() != secondId);
    assert(text(active()) == "extra\nsame\nlast\n");
    QTest::keyClicks(active(), "u");
    assert(text(active()) == "start\nright\nextra\nsame\nlast\n");
    active()->setNavigationPosition(4, 1);
    QTest::keyClicks(active(), "'a");
    assert(active()->navigationPosition().x() == 4);
    auto* pages = window.findChild<QTabBar*>("vimPageTabs");
    assert(pages && pages->isVisible() && pages->count() == 2 && pages->currentIndex() == 1);
    ex("tabmove 0");
    assert(pages->currentIndex() == 0 && active()->filePath() == secondPath);
    ex("tabmove $");
    assert(pages->currentIndex() == 1 && active()->filePath() == secondPath);
    QTest::mouseClick(pages, Qt::LeftButton, Qt::NoModifier, pages->tabRect(0).center());
    assert(workspace->currentIndex() == 0 && active()->filePath() == firstPath);
    QTest::mouseClick(pages, Qt::LeftButton, Qt::NoModifier, pages->tabRect(1).center());
    QTest::keyClicks(active(), "gT");
    assert(workspace->currentIndex() == 0 && workspace->panes().size() == 2);
    QTest::keyClicks(active(), "2gt");
    assert(workspace->currentIndex() == 1 && active()->filePath() == secondPath);
    QTest::keyClicks(active(), "gt");
    assert(workspace->currentIndex() == 0 && active()->filePath() == firstPath);
    ex("tabonly");
    ex("only");
    assert(workspace->count() == 1 && workspace->panes().size() == 1);
    assert(!pages->isVisible());

    ex("vert diffsplit " + secondPath);
    auto* destination = active();
    assert(destination->filePath() == secondPath && workspace->panes().size() == 2);
    auto* source = workspace->panes().front()->editor();
    assert(source->filePath() == firstPath && source->visualRowForLine(2) == 3);
    destination->setNavigationPosition(1, 1);
    QTest::keyClicks(destination, "]c");
    assert(destination->navigationPosition().x() == 2);
    QTest::keyClicks(destination, "]c");
    assert(destination->navigationPosition().x() == 5);
    QTest::keyClicks(destination, "[cdo");
    assert(text(destination) == "start\nleft\nsame\nlast\n");
    QTest::keyClicks(destination, "u");
    assert(text(destination) == "start\nright\nextra\nsame\nlast\n");
    ex("2,3diffget missing.txt");
    assert(text(destination) == "start\nright\nextra\nsame\nlast\n");
    ex("2diffget");
    assert(text(destination) == "start\nleft\nextra\nsame\nlast\n");
    QTest::keyClicks(destination, "u");
    ex(QStringLiteral("2,3diffget %1").arg(firstId));
    assert(text(destination) == "start\nleft\nsame\nlast\n");
    QTest::keyClicks(destination, "u");
    destination->setNavigationPosition(5, 1);
    QTest::keyClicks(destination, "dp");
    assert(text(source) == "start\nleft\nsame\nlast\n");
    windowKey(Qt::Key_H);
    assert(active() == source);
    QTest::keyClicks(source, "u");
    assert(text(source) == "start\nleft\nsame\nend\n");
    ex("diffoff!");
    assert(source->visualRowForLine(2) == 2 && tabs->currentWidget()->property("vimBufferId").toULongLong() == firstId);
    auto* diffSplitter = qobject_cast<QSplitter*>(source->parentWidget()->parentWidget());
    assertEvenSplit(diffSplitter);
    const int otherPaneWidth = diffSplitter->sizes().last();
    windowKey(Qt::Key_V);
    const auto widths = diffSplitter->sizes();
    assert(widths.size() == 3 && widths[0] > 0 && std::abs(widths[0] - widths[1]) <= 1);
    assert(std::abs(widths[2] - otherPaneWidth) <= 1);
    closeDiscard(window);
}
