#pragma once

static void vimSessionTests() {
    using Mode=VimController::Mode;
    auto& session=VimState::Session::instance();
    QSettings().setValue("editor/vimMode",true);QSettings().setValue("editor/minimap",false);QSettings().setValue("editor/autoPairs",false);
    EditorWidget editor;
    auto reg=[&](char name){const auto value=session.read(QChar(name));return value.text?value.text->read(0,value.text->size()):std::string{};};
    auto run=[&](const char* initial,const QString& keys,const char* expected) {
        session.clear();load(editor,initial);vimKeys(editor,keys);
        if(editor.document()->buffer().text()!=expected)qFatal("Vim session [%s]: got [%s], expected [%s]",qPrintable(keys),editor.document()->buffer().text().c_str(),expected);
    };
    run("one two","\"ayiw", "one two");Q_ASSERT(reg('a')=="one" && reg('"')=="one" && reg('0').empty());
    QApplication::clipboard()->setText("outside");vimKeys(editor,"x");Q_ASSERT(reg('a')=="one" && QApplication::clipboard()->text()=="outside");
    run("one two","yiww\"_diw\"0p","one one");Q_ASSERT(reg('0')=="one");
    run("a\nb\nc","dddd\"2P","a\nc");Q_ASSERT(reg('1')=="b\n" && reg('2')=="a\n");
    run("one two","\"ayiww\"Ayiw","one two");Q_ASSERT(reg('a')=="onetwo");
    run("one two","ciwX<Esc>w.","X X");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="X two");
    run("a b c d","dw2.","d");
    run("a\nb","iX<Esc>j.","Xa\nXb");
    run("one\ntwo\nthree","qaI><Esc>jq2@a",">one\n>two\n>three");
    vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()==">one\ntwo\nthree");
    run("abcd\nwxyz","l<C-v>jld","ad\nwz");
    run("a\nb","<C-v>jI><Esc>",">a\n>b");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="a\nb");
    run("abcd\nwxyz","l<C-v>jlcQ<Esc>","aQd\nwQz");
    run("abcdef","RXY<BS>Z<Esc>","XZcdef");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="abcdef");
    run("a😀éz","l2rQ","aQQz");
    run("abc","iX<Esc>\".p","XXabc");
    run("abc\ndef","lmaG`a","abc\ndef");Q_ASSERT(editor.navigationPosition()==QPoint(1,2));
    run("abc\ndef","Ggg<C-o>","abc\ndef");Q_ASSERT(editor.navigationPosition()==QPoint(2,1));vimKeys(editor,"<C-i>");Q_ASSERT(editor.navigationPosition()==QPoint(1,1));
    run("clip","\"+yy","clip");Q_ASSERT(QApplication::clipboard()->text()=="clip\n");
    // Recursion terminates, and manual input remains usable afterwards.
    run("abc","qa@aq@a","abc");Q_ASSERT(!editor.isVimReplaying());vimKeys(editor,"ix<Esc>");Q_ASSERT(editor.document()->buffer().text()=="xabc");
    run("abc","qaix<Esc>q","xabc");QTest::keyClicks(&editor,"10000@a");QTest::keyClick(&editor,Qt::Key_Escape);
    QElapsedTimer interrupted;interrupted.start();while(editor.isVimReplaying() && interrupted.elapsed()<10000)QTest::qWait(5);Q_ASSERT(!editor.isVimReplaying() && editor.byteSize()<10004);
    if(editor.byteSize()>4)vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="xabc");
    run("abcd\nwxyz","l<C-v>jl","abcd\nwxyz");QTest::keyClick(&editor,Qt::Key_C,Qt::ControlModifier);while(editor.isBusy())QTest::qWait(5);Q_ASSERT(QApplication::clipboard()->text()=="bc\nxy");
    // Registers are shared by editors, with immutable file payloads for large text.
    session.clear();const auto large=std::string(2*1024*1024,'x')+" tail";editor.loadBuffer(PieceTreeBuffer(large),"large.txt");vimKeys(editor,"\"adiw");Q_ASSERT(reg('a').size()==2*1024*1024);
    {EditorWidget other;load(other,"");vimKeys(other,"\"aP");Q_ASSERT(other.byteSize()==2*1024*1024);}
    Q_ASSERT(session.read('a').text->cachedBytes()<=ChunkedFile::pageBytes*ChunkedFile::maxPages);
    editor.activateWindow();editor.setFocus();QTest::qWait(10);vimKeys(editor,"\"Ayw");Q_ASSERT(reg('a').size()>2*1024*1024);
    session.clear();editor.loadBuffer(PieceTreeBuffer(large),"large-marks.txt");editor.setNavigationPosition(1,1000);vimKeys(editor,"ma0diwu`a");Q_ASSERT(editor.navigationPosition()==QPoint(1,1000));
    session.clear();load(editor,"abc");session.set('a',VimState::text(QString(8192,'x')));vimKeys(editor,"R<C-r>a<Esc>");Q_ASSERT(editor.byteSize()==8192);vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="abc");
    run("abc","10000RX<Esc>",std::string(10000,'X').c_str());vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="abc");
    session.clear();editor.loadBuffer(PieceTreeBuffer(std::string(2*1024*1024,'x')),"large-replace.txt");vimKeys(editor,"VrQ");Q_ASSERT(editor.byteSize()==2*1024*1024 && editor.document()->buffer().byteAt(1000000)=='Q');vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().byteAt(1000000)=='x');
    run("\xef\xbb\xbf" "abc\r\ndef","<C-v>jld","\xef\xbb\xbf" "c\r\nf");
    run("a\r\nb","yyp","a\r\na\r\nb");Q_ASSERT(reg('0')=="a\n");
    session.clear();load(editor,"aa\nbb\ncc");vimKeys(editor,"qai");QInputMethodEvent committed;committed.setCommitString(QString::fromUtf8("Я"));QApplication::sendEvent(&editor,&committed);vimKeys(editor,"<Esc>qj@aj.");Q_ASSERT(editor.document()->buffer().text()=="Яaa\nЯbb\nЯcc");
    session.clear();load(editor,"");vimKeys(editor,"qai");const auto filesBefore=QDir("/proc/self/fd").entryList(QDir::AllEntries|QDir::NoDotAndDotDot).size();
    for(int n=0;n<1000;++n){QInputMethodEvent input;input.setCommitString(QString::fromUtf8("Я"));QApplication::sendEvent(&editor,&input);}
    vimKeys(editor,"<Esc>q@a");Q_ASSERT(editor.byteSize()==4000);const auto filesAfter=QDir("/proc/self/fd").entryList(QDir::AllEntries|QDir::NoDotAndDotDot).size();Q_ASSERT(filesAfter-filesBefore<32);vimKeys(editor,"u");Q_ASSERT(editor.byteSize()==2000);
    session.clear();load(editor,"a\nb");vimKeys(editor,"i");for(char value:{'x','y'}){QInputMethodEvent input;input.setCommitString(QString(64*1024,value));QApplication::sendEvent(&editor,&input);}vimKeys(editor,"<Esc>j.");
    const auto prefix=std::string(64*1024,'x')+std::string(64*1024,'y');Q_ASSERT(editor.document()->buffer().text()==prefix+"a\n"+prefix+"b");
    session.clear();load(editor,"fo\nba");vimKeys(editor,"la");editor.showCompletions(QJsonArray{QJsonObject{{"label","foobar"}}});auto* completer=editor.findChild<QCompleter*>();Q_ASSERT(completer && completer->popup()->isVisible());QTest::keyClick(completer->popup(),Qt::Key_Return);vimKeys(editor,"<Esc>j.");Q_ASSERT(editor.document()->buffer().text()=="foobar\nfoobar");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="foobar\nba");
    const auto nvim=QStandardPaths::findExecutable("nvim");if(nvim.isEmpty())return;
    struct Case {const char* text;QString keys;};
    const std::vector<Case> cases={
        {"one two three","ciwX<Esc>w."},{"a b c d","dw2."},{"a b c d e","2dw."},
        {"abc\ndef","iX<Esc>j."},{"abc\ndef","A!<Esc>j."},{"abc\ndef","oX<Esc>."},
        {"abc def","x."},{"abc def","rXl."},{"one two","viwcX<Esc>w."},
        {"one two","\"ayiww\"ap"},{"one two","\"ayiww\"Ayiw\"aP"},
        {"one two","yiww\"_diw\"0p"},{"a\nb\nc","dddd\"2P"},{"one two","x\"-p"},
        {"one two","yiwi<C-r>0<Esc>"},{"one two","\"ayiwA<C-r>a<Esc>"},
        {"abc\ndef","qaI><Esc>jq@a"},{"a\nb\nc","qaI><Esc>jq2@a"},
        {"abc def","qaxq@a@@"},{"abc def","qaxq@au"},{"abc def","qaxqqAlxq@a"},
        {"abc\ndef","lmaG`a"},{"  abc\ndef","2lmaG'a"},{"abc\ndef","lmaiZ<Esc>`a"},
        {"abc\ndef","maG``"},{"a\nb\nc","Ggg<C-o>"},{"a\nb\nc","Ggg<C-o><C-i>"},
        {"abc","rX"},{"abc","2rX"},{"abc","4rX"},{"a😀éz","l2rQ"},
        {"abcdef","RXY<Esc>"},{"abcdef","RXY<BS>Z<Esc>"},{"abc","R12345<BS><Esc>"},
        {"abcdef","2RXY<Esc>"},{"abc\ndef","RXY<Esc>j."},{"abcdef","RXY<BS>Z<Esc>u"},
        {"abcdef","vlrX"},{"abc\ndef","VjrX"},
        {"abcd\nwxyz","l<C-v>jld"},{"abcd\nwxyz","l<C-v>jlx"},
        {"abcd\nwxyz","l<C-v>jlcQ<Esc>"},{"a\nb","<C-v>jI><Esc>"},
        {"abcd\nwxyz","l<C-v>jlAZ<Esc>"},{"abcd\nwxyz","l<C-v>jlrQ"},
        {"abcd\nwxyz","l<C-v>jlyjp"},{"abcd\nwxyz","l<C-v>jlyP"},
        {"abcd\nx\nwxyz","2l<C-v>jjI!<Esc>"},{"\tabc\n  xyz","<C-v>jld"},
        {"abcd\nwxyz","l<C-v>j$yP"},{"a😀z\nbéx","l<C-v>jld"},
        {"abc def","3lmaddw u`a"},{"abc def","3lmad0u`a"},{"abc\ndef","jmadggu`a"},
        {"abc def","mawd`a"},{"abc\ndef","majd'a"},{"abc def","maw\"ay`a\"ap"},
        {"abc","r<CR>"},{"abc","2r<CR>"},{"abc","r<Tab>"},
        {"abc","i<C-r><Esc>X<Esc>"},{"abc","qaXq@a"},
        {"abcdef","qa2fzxq@a"},{"abc def","yiwqAlxq@a"},
        {"abcd\nwxyz","l<C-v>jlIfoo<Esc>"},{"abcd\nwxyz","l<C-v>jlcfoo<Esc>"},
        {"abcd\nwxyz","l<C-v>jlcQ<Esc>j."},{"one two three","vlx3."},
        {"one two three","yiwwvP"},{"abcd\nwxyz","<C-v>jlyp"},
        {"abcd\nx\nwxyz","2l<C-v>jjAZ<Esc>"},{"abc\n\ndef","<C-v>jjI!<Esc>"},
        {"one\ntwo","\"ayiwj\"Ayy\"aP"},{"one\ntwo","\"ayyj\"Ayiw\"aP"},
        {"abcd\nwxyz","<C-v>j\"ayl<C-v>j\"Ay\"aP"},
        {"abc","R<Tab><Esc>"},{"abc","iX<Esc>\".p"},{"abcdef","vl<Esc>`<x"},
        {"abc\ndef","qaI><Esc>jqi<Esc>"},{"abcd\nwxyz","l<C-v>jlD"},
        {"abcd\nwxyz","l<C-v>jlCfoo<Esc>"},{"abcd\nwxyz","l<C-v>jlX"},
        {"abcd\nwxyz","l<C-v>jlohd"},{"abcd\nwxyz","l<C-v>jlOhd"},
        {"ab\ncd","yiwl<C-v>jlp"}
    };
    int mismatches=0;
    for(const auto& sample:cases) {
        session.clear();load(editor,sample.text);vimKeys(editor,sample.keys);
        const auto encoded=QJsonDocument(QJsonArray{QString::fromUtf8(sample.text),sample.keys}).toJson(QJsonDocument::Compact).toBase64();
        const auto script=QString("lua local t=vim.fn.json_decode(vim.base64 and vim.base64.decode('%1') or vim.fn.system({'base64','-d'},'%1'));vim.o.shiftwidth=4;vim.o.tabstop=4;vim.o.expandtab=true;vim.o.autoindent=true;vim.api.nvim_buf_set_lines(0,0,-1,false,vim.split(t[1],'\\n',{plain=true}));vim.api.nvim_feedkeys(vim.api.nvim_replace_termcodes(t[2],true,false,true),'xt',false);local p=vim.api.nvim_win_get_cursor(0);local regs={};if not string.find(t[2],'q',1,true) then for _,r in ipairs({'\"','0','1','2','-','a'}) do regs[r]=vim.fn.getreg(r) end end;io.stdout:write('ORACLE:'..vim.fn.json_encode({lines=vim.api.nvim_buf_get_lines(0,0,-1,false),row=p[1],column=p[2],regs=regs}))").arg(QString::fromLatin1(encoded));
        QProcess process;process.start(nvim,{"--headless","-u","NONE","-i","NONE","-n","-c",script,"-c","qa!"});Q_ASSERT(process.waitForFinished(10000) && process.exitCode()==0);
        const auto output=process.readAllStandardOutput();const auto at=output.indexOf("ORACLE:");if(at<0)qFatal("Neovim oracle [%s]: stdout [%s], stderr [%s]",qPrintable(sample.keys),output.constData(),process.readAllStandardError().constData());const auto expected=QJsonDocument::fromJson(output.mid(at+7)).object();
        QStringList lines;for(const auto& line:expected["lines"].toArray())lines<<line.toString();const auto expectedText=lines.join('\n');const auto row=expected["row"].toInt();
        const QPoint position(row,QString::fromUtf8(lines.value(row-1).toUtf8().left(expected["column"].toInt())).size()+1);
        if(editor.document()->buffer().text()!=expectedText.toStdString() || editor.navigationPosition()!=position){++mismatches;qWarning("Vim 3 [%s] on [%s]: got [%s] (%d,%d), expected [%s] (%d,%d)",qPrintable(sample.keys),sample.text,editor.document()->buffer().text().c_str(),editor.navigationPosition().x(),editor.navigationPosition().y(),qPrintable(expectedText),position.x(),position.y());}
        // Compare text registers; macro byte encodings are separately exercised by @.
        if(!sample.keys.contains('q'))for(char name:std::string("\"012-a")) {
            const auto wanted=expected["regs"].toObject()[QString(QChar(name))].toString().toStdString();
            if(reg(name)!=wanted){++mismatches;qWarning("Vim 3 register %c [%s]: got [%s], expected [%s]",name,qPrintable(sample.keys),reg(name).c_str(),wanted.c_str());}
        }
    }
    Q_ASSERT(mismatches==0);
}
