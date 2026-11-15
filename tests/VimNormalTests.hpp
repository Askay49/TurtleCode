#pragma once
static void vimNormalTests(){
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString& text){fprintf(stderr,"%s\n",qPrintable(text));});
    QSettings().setValue("editor/vimMode",true);QSettings().setValue("editor/minimap",false);QSettings().setValue("editor/autoPairs",false);
    EditorWidget editor;auto& state=VimState::Session::instance();const auto nvim=QStandardPaths::findExecutable("nvim");
    const auto run=[&](const char* input,const QString& keys,const char* expected){state.clear();load(editor,input);vimKeys(editor,keys);if(editor.document()->buffer().text()!=expected)qFatal("Normal [%s]: [%s] vs [%s]",qPrintable(keys),editor.document()->buffer().text().c_str(),expected);};
    run("a\n  b\nc","J","a b\nc");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="a\n  b\nc");
    run("aBc def","gUiw","ABC def");run("x 009","2<C-a>","x 011");run("one two three four",":set tw=9<CR>gqq","one two\nthree\nfour");
    state.clear();std::string large;for(int n=0;n<2048;++n)large+=std::string(1023,'a')+"\n";editor.loadBuffer(PieceTreeBuffer(large),"sample.cpp");editor.setFocus();QElapsedTimer elapsed;elapsed.start();vimKeys(editor,"gUG");auto upper=large;std::replace(upper.begin(),upper.end(),'a','A');Q_ASSERT(editor.document()->buffer().text()==upper);vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()==large);qInfo("Normal case/Undo: 2 MiB in %lld ms",elapsed.elapsed());
    vimKeys(editor,"VG");QTest::keyClicks(&editor,"U");QTimer::singleShot(10,&editor,[&]{QTest::keyClick(&editor,Qt::Key_Escape);});QElapsedTimer cancel;cancel.start();while((editor.isBusy() || editor.isVimReplaying()) && cancel.elapsed()<10000)QTest::qWait(5);Q_ASSERT(!editor.isBusy() && !editor.isVimReplaying() && editor.document()->buffer().text()==large);
    if(nvim.isEmpty()){qInfo("Neovim Normal oracle skipped");return;}
    struct Case{QString input,keys;};
    const QVector<Case> cases={
        {"foo\n\nbar","Gge"},{"foo\n\n\nbar","G2ge"},{"foo\n\nbar","GgE"},
        {QString::fromUtf8("界界 界界 界界"),":set tw=9<CR>gqq"},
        {"\tabc","5|"},{QString::fromUtf8("界abc"),"3|"},
        {QString::fromUtf8("é abc"),"wdge"},{QString::fromUtf8("é abc"),"wge"},
        {"one\ntwo\nthree","iX<Esc>jA!<Esc>ggg,"},
        {"int f() {\nif (a) {\nx();\n} else {\ny();\n}\n}","gg=G"},
        {"int f() {\nlabel:\nx();\n}","gg=G"},{"class A {\npublic:\nint f();\n};","gg=G"},
        {"switch (x) {\ncase 1:\nif (a) {\nx();\n}\ny();\nbreak;\n}","gg=G"},
        {"one two\n\n\nthree four\n\n",":set tw=12<CR>gqG"},{"// one two\n// three four\n// five six",":set tw=15<CR>gqG"},
        {"abc def","gUwggg;"},{"one\n  two\nthree","jllmaggJugg`a"},
        {"one two three four five six","2d2ww2."},{"  abc",":set tw=20<CR>gqq"},
        {"one two three four five six seven eight","2gU3w"},{"one two three four five six seven eight","gU2ww2."},
        {"one two three four","qa~wq2@a"},{"aBc\nDEF","g~g~j."},{"abc\ndef\nghi","VjUj."},
        {"   abc","VU"},{" a1\n b2","<C-v>jjg<C-a>"},{"12345","lvll<C-a>"},{"12345","llvl<C-x>"},
        {"0X10","<C-x>"},{"0b111","ll<C-a>"},{"0x1234","llvl<C-a>"},{"0",":set nrformats=unsigned<CR><C-x>"},
        {"18446744073709551615",":set nrformats=unsigned<CR><C-a>"},{"-18446744073709551615","<C-x>"},
        {"a 1\nb 1\nc 1","<C-v>jjllg<C-a>"},{"one two three four five six",":set tw=12<CR>gqgq"},
        {"one two three\nfour five six",":set tw=12<CR>Vjgq"},{"one.\ntwo three",":set tw=79 joinspaces<CR>gqj"},
        {QString::fromUtf8("слово один два три четыре пять"),":set tw=15<CR>gqq"},
        {QString::fromUtf8("İı éé ßẞ"),"gU$"},{QString::fromUtf8("İı ÉÉ ßẞ"),"gu$"},
        {"int f() {\ncall();\nreturn 0;\n}\nnext();","gg=G"},{"#define A 1\nint f() {\n x();\n}","gg=G"},
        {"{\n x;\n y;\n}","j=jj."},{"one\n  two\nthree","jllmaggJ`a"},
        {"abc def","llmagUiw`a"},{"abc def","gUwgg`]"},{"123abc def","gUwgg`["},
        {"abc def","iXYZ<Esc>ggg;"},{"abc def\nxyz","gUwug;"},{"one\ntwo\nthree","jmaGg'a''"},
        {"One. Two.\n\nThree.","$`)"},{"One. Two.\n\nThree.","G`("},{"one\n\ntwo","G'{"},
        {"abc\ndef","iX<Esc>jA!<Esc>:'[,']y a<CR>\"ap"},
        {"abc\ndef","gU/def<CR>"},{"one. Two. Three.","2)c(X<Esc>"},
        {"top\n.SH head\ntext\n.NH next\nend","2]]"},{"{a (x) b}","f(])"},
        {"one\n  two\nthree","J"},{"one\n  two\nthree","3J"},{"one\n  two\nthree","gJ"},{"one\n  two\nthree","3gJ"},
        {"one \n two\n) x","3J"},{"one\n\nthree","3J"},{"one\ntwo\nthree","VjJ"},{"one\ntwo\nthree","VjgJ"},
        {"one\ntwo\nthree\nfour","Jj."},{"one\ntwo\nthree","Ju<C-r>"},{"end.\nnext","J"},{"end.\nnext",":set joinspaces<CR>J"},
        {"aBc DEF","~"},{"aBc DEF","3~"},{"aBc DEF","$3~"},{"12 ABC","3~"},{"aBc DEF","gUw"},{"aBc DEF","guw"},
        {"aBc DEF","g~w"},{"aBc DEF","gUiw"},{"aBc DEF","guu"},{"aBc DEF","gUU"},{"aBc DEF","gUgU"},
        {"aBc\nDEF\nghi","2gUU"},{"aBc\nDEF\nghi","gUG"},{"aBc DEF","viwU"},{"aBc DEF","vll~"},
        {"aBc DEF","Vlu"},{"aBc DEF","gUww."},{"aBc DEF","gUiwugUiw"},{"aBc\nDEF","<C-v>jlU"},
        {QString::fromUtf8("ßé ПрИвЕт"),"gU$"},{QString::fromUtf8("ßé ПрИвЕт"),"4~"},{QString::fromUtf8("ЁЖ\nАБВ"),"gug" "g"},
        {"one two three","wge"},{"one two three","2w2ge"},{"one, two.three","$3ge"},{"one, two.three","$2gE"},
        {"one two three","wdge"},{"one two three","$cgeX<Esc>"},{"one two","dge"},
        {"a\n  bb\n ccc","+"},{"a\n  bb\n ccc","2+"},{"a\n  bb\n ccc","G-"},{"a\n  bb\n ccc","2_"},
        {"a\n  bb\n ccc","d+"},{"a\n  bb\n ccc","Gd-"},{"abcdef","4|"},{"a  b   ","g_"},{"a\nb\nc\nd","50%"},
        {"One. Two! Three? Four.",")"},{"One. Two! Three? Four.","2)"},{"One. Two! Three? Four.","$2("},
        {"One.) Two?\nThree!\n\nFour.","3)"},{"One. Two! Three? Four.","d)"},{"One. Two! Three? Four.","2)d("},
        {"One\n\n\nTwo.",")"},{"One\n\n\nTwo.","2)"},{"  One.\nTwo.","$("},
        {"top\n{\n  body\n}\nnext\n{\n  b\n}","]]"},{"top\n{\n  body\n}\nnext\n{\n  b\n}","]["},
        {"top\n{\n  body\n}\nnext\n{\n  b\n}","G[["},{"top\n{\n  body\n}\nnext\n{\n  b\n}","G[]"},
        {"top\n{\n  body\n}\nnext\n{\n  b\n}","d]]"},{"(a (b) c)","$[("},{"{a {b} c}","]}"},
        {"1","<C-a>"},{"0","<C-x>"},{"-1","<C-a>"},{"-1","2<C-a>"},{"-1","2<C-x>"},
        {"0010","<C-a>"},{"abc 009","<C-a>"},{"a123 b","ll<C-a>"},{"one 4 two 5","w<C-a>"},
        {"0xff","<C-a>"},{"0xFF","<C-a>"},{"0X0F","<C-x>"},{"0b011","<C-a>"},{"0b000","<C-x>"},
        {"-0x10","<C-a>"},{"-10","l<C-x>"},{"0xabc","lll<C-a>"},{"18446744073709551615","<C-a>"},
        {"18446744073709551616","<C-x>"},{"0077",":set nrformats=octal<CR><C-a>"},{"0089",":set nrformats=octal<CR><C-a>"},
        {"z",":set nrformats=alpha<CR><C-a>"},{"A",":set nrformats=alpha<CR><C-x>"},{"-1",":set nrformats=unsigned<CR><C-a>"},
        {"1\n2\n3","Vjj<C-a>"},{"1\n2\n3","Vjjg<C-a>"},{"1\n\n3","Vjjg<C-x>"},{"-1\n-2","lvj<C-a>"},
        {"one 1 two 2","3<C-a>w."},{"a1\nb2","qa<C-a>qj@a"},{"1","<C-a>u<C-r>"},
        {"int f() {\nx();\nif (x) {\ny();\n}\n}","gg=G"},{"{\n a;\n}\n", "gg=G"},
        {"if (x) {\n  a();\n  b();\n}","j=="},{"if (x) {\na();\n}\n", "gg=G"},
        {"if (x)\ny();\nz();", "gg=G"},{"if (x)\n{\ny();\n}\nz();", "gg=G"},
        {"switch (x) {\ncase 1:\nx();\nbreak;\ndefault:\ny();\n}","gg=G"},
        {"foo(a,\nb,\nc);", "gg=G"},{"/* text\n* one\n*/\nint a;", "gg=G"},
        {"int f() {\n  const char* x = \"}\";\nreturn 1;\n}","gg=G"},
        {"one two three four five six seven eight nine ten",":set tw=20<CR>gqq"},
        {"one two\nthree four\nfive six",":set tw=20<CR>gqap"},
        {"one two\n\nthree four",":set tw=20<CR>gqG"},
        {"  one two three four five six seven eight",":set tw=20<CR>gqq"},
        {"// one two three four five six seven eight nine",":set tw=20<CR>gqq"},
        {"one  two three\nfour five six",":set tw=15<CR>gqj"},
        {"one two three four five six seven eight nine ten",":set tw=20<CR>gqqu<C-r>"},
        {"one\ntwo\nthree","iX<Esc>jA!<Esc>ggg;"},{"one\ntwo\nthree","iX<Esc>jA!<Esc>gg2g;g,"},
        {"abc","iXYZ<Esc>gg`."},{"abc","iXYZ<Esc>gg`["},{"abc","iXYZ<Esc>gg`]"},{"abc","iXYZ<Esc>gg`^"},
        {"abc\ndef","gUwgg`["},{"abc\ndef","gUwgg`]"}
    };
    int mismatches=0;
    for(const auto& test:cases){state.clear();const auto bytes=test.input.toUtf8();editor.loadBuffer(PieceTreeBuffer(std::string_view(bytes.data(),std::size_t(bytes.size()))),"sample.cpp");editor.resize(800,400);editor.show();editor.activateWindow();editor.setFocus();QApplication::processEvents();vimKeys(editor,test.keys);
        const auto quote=[](const QString& value){auto data=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);return QString::fromUtf8(data.mid(1,data.size()-2));};
        const auto script=QString("lua vim.o.startofline=true; vim.o.shiftwidth=4; vim.o.tabstop=4; vim.o.expandtab=true; vim.o.autoindent=true; vim.o.joinspaces=false; vim.o.textwidth=79; local s=vim.fn.json_decode([==[%1]==]); local l=vim.split(s,'\\n',{plain=true}); if s:sub(-1)=='\\n' then table.remove(l) end; vim.api.nvim_buf_set_lines(0,0,-1,false,l); vim.api.nvim_feedkeys(vim.api.nvim_replace_termcodes(vim.fn.json_decode([==[%2]==]),true,false,true),'xt',false); local p=vim.api.nvim_win_get_cursor(0); io.stdout:write('ORACLE:'..vim.fn.json_encode({lines=vim.api.nvim_buf_get_lines(0,0,-1,false),row=p[1],column=p[2]}))").arg(quote(test.input),quote(test.keys));
        QProcess process;process.start(nvim,{"--headless","-u","NONE","-i","NONE","-n","-c",script,"-c","qa!"});Q_ASSERT(process.waitForFinished(10000));auto output=process.readAllStandardOutput();const auto at=output.indexOf("ORACLE:");if(at<0)qFatal("Normal oracle: %s",process.readAllStandardError().constData());
        const auto result=QJsonDocument::fromJson(output.mid(at+7)).object();QStringList lines;for(const auto& line:result["lines"].toArray())lines<<line.toString();const auto prefix=lines.value(result["row"].toInt()-1).toUtf8().left(result["column"].toInt());const QPoint cursor(result["row"].toInt(),QString::fromUtf8(prefix).size()+1);
        auto actual=QString::fromStdString(editor.document()->buffer().text());if(test.input.endsWith('\n') && actual.endsWith('\n'))actual.chop(1);if(actual!=lines.join('\n') || editor.navigationPosition()!=cursor){++mismatches;qWarning("Normal oracle [%s] on [%s]: [%s] %d:%d vs [%s] %d:%d",qPrintable(test.keys),qPrintable(test.input),qPrintable(actual),editor.navigationPosition().x(),editor.navigationPosition().y(),qPrintable(lines.join('\n')),cursor.x(),cursor.y());}
    }
    if(mismatches)qFatal("Normal oracle: %d/%lld failures",mismatches,qint64(cases.size()));qInfo("Normal oracle: %lld scenarios passed",qint64(cases.size()));
}
