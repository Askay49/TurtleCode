#pragma once
static void vimSearchWindowTests(MainWindow& window,EditorWidget* editor){
    VimState::Session::instance().clear();editor->loadBuffer(PieceTreeBuffer("one foo two\nfoo tail"),editor->filePath());
    window.activateWindow();editor->setFocus();QTest::qWait(20);
    const auto settled=[&]{return !editor->isBusy() && !editor->isVimReplaying() && (!editor->vimCommandExecuting() || editor->vimSubstitutionConfirming());};
    QTest::keyClicks(editor,"/foo");auto* input=editor->findChild<QLineEdit*>("vimCommandInput");assert(input && input->hasFocus());
    assert(waitFor([&]{return !editor->vimSearchRunning();}));assert(editor->navigationPosition()==QPoint(1,5) && !editor->isModified());
    window.grab().save("/tmp/turtle-vim-search.png");QTest::keyClick(input,Qt::Key_Escape);assert(editor->navigationPosition()==QPoint(1,1) && editor->hasFocus());
    QTest::keyClicks(editor,"/foo");QTest::keyClick(editor,Qt::Key_Return);assert(waitFor(settled));assert(editor->navigationPosition()==QPoint(1,5));QTest::qWait(120);
    const auto colored=[&]{const auto picture=editor->viewport()->grab().toImage();int count=0;for(int y=0;y<picture.height();++y)for(int x=0;x<picture.width();++x){const auto color=picture.pixelColor(x,y);if(color==ThemeManager::instance().color("search.match") || color==ThemeManager::instance().color("search.activeMatch"))++count;}return count;};
    assert(colored()>10);
    QTest::keyClicks(editor,":noh");QTest::keyClick(editor,Qt::Key_Return);assert(waitFor(settled));QTest::qWait(30);assert(colored()==0);
    QTest::keyClick(editor,Qt::Key_N);assert(waitFor(settled));assert(editor->navigationPosition()==QPoint(2,1));QTest::qWait(80);assert(colored()>10);
    QTest::keyClicks(editor,":%s/foo/bar/gc");QTest::keyClick(editor,Qt::Key_Return);assert(waitFor(settled));assert(editor->vimSubstitutionConfirming());
    auto* indicator=window.findChild<QLabel*>("vimModeIndicator");assert(indicator && indicator->text()=="CONFIRM");
    window.grab().save("/tmp/turtle-vim-substitute.png");QTest::keyClick(editor,Qt::Key_Y);assert(waitFor(settled));QTest::keyClick(editor,Qt::Key_Q);assert(waitFor(settled));
    assert(editor->document()->buffer().text()=="one bar two\nfoo tail" && !editor->vimSubstitutionConfirming());
    QTest::keyClick(editor,Qt::Key_U);assert(waitFor(settled));assert(editor->document()->buffer().text()=="one foo two\nfoo tail");
    editor->setNavigationPosition(1,1);QTest::keyClicks(editor,"iX");QTest::keyClick(editor,Qt::Key_Escape);
    QTest::keyClicks(editor,":%s/foo/\\=1+1/");QTest::keyClick(editor,Qt::Key_Return);assert(waitFor(settled));
    assert(editor->document()->buffer().text().starts_with("Xone") && editor->isModified());
}
