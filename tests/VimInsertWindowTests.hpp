#pragma once
#include <QInputMethodEvent>
static void vimInsertWindowTests(QTemporaryDir& directory) {
    using Mode=VimController::Mode;
    QSettings().setValue("editor/vimMode",true);QSettings().setValue("editor/recoveryEnabled",false);QSettings().setValue("editor/autoPairs",false);
    QSettings().setValue("vimKeymaps/bindings", "insert jk = keys:<Esc>\ninsert uvw = keys:<Esc>\ninsert xy = keys:jk\ninsert ab = keys:<Esc>\ninsert abc = keys:Q");
    ThemeManager::instance().initialize();
    const auto path=directory.filePath("vim-insert.txt");{QFile file(path);assert(file.open(QIODevice::WriteOnly));file.write("alpha alphabet\nal");}
    MainWindow window;window.resize(1100,720);window.show();window.openPaths({path});
    auto* editor=window.findChild<EditorWidget*>();assert(editor);
    const auto idle=[&]{return !editor->isBusy() && !editor->isVimReplaying() && !editor->vimCommandExecuting();};
    assert(waitFor(idle));window.activateWindow();editor->setFocus();QTest::qWait(30);
    QTest::keyClicks(editor,"GA");QTest::keyClick(editor,Qt::Key_N,Qt::ControlModifier);assert(waitFor(idle));
    assert(editor->document()->buffer().text()=="alpha alphabet\nalpha" && editor->vimMode()==Mode::Insert);
    QTest::keyClick(editor,Qt::Key_Escape);QTest::keyClick(editor,Qt::Key_U);assert(editor->document()->buffer().text()=="alpha alphabet\nal");
    editor->setNavigationPosition(2,1);QTest::keyClicks(editor,"i");QTest::keyClick(editor,Qt::Key_K,Qt::ControlModifier);QTest::keyClicks(editor,"a:");QTest::keyClick(editor,Qt::Key_Escape);
    assert(editor->document()->buffer().text()=="alpha alphabet\näal");QTest::keyClick(editor,Qt::Key_U);
    editor->setNavigationPosition(2,1);QTest::keyClicks(editor,"i");QTest::keyClick(editor,Qt::Key_V,Qt::ControlModifier);QTest::keyClicks(editor,"x41");QTest::keyClick(editor,Qt::Key_Escape);
    assert(editor->document()->buffer().text()=="alpha alphabet\nAal");QTest::keyClick(editor,Qt::Key_U);
    editor->setNavigationPosition(2,1);QTest::keyClicks(editor,"i");QTest::keyClick(editor,Qt::Key_O,Qt::ControlModifier);QTest::keyClicks(editor,":set dict=words.txt");QTest::keyClick(editor,Qt::Key_Return);assert(waitFor(idle));
    assert(editor->vimMode()==Mode::Insert && editor->hasFocus());QTest::keyClicks(editor,"Z");QTest::keyClick(editor,Qt::Key_Escape);QTest::keyClick(editor,Qt::Key_U);
    int requests=0;QObject::connect(editor,&EditorWidget::vimCompletionRequested,&window,[&]{++requests;});
    QTest::keyClicks(editor,"i");QTest::keyClick(editor,Qt::Key_X,Qt::ControlModifier);QTest::keyClick(editor,Qt::Key_O,Qt::ControlModifier);
    assert(requests==1 && !QApplication::activeModalWidget() && editor->vimMode()==Mode::Insert);QTest::keyClick(editor,Qt::Key_Escape);
    const std::string original = "alpha alphabet\nal";
    const auto text = [&] { return editor->document()->buffer().text(); };
    const auto startInsert = [&] {
        editor->setNavigationPosition(1, 1);
        QTest::keyClick(editor, Qt::Key_Insert);
        assert(editor->vimMode() == Mode::Insert);
    };
    const auto undo = [&] {
        QTest::keyClick(editor, Qt::Key_U);
        assert(waitFor(idle) && text() == original);
    };
    startInsert();
    QTest::keyClicks(editor, "Xj");
    assert(text() == "X" + original && editor->vimMode() == Mode::Insert);
    QTest::keyClicks(editor, "k");
    assert(text() == "X" + original && editor->vimMode() == Mode::Normal);
    undo();

    startInsert();
    QTest::keyClicks(editor, "uvz");
    assert(text() == "uvz" + original);
    QTest::keyClick(editor, Qt::Key_Escape);
    undo();

    startInsert();
    QTest::keyClicks(editor, "j");
    assert(text() == original);
    assert(waitFor([&] { return text() == "j" + original; }));
    QTest::keyClicks(editor, "k");
    assert(text() == "jk" + original && editor->vimMode() == Mode::Insert);
    QTest::keyClick(editor, Qt::Key_Escape);
    QTest::keyClick(editor, Qt::Key_0);
    QTest::keyClicks(editor, ".");
    assert(waitFor(idle) && text() == "jkjk" + original);
    QTest::keyClick(editor, Qt::Key_U);
    assert(text() == "jk" + original);
    undo();

    startInsert();
    QTest::keyClicks(editor, "xy");
    assert(text() == "jk" + original && editor->vimMode() == Mode::Insert);
    QTest::keyClick(editor, Qt::Key_Escape);
    undo();

    startInsert();
    QTest::keyClicks(editor, "abd");
    assert(text() == original && editor->vimMode() == Mode::Normal);
    QTest::keyClick(editor, Qt::Key_Escape);
    startInsert();
    QTest::keyClicks(editor, "abc");
    assert(text() == "Q" + original && editor->vimMode() == Mode::Insert);
    QTest::keyClick(editor, Qt::Key_Escape);
    undo();

    startInsert();
    QTest::keyClicks(editor, "j");
    QInputMethodEvent input;
    input.setCommitString(QStringLiteral("界"));
    QApplication::sendEvent(editor, &input);
    assert(text() == "j界" + original);
    QTest::keyClick(editor, Qt::Key_Escape);
    undo();
    closeDiscard(window);
}
