#pragma once
#include <QSignalSpy>
static void vimExWindowTests(MainWindow& window,EditorWidget* editor,QTemporaryDir& directory){
    auto* tabs=window.findChild<QTabWidget*>();assert(tabs);
    window.activateWindow();tabs->setCurrentWidget(editor);editor->setFocus();QTest::qWait(20);
    const auto path=directory.filePath("ex-source.txt");
    {QFile file(path);assert(file.open(QIODevice::WriteOnly));file.write("one\ntwo\nthree");}
    editor->loadBuffer(PieceTreeBuffer("one\ntwo\nthree"),path);
    const auto read=[](const QString& path){QFile file(path);assert(file.open(QIODevice::ReadOnly));return file.readAll();};
    const auto ex=[&](const QString& command){
        QTest::keyClicks(editor,":"+command);QTest::keyClick(editor,Qt::Key_Return);
        assert(waitFor([&]{return !editor->isBusy() && !editor->vimCommandExecuting() && !editor->isVimReplaying();}));
    };
    QSignalSpy status(editor,&EditorWidget::highlightingStatusChanged);
    QTest::keyClicks(editor,"iX");QTest::keyClick(editor,Qt::Key_Escape);assert(editor->isModified());
    ex("w | $");assert(!editor->isModified() && editor->navigationPosition().x()==3 && read(path).startsWith("Xone\n"));
    const auto copy=directory.filePath("ex-copy.txt");
    QTest::keyClicks(editor,"iY");QTest::keyClick(editor,Qt::Key_Escape);
    ex("w "+copy);assert(editor->filePath()==path && editor->isModified() && read(copy).contains("Ythree"));
    {QFile file(copy);assert(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("keep");}
    ex("w "+copy+" | 1d");assert(read(copy)=="keep" && status.last()[0].toString().startsWith("E13") && editor->document()->buffer().text().starts_with("Xone"));
    ex("w! "+copy);assert(read(copy).contains("Ythree"));
    const auto range=directory.filePath("ex-range.txt");ex("2w "+range);assert(read(range)=="two\n");
    // Rejected close does not show a modal save dialog or discard changes.
    ex("q");assert(tabs->indexOf(editor)>=0 && editor->isModified() && status.last()[0].toString().startsWith("E37"));
    // External changes stop the chain; explicit ! authorizes an overwrite.
    {QFile file(path);assert(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("external");}
    ex("w | 1d");assert(read(path)=="external" && editor->document()->buffer().text().starts_with("Xone"));
    ex("w!");assert(!editor->isModified() && read(path).startsWith("Xone"));
    ex("1w!");assert(editor->isModified() && read(path)=="Xone\n");
    ex("w");assert(!editor->isModified() && read(path).contains("Ythree"));
    // Command line owns Tab, history, the status indicator and input focus.
    QTest::keyClicks(editor,":w ");auto* input=editor->findChild<QLineEdit*>("vimCommandInput");assert(input && input->hasFocus());
    auto* indicator=window.findChild<QLabel*>("vimModeIndicator");assert(indicator && indicator->text()=="COMMAND" && indicator->isEnabled());
    window.grab().save("/tmp/turtle-vim-ex.png");QTest::keyClick(input,Qt::Key_Escape);assert(editor->hasFocus());
    // Saving inside a recorded macro waits for the write before the next command.
    QTest::keyClicks(editor,"qz:w");QTest::keyClick(editor,Qt::Key_Return);
    assert(waitFor([&]{return !editor->vimCommandExecuting() && !editor->isBusy();}));QTest::keyClick(editor,Qt::Key_Q);
    QTest::keyClicks(editor,"iZ");QTest::keyClick(editor,Qt::Key_Escape);QTest::keyClicks(editor,"@z");
    assert(waitFor([&]{return !editor->isVimReplaying() && !editor->vimCommandExecuting() && !editor->isBusy();}));assert(!editor->isModified() && read(path).startsWith("Xone"));
    // :wq waits for a successful save and closes only its originating tab.
    const auto closingPath=directory.filePath("ex-close.txt");{QFile file(closingPath);assert(file.open(QIODevice::WriteOnly));file.write("close");}
    window.openPaths({closingPath});auto* closing=qobject_cast<EditorWidget*>(tabs->currentWidget());assert(closing && closing!=editor);
    assert(waitFor([&]{return !closing->isBusy();}));closing->setFocus();QTest::keyClicks(closing,"iX");QTest::keyClick(closing,Qt::Key_Escape);
    QPointer<EditorWidget> guard=closing;QTest::keyClicks(closing,":wq");QTest::keyClick(closing,Qt::Key_Return);
    assert(waitFor([&]{return guard.isNull();}));assert(read(closingPath).startsWith("Xclose") && tabs->indexOf(editor)>=0);
    window.openPaths({closingPath});closing=qobject_cast<EditorWidget*>(tabs->currentWidget());assert(waitFor([&]{return !closing->isBusy();}));closing->setFocus();
    QTest::keyClicks(closing,"i!");QTest::keyClick(closing,Qt::Key_Escape);guard=closing;
    QTest::keyClicks(closing,":q!");QTest::keyClick(closing,Qt::Key_Return);assert(waitFor([&]{return guard.isNull();}));assert(read(closingPath).startsWith("Xclose"));
    tabs->setCurrentWidget(editor);editor->setFocus();
}
