#pragma once
static void vimExExtendedWindowTests(MainWindow& window,EditorWidget* original,QTemporaryDir& directory){
    auto* tabs=window.findChild<QTabWidget*>();assert(tabs);
    auto& state=VimState::Session::instance();state.clear();
    const auto write=[](const QString& path,const QByteArray& text){QFile file(path);assert(file.open(QIODevice::WriteOnly|QIODevice::Truncate));assert(file.write(text)==text.size());};
    const auto read=[](const QString& path){QFile file(path);assert(file.open(QIODevice::ReadOnly));return file.readAll();};
    const auto first=directory.filePath("extended first.txt"),second=directory.filePath("extended-second.txt"),saved=directory.filePath("extended-saved.txt");
    write(first,"one\ntwo\nthree");write(second,"alpha\nbeta\n");
    const auto quote=[](QString path){return path.replace(" ","\\ ");};
    const auto current=[&]{return qobject_cast<EditorWidget*>(tabs->currentWidget());};
    const auto idle=[&]{for(auto* item:window.findChildren<EditorWidget*>())if(item->isBusy() || item->vimCommandExecuting() || item->isVimReplaying())return false;return true;};
    const auto ex=[&](const QString& text){auto* source=current();assert(source);source->setFocus();QTest::keyClicks(source,":"+text);QTest::keyClick(source,Qt::Key_Return);assert(waitFor(idle));};
    tabs->setCurrentWidget(original);ex("e "+quote(first)+" | 2");auto* a=current();assert(a!=original && a->filePath()==first && a->navigationPosition()==QPoint(2,1));
    const auto aId=a->property("vimBufferId").toString();
    ex("e "+quote(second)+" | $");auto* b=current();assert(b!=a && b->filePath()==second && b->navigationPosition()==QPoint(2,1));
    const auto bId=b->property("vimBufferId").toString();
    ex("b # | 1");assert(current()==a && a->navigationPosition().x()==1);
    ex("buffer "+bId+" | 1d");assert(current()==b && b->document()->buffer().text()=="beta\n");
    ex("bd");assert(current()==b && b->isModified());
    QTest::keyClick(b,Qt::Key_U);assert(waitFor(idle));assert(b->document()->buffer().text()=="alpha\nbeta\n");
    ex("b "+aId);ex("0r "+quote(second));assert(a->document()->buffer().text()=="alpha\nbeta\none\ntwo\nthree");
    QTest::keyClick(a,Qt::Key_U);assert(waitFor(idle));assert(a->document()->buffer().text()=="one\ntwo\nthree");
    a->setNavigationPosition(2,2);QTest::keyClicks(a,"ma");ex("saveas "+quote(saved));assert(a->filePath()==saved && read(saved)=="one\ntwo\nthree");
    QTest::keyClicks(a,"gg`a");assert(a->navigationPosition()==QPoint(2,2));
    write(saved,"new\ncontents");ex("edit | $");assert(a->document()->buffer().text()=="new\ncontents" && a->navigationPosition().x()==2);
    QTest::keyClicks(a,"i!");QTest::keyClick(a,Qt::Key_Escape);ex("edit");assert(a->document()->buffer().text()=="new\n!contents");
    ex("edit! | 1");assert(a->document()->buffer().text()=="new\ncontents" && !a->isModified() && a->navigationPosition().x()==1);
    // Register programs retain subsequent lines when :buffer transfers the interpreter.
    state.set('z',VimState::text("buffer "+bId+"\n1d"));ex("@z");assert(current()==b && b->document()->buffer().text()=="beta\n");
    QTest::keyClick(b,Qt::Key_U);assert(waitFor(idle));
    // A recorded macro resumes only after the new document is selected and loaded.
    b->setNavigationPosition(1,1);tabs->setCurrentWidget(a);a->setFocus();QTest::keyClicks(a,"qx:b "+bId);QTest::keyClick(a,Qt::Key_Return);assert(waitFor(idle));
    assert(current()==b);QTest::keyClicks(b,"i!");QTest::keyClick(b,Qt::Key_Escape);QTest::keyClick(b,Qt::Key_Q);QTest::keyClick(b,Qt::Key_U);assert(waitFor(idle));
    tabs->setCurrentWidget(a);a->setFocus();QTest::keyClicks(a,"@x");assert(waitFor(idle));if(current()!=b || b->document()->buffer().text()!="!alpha\nbeta\n")qFatal("buffer macro current=%d text=[%s] at=%d:%d",current()==b,b->document()->buffer().text().c_str(),b->navigationPosition().x(),b->navigationPosition().y());
    QTest::keyClick(b,Qt::Key_U);assert(waitFor(idle));
    b->setNavigationPosition(1,1);tabs->setCurrentWidget(a);a->setFocus();state.set('z',VimState::text(":b "+bId+"\ri?"+QChar(27)));ex("normal @z");assert(current()==b && b->document()->buffer().text()=="?alpha\nbeta\n");QTest::keyClick(b,Qt::Key_U);assert(waitFor(idle));
    QPointer<EditorWidget> deleted=b;ex("bd | 1");assert(waitFor([&]{return deleted.isNull();}));assert(current()!=b);
    ex("enew");assert(current()->filePath().isEmpty());const auto renamed=directory.filePath("extended-renamed.txt");ex("file "+quote(renamed));assert(current()->filePath()==renamed && !QFileInfo::exists(renamed));
    QTest::keyClicks(current(),"iX");QTest::keyClick(current(),Qt::Key_Escape);ex("edit");assert(current()->document()->buffer().text()=="X");ex("edit!");assert(current()->byteSize()==0);
    ex("w");assert(QFileInfo::exists(renamed));
    tabs->setCurrentWidget(original);original->setFocus();
}
