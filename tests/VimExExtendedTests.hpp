#pragma once
#include <QSignalSpy>
static void vimExExtendedTests(){
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString& text){fprintf(stderr,"%s\n",qPrintable(text));});
    VimEx::Registry registry;VimEx::Context context;context.current=2;context.last=5;context.mark=[](QChar)->std::optional<qint64>{return 4;};
    auto parsed=registry.parse("silent! keepjumps 1,2copy 'a | $",context);Q_ASSERT(parsed.error.isEmpty() && parsed.command.destination==4 && parsed.command.silent && parsed.command.ignoreErrors && parsed.command.keepJumps && parsed.rest==" $");
    parsed=registry.parse("g#a|b#s/x/y/ | d",context);Q_ASSERT(parsed.error.isEmpty() && parsed.rest.isEmpty() && parsed.command.arguments=="#a|b#s/x/y/ | d");
    Q_ASSERT(VimEx::parseGlobal("/abc").commands=="print");Q_ASSERT(!VimEx::parseGlobal("invalid").error.isEmpty());
    VimEx::LineMarks marks;for(int row:{0,2,4,6})marks.add(row);auto saved=marks;marks.edit(1,0,2,0);Q_ASSERT(marks.takeFirst()==0 && marks.takeFirst()==2 && marks.takeFirst()==4 && marks.empty());
    saved.move(2,4,0);Q_ASSERT(saved.takeFirst()==0 && saved.takeFirst()==2 && saved.takeFirst()==3 && saved.takeFirst()==6);
    auto& state=VimState::Session::instance();QSettings().setValue("editor/vimMode",true);QSettings().setValue("editor/minimap",false);QSettings().setValue("editor/autoPairs",false);
    EditorWidget editor;QSignalSpy messages(&editor,&EditorWidget::highlightingStatusChanged);
    const auto run=[&](const char* input,const QString& keys,const char* expected){state.clear();load(editor,input);vimKeys(editor,keys);if(editor.document()->buffer().text()!=expected)qFatal("Ex extended [%s]: [%s] vs [%s]",qPrintable(keys),editor.document()->buffer().text().c_str(),expected);};
    run("one\ntwo\none",":g/one/d<CR>","two");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="one\ntwo\none");
    run("one\ntwo\none",":v/one/d<CR>","one\none");
    run("one\ntwo\none",":g/one/s/one/X/g<CR>","X\ntwo\nX");
    run("ab\nac\nbc",":g/a/v/b/d<CR>","ab\nbc");
    run("a\nb\nc",":g/./normal I><CR>",">a\n>b\n>c");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="a\nb\nc");
    run("a\nb\nc",":%normal A!<CR>","a!\nb!\nc!");
    run("a\nb\nc",":g/./m0<CR>","c\nb\na");
    run("a\nb\nc",":1,2co $<CR>","a\nb\nc\na\nb");
    run("a\nb\nc",":1m $<CR>","b\nc\na");
    run("a\n  b\nc",":1,3join<CR>","a b c");
    run("a\n  b\nc",":1,3join!<CR>","a  bc");
    run("c\na\nb",":sort<CR>","a\nb\nc");
    run("b\na\nb",":sort u<CR>","a\nb");
    run("10\n2\n1",":sort n<CR>","1\n2\n10");
    run("a\nb\nc",":silent! nonsense | 1d<CR>","a\nb\nc");
    run("a\nb\nc",":silent! s/z/x/ | 1d<CR>","b\nc");
    state.clear();load(editor,"a\nb");state.searchPattern="saved";vimKeys(editor,":keeppatterns g/a/s//X/<CR>");Q_ASSERT(editor.document()->buffer().text()=="X\nb" && state.searchPattern=="saved");
    state.clear();load(editor,"a\nb\nc");state.set('a',VimState::text("1d\n$d"));vimKeys(editor,":@a<CR>");Q_ASSERT(editor.document()->buffer().text()=="b");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="a\nb\nc");
    run("a\nb",":2ka<CR>gg'a","a\nb");if(editor.navigationPosition().x()!=2)qFatal("mark cursor %d:%d, %s",editor.navigationPosition().x(),editor.navigationPosition().y(),qPrintable(state.marks("sample.txt").contains('a')?QString::number(state.marks("sample.txt")['a']->offset):QString("missing")));
    run("a\nb\nc",":2m2<CR>","a\nb\nc");
    run("a\nb\nc",":g/./2g/a/d<CR>","a\nb\nc");
    run("a\nb",":normal qa<CR>","a\nb");Q_ASSERT(state.recording.isNull());
    state.clear();load(editor,"a\nb\nc");state.set('z',VimState::text(":s/./X/\r"));vimKeys(editor,":g/./normal @z<CR>");Q_ASSERT(editor.document()->buffer().text()=="X\nX\nX");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="a\nb\nc");
    state.clear();load(editor,"a\nb\nc");state.set('z',VimState::text("1d"));vimKeys(editor,":@z<CR>:@@<CR>");Q_ASSERT(editor.document()->buffer().text()=="c");

    state.clear();std::string many;for(int i=0;i<5000;++i)many+="x\ny\n";
    editor.loadBuffer(PieceTreeBuffer(many),"sample.txt");QElapsedTimer elapsed;elapsed.start();vimKeys(editor,":g/x/d<CR>");Q_ASSERT(editor.byteSize()==10000);vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()==many);
    qInfo("Global delete/Undo: 10000 lines in %lld ms",elapsed.elapsed());
    state.clear();many.clear();for(int i=0;i<1000;++i)many+="x\n";editor.loadBuffer(PieceTreeBuffer(many),"sample.txt");elapsed.restart();vimKeys(editor,":g/x/s/x/y/<CR>");auto changed=many;std::replace(changed.begin(),changed.end(),'x','y');Q_ASSERT(editor.document()->buffer().text()==changed);vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()==many);qInfo("Global substitute/Undo: 1000 lines in %lld ms",elapsed.elapsed());

    state.clear();load(editor,"a");state.set('z',VimState::text("@z"));vimKeys(editor,":silent! @z<CR>");Q_ASSERT(!editor.vimCommandExecuting());
    state.clear();load(editor,"a");state.set('z',VimState::text("@z"));vimKeys(editor,":normal @z<CR>");Q_ASSERT(!editor.vimCommandExecuting() && !editor.isVimReplaying());
    state.clear();many.clear();for(int i=0;i<10000;++i)many+="row\n";editor.loadBuffer(PieceTreeBuffer(many),"sample.txt");QTimer::singleShot(50,&editor,[&]{QTest::keyClick(&editor,Qt::Key_Escape);});vimKeys(editor,":g/./normal OX<CR>");Q_ASSERT(!editor.vimCommandExecuting() && !editor.isVimReplaying());if(editor.document()->buffer().text()!=many){vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()==many);}
    const auto nvim=QStandardPaths::findExecutable("nvim");if(nvim.isEmpty()){qInfo("Neovim extended Ex oracle skipped");return;}
    struct Case {QString input,keys;};
    const QVector<Case> cases={
        {"ab2\nab1\nac3","/b<CR>:sort //<CR>"},{"a\nb\nc",":2ka | 2m$<CR>gg'a"},
        {"a\nb\nc",":g/b/s/./X/<CR>u"},{"a\nb\nc",":g/b/d<CR>u"},
        {"a\nb\nc",":2co0<CR>u"},{"a\nb\nc",":1m$<CR>u"},
        {"b2\na1\nc3",":sort #a|b#<CR>"},{"Б\nа\nА",":sort iu<CR>"},
        {"9223372036854775807\n9223372036854775806\n1",":sort n<CR>"},
        {QString(4000,'z')+"10\n"+QString(4000,'z')+"2",":sort n<CR>"},
        {"a\nb\nc\nd",":1d<CR>:@:<CR>:@:<CR>"},
        {"a\nb\na",":g/./s/a/X/ | s/b/z/<CR>"},
        {"one\ntwo\none",":g/one/d<CR>"},{"one\ntwo\none",":v/one/d<CR>"},{"one\ntwo\none",":g!/one/d<CR>"},
        {"one\ntwo\none",":1,2g/one/d<CR>"},{"a\nb\nc",":g/./d<CR>u"},{"a\na\nb\na",":g/a/.,+1d<CR>"},
        {"aa\nbb\naa",":g/a/s/a/X/g<CR>"},{"ab\nac\nbc",":g/a/v/b/d<CR>"},{"a\nb\nc",":g/./normal I><CR>"},
        {"a\nb\nc",":%normal A!<CR>"},{"a\nb\nc",":g/./m0<CR>"},{"a\nb\nc",":g/./t$<CR>"},
        {"a\nb\nc",":g/./join<CR>"},{"a\nb\nc",":g/./+1d<CR>"},{"a\nb\nc",":g/./d | $<CR>"},
        {"a\nb\nc",":normal jdd<CR>"},{"a\nb\nc",":2,3normal I><CR>"},{"abc", "l:normal iX<CR>"},
        {"a\nb\nc",":1,2co $<CR>"},{"a\nb\nc",":2co0<CR>"},{"a\nb\nc",":1m$<CR>"},{"a\nb\nc",":3m0<CR>"},
        {"a\nb\nc\nd",":2,3m4<CR>"},{"a\nb\nc\nd",":2,3m0<CR>"},{"a\nb\nc",":2m1<CR>"},{"a\nb\nc",":2m2<CR>"},
        {"a\nb\nc",":1,2co1<CR>"},{"a\nb\nc",":%co0<CR>"},{"a\n  b\nc",":1,3join<CR>"},{"a\n  b\nc",":1,3join!<CR>"},
        {"a\nb\nc",":join<CR>"},{"a\nb\nc",":join 3<CR>"},{"a \n b\n) c",":%join<CR>"},
        {"c\na\nb",":sort<CR>"},{"c\na\nb",":sort!<CR>"},{"b\na\nb",":sort u<CR>"},{"B\na\nb",":sort iu<CR>"},
        {"10\n2\n1",":sort n<CR>"},{"z10\ny2\nx1",":sort n<CR>"},{"b2\na1\nc3",":sort /./<CR>"},
        {"a\nb\nc",":silent! nonexistent | 1d<CR>"},{"a\nb\nc",":keepjumps 3<CR>"},
        {"one\ntwo\none",":g/one/normal x<CR>"},{"a\nb",":2ka<CR>gg'a"},{"a\nb",":2ka<CR>"},{"foo\nbar\nfoo",":g/foo/s//X/<CR>"},
        {"a\nb\nc", "qa:g/./normal A!<CR>q@a"},{"a\nb\nc",":g/./normal dd<CR>"},{"a\nb\na",":g/./s/a/X/<CR>"},{"a\nb\na",":g/./s/a/X/ | s/b/z/<CR>"},{"a\nb",":g/./normal I# <CR>"},{"a\nb",":normal A <CR>"},
        {"a\nb\nc",":g/./d a<CR>"},{"a\nb\nc",":g/./y A<CR>"},{"x\na\nx\nb",":g/x/.,+1m$<CR>"}
    };
    for(const auto& test:cases){state.clear();const auto bytes=test.input.toUtf8();editor.loadBuffer(PieceTreeBuffer(std::string_view(bytes.data(),std::size_t(bytes.size()))),"sample.txt");editor.setFocus();vimKeys(editor,test.keys);
        const auto quote=[](const QString& value){auto bytes=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);return QString::fromUtf8(bytes.mid(1,bytes.size()-2));};
        const auto script=QString("lua vim.o.startofline=true; vim.api.nvim_buf_set_lines(0,0,-1,false,vim.split(vim.fn.json_decode([==[%1]==]),'\\n',{plain=true})); local k=vim.api.nvim_replace_termcodes(vim.fn.json_decode([==[%2]==]),true,false,true); vim.api.nvim_feedkeys(k,'xt',false); local p=vim.api.nvim_win_get_cursor(0); io.stdout:write('ORACLE:'..vim.fn.json_encode({lines=vim.api.nvim_buf_get_lines(0,0,-1,false),row=p[1],column=p[2]+1,a=vim.fn.getreg('a'),one=vim.fn.getreg('1')}))").arg(quote(test.input),quote(test.keys));
        QProcess process;process.start(nvim,{"--headless","-u","NONE","-i","NONE","-n","-c",script,"-c","qa!"});Q_ASSERT(process.waitForFinished(10000));const auto output=process.readAllStandardOutput();const auto start=output.indexOf("ORACLE:");if(start<0)qFatal("Extended oracle unavailable: %s",process.readAllStandardError().constData());
        const auto result=QJsonDocument::fromJson(output.mid(start+7)).object();QStringList lines;for(const auto& line:result["lines"].toArray())lines<<line.toString();
        const auto actual=QString::fromStdString(editor.document()->buffer().text());
        if(actual!=lines.join('\n') || editor.navigationPosition()!=QPoint(result["row"].toInt(),result["column"].toInt()))qFatal("Extended oracle [%s]: [%s] %d:%d vs [%s] %d:%d",qPrintable(test.keys),qPrintable(actual),editor.navigationPosition().x(),editor.navigationPosition().y(),qPrintable(lines.join('\n')),result["row"].toInt(),result["column"].toInt());
        for(const auto reg:QStringLiteral("a1")){const auto value=state.read(reg);const auto actual=value.text?QString::fromStdString(value.text->read(0,value.text->size())):QString{};if(actual!=result[reg=='a'?"a":"one"].toString())qFatal("Extended register %c [%s]: [%s] vs [%s]",reg.toLatin1(),qPrintable(test.keys),qPrintable(actual),qPrintable(result[reg=='a'?"a":"one"].toString()));}
    }
    qInfo("Extended Ex oracle: %lld scenarios passed",qint64(cases.size()));
}
