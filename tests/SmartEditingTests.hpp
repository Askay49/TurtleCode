#pragma once
#include <QSettings>
#include <QInputMethodEvent>
#include <cassert>
#include <QVBoxLayout>
#include <QPushButton>
#include <QWindow>

static void smartEditingTests() {
    const auto text=[](EditorWidget& e){return e.document()->buffer().text();};
    const auto type=[](EditorWidget& e,char c) {
        QKeyEvent event(QEvent::KeyPress,0,Qt::NoModifier,QString(QChar::fromLatin1(c)));
        QApplication::sendEvent(&e,&event);
    };
    const auto undo=[](EditorWidget& e){QTest::keyClick(&e,Qt::Key_Z,Qt::ControlModifier);};
    const auto all=[](EditorWidget& e){QTest::keyClick(&e,Qt::Key_A,Qt::ControlModifier);};
    const auto settle=[](EditorWidget& e) {
        QElapsedTimer timer;timer.start();while(e.isBusy() && timer.elapsed()<15000){QApplication::processEvents();QThread::msleep(1);}assert(!e.isBusy());
    };
    {
        // Send keys through QWindow, with a real neighbouring focus target.
        QWidget window;auto* layout=new QVBoxLayout(&window);
        auto* editor=new EditorWidget(&window);auto* button=new QPushButton("Next focus target",&window);
        layout->addWidget(editor);layout->addWidget(button);
        editor->loadBuffer(PieceTreeBuffer("abc\ndef"),"tab.cpp");
        window.resize(700,400);window.show();window.activateWindow();editor->setFocus();QTest::qWait(30);
        assert(QApplication::focusWidget()==editor && window.windowHandle());
        QTest::keyClick(window.windowHandle(),Qt::Key_Tab);
        assert(text(*editor)=="    abc\ndef" && QApplication::focusWidget()==editor);
        QTest::keyClick(window.windowHandle(),Qt::Key_Tab,Qt::ShiftModifier);
        assert(text(*editor)=="abc\ndef" && QApplication::focusWidget()==editor);
        all(*editor);QTest::keyClick(window.windowHandle(),Qt::Key_Tab);
        assert(text(*editor)=="    abc\n    def" && !editor->selectedText().isEmpty());
        QTest::keyClick(window.windowHandle(),Qt::Key_Backtab,Qt::ShiftModifier);
        assert(text(*editor)=="abc\ndef" && editor->selectedText()=="abc\ndef" && QApplication::focusWidget()==editor);
        editor->setBusy(true);QTest::keyClick(window.windowHandle(),Qt::Key_Tab);
        assert(text(*editor)=="abc\ndef" && QApplication::focusWidget()==editor);editor->setBusy(false);
        editor->showFindBar(true);auto* find=editor->findChild<QLineEdit*>("fileFindInput");assert(find);
        find->setFocus();QTest::keyClick(window.windowHandle(),Qt::Key_Tab);
        assert(QApplication::focusWidget()!=find && text(*editor)=="abc\ndef");
        QTest::keyClick(find,Qt::Key_Escape);editor->loadBuffer(PieceTreeBuffer("ca"),"tab.cpp");
        editor->setNavigationPosition(1,3);editor->setFocus();
        editor->showCompletions(QJsonArray{QJsonObject{{"label","call"},{"insertText","call"}}});
        auto* popup=editor->findChild<QCompleter*>()->popup();assert(popup->isVisible());
        QTest::keyClick(popup->windowHandle(),Qt::Key_Tab);
        assert(text(*editor)=="call" && !popup->isVisible());
    }
    QSettings().setValue("editor/autoPairs",true);
    for(const auto opening:std::string("([{\"'")) {
        EditorWidget e;load(e,"");type(e,opening);
        assert(text(e)==std::string({opening,CodeContext::closeFor(opening)}));
        assert(e.navigationPosition()==QPoint(1,2));
        type(e,CodeContext::closeFor(opening));assert(text(e).size()==2 && e.navigationPosition()==QPoint(1,3));
        undo(e);assert(text(e).empty());
        type(e,opening);QTest::keyClick(&e,Qt::Key_Backspace);assert(text(e).empty());
        undo(e);assert(text(e)==std::string({opening,CodeContext::closeFor(opening)}));
    }
    for(const auto source:std::vector<std::string>{"// comment ","/* block\ncomment ","\"string ","\"continued\\\r\nstring ","R\"tag(raw\ntext ","'x"}) {
        EditorWidget e;load(e,source.c_str());QTest::keyClick(&e,Qt::Key_End,Qt::ControlModifier);type(e,'(');
        assert(text(e)==source+"(");
    }
    {
        EditorWidget e;load(e,"1");QTest::keyClick(&e,Qt::Key_End);type(e,'\'');type(e,'0');type(e,'(');
        assert(text(e)=="1'0()");
        load(e,"");type(e,'"');type(e,'\\');type(e,'"');assert(text(e)=="\"\\\"\"");
        load(e,"word");e.setNavigationPosition(1,3);type(e,'(');assert(text(e)=="wo(rd");
        load(e,"");type(e,'<');assert(text(e)=="<");
    }
    for(const bool reversed:{false,true}) {
        EditorWidget e;load(e,"привет 😀\nnext");
        if(!reversed)all(e);
        else {QTest::keyClick(&e,Qt::Key_End,Qt::ControlModifier);QTest::keyClick(&e,Qt::Key_Home,Qt::ControlModifier|Qt::ShiftModifier);}
        type(e,'(');assert(text(e)=="(привет 😀\nnext)" && e.selectedText()==QString::fromUtf8("привет 😀\nnext"));
        assert(e.navigationPosition()==(reversed?QPoint(1,2):QPoint(2,5)));
        undo(e);assert(text(e)=="привет 😀\nnext");
    }
    {
        EditorWidget e;load(e,"foo foo\n");QTest::keyClick(&e,Qt::Key_D,Qt::ControlModifier);assert(e.cursorCount()==2);
        type(e,'"');assert(text(e)=="\"foo\" \"foo\"\n" && e.selectedText()=="foo");undo(e);assert(text(e)=="foo foo\n");
        load(e,"a\nb\n");e.setNavigationPosition(1,2);QTest::keyClick(&e,Qt::Key_Down,Qt::ControlModifier|Qt::ShiftModifier);
        type(e,'(');assert(text(e)=="a()\nb()\n");QTest::keyClick(&e,Qt::Key_Backspace);assert(text(e)=="a\nb\n");
        undo(e);assert(text(e)=="a()\nb()\n");
    }
    {
        EditorWidget e;load(e,"    if (x) {}");e.setNavigationPosition(1,13);
        QTest::keyClick(&e,Qt::Key_Return);assert(text(e)=="    if (x) {\n        \n    }" && e.navigationPosition()==QPoint(2,9));
        undo(e);assert(text(e)=="    if (x) {}");
        load(e,"\t{\r\n}");e.setNavigationPosition(1,3);QTest::keyClick(&e,Qt::Key_Return);
        assert(text(e)=="\t{\r\n\t\t\r\n}");undo(e);
        load(e,"  // { comment");QTest::keyClick(&e,Qt::Key_End);QTest::keyClick(&e,Qt::Key_Return);
        assert(text(e)=="  // { comment\n  ");
        load(e,"\xef\xbb\xbf{\r\n");e.setNavigationPosition(1,3);QTest::keyClick(&e,Qt::Key_Return);
        assert(text(e)==std::string("\xef\xbb\xbf")+"{\r\n    \r\n");
    }
    for(const bool reversed:{false,true}) {
        EditorWidget e;load(e,"one\n  два\nlast");
        if(reversed){e.setNavigationPosition(3,1);QTest::keyClick(&e,Qt::Key_Home,Qt::ControlModifier|Qt::ShiftModifier);}
        else {QTest::keyClick(&e,Qt::Key_Down,Qt::ShiftModifier);QTest::keyClick(&e,Qt::Key_Down,Qt::ShiftModifier);}
        QTest::keyClick(&e,Qt::Key_Tab);assert(text(e)=="    one\n      два\nlast");
        assert(!e.selectedText().isEmpty());
        QTest::keyClick(&e,Qt::Key_Backtab,Qt::ShiftModifier);assert(text(e)=="one\n  два\nlast");
        assert(e.selectedText()==QString::fromUtf8("one\n  два\n"));
        undo(e);assert(text(e)=="    one\n      два\nlast");
    }
    {
        EditorWidget e;load(e,"\xef\xbb\xbf\tfoo\r\n  bar");all(e);QTest::keyClick(&e,Qt::Key_Tab);
        assert(text(e)==std::string("\xef\xbb\xbf")+"    \tfoo\r\n      bar");
        QTest::keyClick(&e,Qt::Key_Backtab,Qt::ShiftModifier);assert(text(e)==std::string("\xef\xbb\xbf")+"\tfoo\r\n  bar");
        e.setNavigationPosition(1,2);QTest::keyClick(&e,Qt::Key_Backtab,Qt::ShiftModifier);
        assert(text(e)==std::string("\xef\xbb\xbf")+"foo\r\n  bar");
    }
    {
        EditorWidget e;load(e,"  int x;\r\n    int y;\r\n\r\nlast");e.setNavigationPosition(4,1);
        QTest::keyClick(&e,Qt::Key_Home,Qt::ControlModifier|Qt::ShiftModifier);
        QTest::keyClick(&e,Qt::Key_Slash,Qt::ControlModifier);
        assert(text(e)=="  // int x;\r\n    // int y;\r\n\r\nlast");
        QTest::keyClick(&e,Qt::Key_Slash,Qt::ControlModifier);assert(text(e)=="  int x;\r\n    int y;\r\n\r\nlast");
        undo(e);assert(text(e)=="  // int x;\r\n    // int y;\r\n\r\nlast");
        load(e,"привет😀");all(e);QTest::keyClick(&e,Qt::Key_A,Qt::ControlModifier|Qt::ShiftModifier);
        assert(text(e)=="/*привет😀*/" && e.selectedText()==QString::fromUtf8("привет😀"));
        QTest::keyClick(&e,Qt::Key_A,Qt::ControlModifier|Qt::ShiftModifier);assert(text(e)=="привет😀");
        load(e,"/* existing */ x");all(e);e.toggleBlockComment();assert(text(e)=="/* existing */ x");
        e.loadBuffer(PieceTreeBuffer("  x\n  y"),"test.py");all(e);e.toggleLineComment();assert(text(e)=="  # x\n  # y");
        e.toggleBlockComment();assert(text(e)=="  x\n  y"); // Python uses real comments, not string literals.
        e.loadBuffer(PieceTreeBuffer("<tag/>"),"test.html");all(e);e.toggleLineComment();assert(text(e)=="<!--<tag/>-->");
        e.toggleLineComment();assert(text(e)=="<tag/>");
        e.loadBuffer(PieceTreeBuffer("{}"),"test.json");all(e);e.toggleLineComment();e.toggleBlockComment();assert(text(e)=="{}");
        e.loadBuffer(PieceTreeBuffer("key: value"),"test.yaml");e.toggleLineComment();assert(text(e)=="# key: value");
        e.loadBuffer(PieceTreeBuffer("value=1"),"test.ini");e.toggleLineComment();assert(text(e)=="; value=1");
        e.loadBuffer(PieceTreeBuffer("SELECT 1;"),"test.sql");e.toggleLineComment();assert(text(e)=="-- SELECT 1;");
    }
    {
        EditorWidget e;load(e,"");QSettings().setValue("editor/autoPairs",false);type(e,'(');assert(text(e)=="(");
        QSettings().setValue("editor/autoPairs",true);
        QApplication::clipboard()->setText("(");QTest::keyClick(&e,Qt::Key_V,Qt::ControlModifier);assert(text(e)=="((");
        QInputMethodEvent commit;commit.setCommitString("(");QApplication::sendEvent(&e,&commit);assert(text(e)=="(((");
        e.setBusy(true);type(e,'{');e.toggleLineComment();e.indentSelection();assert(text(e)=="(((");e.setBusy(false);
    }
    {
        // Plan and lexical checks avoid relying only on painted pixels.
        const auto check=[](const std::string& source,std::size_t cursor,std::optional<std::size_t> partner,bool unmatched=false) {
            PieceTreeBuffer buffer(source);CodeContext::Cache cache;auto match=CodeContext::match(buffer,cursor,cache,{});
            assert(!match.pending && match.second==partner && match.unmatched==unmatched);
        };
        check("(a[0])",0,5);check("(a[0])",6,0);check("{ \"}\" /* } */ (x) }",0,18);
        check("{ R\"x(})x\" }",0,11);check("/* { */",4,{});check("{]",0,{},true);check("{",0,{},true);
        PieceTreeBuffer buffer(std::string(300000,' ')+"/*\n{\n*/\n()");CodeContext::Cache cache;
        auto state=cache.at(buffer,300004,{},4096);assert(!state);
        for(int i=0;i<100 && !state;++i)state=cache.at(buffer,300004,{},4096);
        assert(state && state->mode==CodeContext::Mode::BlockComment);
        buffer.erase(300000,2);cache.invalidate(300000);
        state=cache.at(buffer,300002,{});assert(state && state->mode==CodeContext::Mode::Code);
    }
    {
        EditorWidget e;const auto source=std::string(1100000,'x')+"\ntail";load(e,source.c_str());all(e);e.indentSelection();assert(e.isBusy());settle(e);
        assert(text(e)=="    "+std::string(1100000,'x')+"\n    tail");undo(e);settle(e);assert(text(e)==source);
        all(e);e.toggleLineComment();assert(e.isBusy());auto dialogs=e.findChildren<QProgressDialog*>();assert(!dialogs.isEmpty());dialogs.back()->cancel();settle(e);assert(text(e)==source);
        all(e);type(e,')');assert(e.isBusy());settle(e);assert(text(e)==")");undo(e);settle(e);assert(text(e)==source);
    }
    {
        // Offscreen render: matching brackets must add a visible border.
        EditorWidget e;load(e,"(value)");e.setNavigationPosition(1,1);QTest::qWait(80);
        QImage highlighted(e.viewport()->size(),QImage::Format_ARGB32);e.viewport()->render(&highlighted);
        QSettings().setValue("editor/highlightBrackets",false);
        QImage plain(e.viewport()->size(),QImage::Format_ARGB32);e.viewport()->render(&plain);
        assert(highlighted!=plain);QSettings().setValue("editor/highlightBrackets",true);
    }
}
