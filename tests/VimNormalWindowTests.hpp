#pragma once
static void vimNormalWindowTests(MainWindow& window,EditorWidget* editor){
    auto* tabs=window.findChild<QTabWidget*>();assert(tabs);tabs->setCurrentWidget(editor);window.activateWindow();editor->setFocus();
    const auto idle=[&]{return !editor->isBusy() && !editor->isVimReplaying() && !editor->vimCommandExecuting();};
    VimState::Session::instance().clear();editor->loadBuffer(PieceTreeBuffer("one\n  two\n9"),editor->filePath());
    QTest::keyClicks(editor,"J");assert(waitFor(idle));assert(editor->document()->buffer().text()=="one two\n9");
    QTest::keyClicks(editor,"G");QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);assert(waitFor(idle));assert(editor->document()->buffer().text()=="one two\n10" && editor->selectedText().isEmpty());
    QTest::keyClick(editor,Qt::Key_X,Qt::ControlModifier);assert(waitFor(idle));assert(editor->document()->buffer().text()=="one two\n9" && editor->selectedText().isEmpty());
    QTest::keyClicks(editor,"gggUw");assert(waitFor(idle));assert(editor->document()->buffer().text()=="ONE two\n9");
    QTest::keyClick(editor,Qt::Key_U);assert(waitFor(idle));assert(editor->document()->buffer().text()=="one two\n9");
    QTest::keyClicks(editor,":set tw=5");QTest::keyClick(editor,Qt::Key_Return);assert(waitFor(idle));QTest::keyClicks(editor,"gqq");assert(waitFor(idle));assert(editor->document()->buffer().text()=="one\ntwo\n9");
    QTest::keyClicks(editor,"ggg;");assert(waitFor(idle));assert(editor->vimMode()==VimController::Mode::Normal && editor->hasFocus());
    QTest::keyClicks(editor,"mA");EditorWidget* other=nullptr;
    for(int n=0;n<tabs->count();++n)if(auto* item=qobject_cast<EditorWidget*>(tabs->widget(n));item && item!=editor && !item->filePath().isEmpty() && QFileInfo::exists(item->filePath())){other=item;break;}
    assert(other);tabs->setCurrentWidget(other);other->setFocus();QTest::keyClicks(other,"g'A");assert(waitFor(idle));assert(tabs->currentWidget()==editor);
    QTest::keyClick(editor,Qt::Key_O,Qt::ControlModifier);assert(waitFor([&]{return tabs->currentWidget()==other && !other->isBusy();}));tabs->setCurrentWidget(editor);editor->setFocus();
}
