#pragma once

static void vimEditingTests() {
    using Mode=VimController::Mode;
    EditorWidget editor;
    auto run=[&](const char* text,const QString& keys,const char* expected) {
        load(editor,text);vimKeys(editor,keys);
        if(editor.document()->buffer().text()!=expected)qFatal("Vim scenario %s: got [%s], expected [%s]",qPrintable(keys),editor.document()->buffer().text().c_str(),expected);
    };
    run("one two three","2dw","three");
    run("one two three four five six seven","2d3w","seven");
    run("say \"hello\" now","f\"lci\"world<Esc>","say \"world\" now");
    vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="say \"hello\" now");
    vimKeys(editor,"<C-r>");Q_ASSERT(editor.document()->buffer().text()=="say \"world\" now");
    run("a\nb\nc","2dd","c");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="a\nb\nc");
    run("a\nb","Gdd","a");
    run("a\nb","yyjp","a\nb\na");
    run("  one\ntwo","ccnew<Esc>","  new\ntwo");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="  one\ntwo");
    run("alpha beta\n\ngamma","dap","gamma");
    run("call(a(b)c)","fbdi(","call(a()c)");
    run("one two","viwd"," two");
    run("a\nb\nc","Vjd","c");
    run("a\nb","2>>","    a\n    b");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="a\nb");
    run("    a\n    b","2<<","a\nb");
    run("one","iA<CR>B<BS>C<Esc>","A\nCone");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="one");
    run("a😀éz","l2x","az");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="a😀éz");
    run("one\r\ntwo\r\nthree","jdd","one\r\nthree");
    run("\xef\xbb\xbf" "one two","diw","\xef\xbb\xbf" " two");
    run("one two","d<Esc>l","one two");
    run("one two","diq","one two");Q_ASSERT(editor.vimMode()==Mode::Normal);
    run("one","3iX<Esc>","XXXone");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="one");
    run("one","3oX<Esc>","one\nX\nX\nX");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="one");
    run("one","2OX<Esc>","X\nX\none");
    run("one\n\ntwo","jD","one\n\ntwo");
    run("one\ntwo\n","Gdd","one\n");Q_ASSERT(editor.navigationPosition()==QPoint(1,1));
    run("one\ntwo\n","GccX<Esc>","one\nX\n");
    run("one\r\ntwo\r\n","Gdd","one\r\n");
    run("one\n","Gll","one\n");Q_ASSERT(editor.navigationPosition()==QPoint(1,3));
    run("alpha\n\nbeta","Gdipdd","alpha");
    // Modal change groups remain indivisible after the ordinary typing timeout.
    load(editor,"old");vimKeys(editor,"ciwnew");QTest::qWait(800);vimKeys(editor,"er<Esc>u");Q_ASSERT(editor.document()->buffer().text()=="old");
    // Slow object scans and destructive ranges use cancellable staged work.
    const auto large=std::string(2*1024*1024,'a')+" tail";
    editor.loadBuffer(PieceTreeBuffer(large),"large.txt");vimKeys(editor,"diw");Q_ASSERT(editor.document()->buffer().text()==" tail");
    vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()==large);
    vimKeys(editor,"diwP");Q_ASSERT(editor.document()->buffer().text()==large);
    editor.loadBuffer(PieceTreeBuffer(std::string(8*1024*1024,'a')),"cancel.txt");
    QTest::keyClicks(&editor,"diw");Q_ASSERT(editor.isBusy());
    for(auto* progress:editor.findChildren<QProgressDialog*>())progress->cancel();
    QElapsedTimer cancelled;cancelled.start();while(editor.isBusy() && cancelled.elapsed()<10000)QTest::qWait(5);
    Q_ASSERT(!editor.isBusy() && editor.byteSize()==8*1024*1024 && !editor.isModified() && !editor.document()->history().canUndo());
    load(editor,"");vimKeys(editor,"3000i"+QString(512,'x')+"<Esc>");Q_ASSERT(editor.byteSize()==3000*512);
    vimKeys(editor,"u");Q_ASSERT(editor.byteSize()==0 && !editor.isModified());

    const auto nvim=QStandardPaths::findExecutable("nvim");if(nvim.isEmpty())return;
    struct Case {const char* text;QString keys;};
    const std::vector<Case> cases={
        {"one two three", "w"},{"one two three","2w"},{"one, two... three","wwe"},
        {"one, two... three","W"},{"one two three","$b"},{"one two three","$B"},
        {"one two three","2e"},{"one two three","dw"},{"one two three","2dw"},
        {"one two three four five six seven","2d3w"},{"one two","cwX<Esc>"},
        {"one   two","daw"},{"one   two","wdiw"},{"one   two","wdiwP"},
        {"one two","ciwXYZ<Esc>u"},{"one two","ciwXYZ<Esc>u<C-r>"},
        {"one two","diw"},{"one two","x"},{"one two","2x"},{"one two","lX"},
        {"one two","sZ<Esc>"},{"one two","A!<Esc>"},{"  one two","IX<Esc>"},
        {"a\nb\nc","dd"},{"a\nb\nc","2dd"},{"a\nb\nc","Gdd"},
        {"a\nb\nc","dj"},{"a\nb\nc","jdk"},{"a\nb\nc","2G"},
        {"a\nb\nc","Ggg"},{"a\nb\nc","dG"},{"a\nb\nc","Gdgg"},
        {"one\ntwo","D"},{"one\ntwo","Cnew<Esc>"},{"  one\ntwo","ccnew<Esc>"},
        {"one\ntwo","oX<Esc>"},{"one\ntwo","OX<Esc>"},
        {"one two","ywP"},{"one two","ywp"},{"one\ntwo","yyjp"},
        {"one\ntwo","yyp"},{"one\ntwo","yyP"},{"one\ntwo","ddp"},
        {"one two three","ft"},{"one two three","ft;"},{"one two three","tt"},
        {"one two three","$Fo"},{"one two three","dft"},{"one two three","dtt"},
        {"one two three","$dFo"},{"a(b[c]d)e","%"},{"a(b[c]d)e","d%"},
        {"say \"hello\" now","f\"lci\"world<Esc>"},{"a(b[c]d)e","ldi("},
        {"a(b[c]d)e","ldab"},{"a(b[c]d)e","fci[Q<Esc>"},
        {"alpha beta\n\ngamma","dap"},{"alpha beta\n\ngamma","dip"},
        {"alpha beta\n\ngamma","}"},{"alpha beta\n\ngamma","G{"},
        {"a\nb\nc","Vjd"},{"one two","viwd"},{"one two three","veely"},
        {"a\nb","2>>"},{"    a\n    b","2<<"},{"a\nb",">j"},
        {"one","iA<CR>B<BS>C<Esc>u"},{"one two","dwue"},
        {"a😀éz","l2x"},{"мир привет","dw"},{"мир привет","e"},{"élan fin","e"},
        {"élan fin","de"},{"one","3iX<Esc>"},{"one","3aX<Esc>"},{"one","3oX<Esc>"},{"one","2OX<Esc>"},
        {"one two","wdb"},{"one two","wd0"},{"one two","d$"},{"one\n\ntwo","jD"},
        {"one   two","ld2iw"},{"one   two three","ld2aw"},{"one   two","wdaw"},
        {"one   two","3ldiw"},{"one   two","3ldaw"},{"a(b(c)d)e","fcd2i("},
        {"one two three","tt;"},{"one two three","ft;,"},{"one two three","2ft"},
        {"a\nb\nc","xx2u"},{"a\nb\nc","2ddp"},
        {"one two","llyy"},{"one two","llyb"},{"one two","llvly"},
        {"a\nb\nc","GVky"},{"one two three","wciwXYZ<Esc>u<C-r>"},
        {"a{\n  hi\n}z","f{ci{X<Esc>"},{"a(\")\")z","ldi("},
        {"a(\")\")z","ld%"},{"alpha\n\nbeta","Gdip"},{"alpha\n\nbeta","Gdap"},
        {"alpha\n\nbeta","jdip"},{"alpha\n\nbeta","jdap"},
        {"alpha\n\nbeta\n\ngamma","d2ap"},
        {"one\ntwo","dw"},{"one\ntwo","d2w"},{"one\n  two","d2w"},
        {"one\n  two","ld2w"},{"one\n  two","w"},{"one\n\ntwo","2w"},
        {"one two","vlod"},{"one\ntwo","vljD"},{"one\ntwo","vlYp"},{"one two","Vvd"},
        {"one two","xx2u2<C-r>"}
    };
    int mismatches=0;
    for(const auto& sample:cases) {
        load(editor,sample.text);vimKeys(editor,sample.keys);
        const auto encoded=QJsonDocument(QJsonArray{QString::fromUtf8(sample.text),sample.keys}).toJson(QJsonDocument::Compact).toBase64();
        const auto script=QString("lua local t=vim.fn.json_decode(vim.base64 and vim.base64.decode('%1') or vim.fn.system({'base64','-d'},'%1')); vim.o.shiftwidth=4; vim.o.tabstop=4; vim.o.expandtab=true; vim.o.autoindent=true; vim.api.nvim_buf_set_lines(0,0,-1,false,vim.split(t[1],'\\n',{plain=true})); vim.api.nvim_feedkeys(vim.api.nvim_replace_termcodes(t[2],true,false,true),'xt',false); local p=vim.api.nvim_win_get_cursor(0); io.stdout:write('ORACLE:'..vim.fn.json_encode({lines=vim.api.nvim_buf_get_lines(0,0,-1,false),row=p[1],column=p[2]}))").arg(QString::fromLatin1(encoded));
        QProcess process;process.start(nvim,{"--headless","-u","NONE","-i","NONE","-n","-c",script,"-c","qa!"});
        Q_ASSERT(process.waitForFinished(10000) && process.exitCode()==0);
        const auto output=process.readAllStandardOutput();const auto at=output.indexOf("ORACLE:");Q_ASSERT(at>=0);
        const auto expected=QJsonDocument::fromJson(output.mid(at+7)).object();QStringList lines;for(const auto& line:expected["lines"].toArray())lines<<line.toString();
        const auto expectedText=lines.join('\n');const auto row=expected["row"].toInt();
        const auto prefix=lines.value(row-1).toUtf8().left(expected["column"].toInt());
        const QPoint position(row,QString::fromUtf8(prefix).size()+1);
        if(editor.document()->buffer().text()!=expectedText.toStdString() || editor.navigationPosition()!=position) {
            ++mismatches;
            qWarning("Vim oracle [%s] on [%s]: got [%s] (%d,%d), expected [%s] (%d,%d)",qPrintable(sample.keys),sample.text,editor.document()->buffer().text().c_str(),editor.navigationPosition().x(),editor.navigationPosition().y(),qPrintable(expectedText),position.x(),position.y());
        }
    }
    Q_ASSERT(mismatches==0);
}
