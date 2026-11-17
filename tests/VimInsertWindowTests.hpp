#pragma once
static void vimInsertWindowTests(QTemporaryDir& directory) {
    using Mode=VimController::Mode;
    QSettings().setValue("editor/vimMode",true);QSettings().setValue("editor/recoveryEnabled",false);QSettings().setValue("editor/autoPairs",false);
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
    closeDiscard(window);
}
