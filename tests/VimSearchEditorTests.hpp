#pragma once
#include <QSignalSpy>
static void vimSearchEditorTests(){
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString& text){fprintf(stderr,"%s\n",qPrintable(text));});
    auto& state=VimState::Session::instance();QSettings().setValue("editor/vimMode",true);QSettings().setValue("editor/minimap",false);QSettings().setValue("editor/autoPairs",false);
    EditorWidget editor;QSignalSpy messages(&editor,&EditorWidget::highlightingStatusChanged);
    const auto run=[&](const char* text,const QString& keys,const char* expected){state.clear();load(editor,text);vimKeys(editor,keys);if(editor.document()->buffer().text()!=expected)qFatal("Search edit [%s]: [%s] vs [%s]",qPrintable(keys),editor.document()->buffer().text().c_str(),expected);};
    run("foo bar foo\nfoo", "/foo<CR>nN", "foo bar foo\nfoo");Q_ASSERT(editor.navigationPosition()==QPoint(1,9));
    run("one two one", "d/one<CR>", "one");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="one two one");
    run("foo foo", "\".d/", "foo foo");Q_ASSERT(!editor.vimCommandLineActive());
    run("foo foo\nfoo", ":%s/foo/bar/g<CR>", "bar bar\nbar");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="foo foo\nfoo");
    run("foo foo\nfoo", ":%s/foo/bar/gc<CR>yna", "bar foo\nbar");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="foo foo\nfoo");
    run("foo foo\nfoo", ":%s/foo/bar/gc<CR>yq", "bar foo\nfoo");
    run("foo foo\nfoo", ":%s/foo/bar/gc<CR>l", "bar foo\nfoo");
    run("foo foo\nfoo", ":%s/foo/bar/gn<CR>", "foo foo\nfoo");Q_ASSERT(!editor.isModified());
    run("one\nfoo\nend", ":/foo/,$d<CR>", "one");
    run("foo\nbar\nfoo", ":1;/bar/d<CR>", "foo");
    run("Foo foo", ":set ic<CR>:%s/foo/X/g<CR>", "X X");
    run("Foo foo", ":set ic scs<CR>:%s/Foo/X/g<CR>", "X foo");
    run("foo foo", ":set nohlsearch<CR>/foo<CR>", "foo foo");Q_ASSERT(!state.highlightSearch);
    run("foo foo\nfoo", ":set gdefault<CR>:%s/foo/X/<CR>", "X X\nX");
    run("foo foo\nfoo", ":set gdefault<CR>:%s/foo/X/g<CR>", "X foo\nX");
    run("foo foo\nfoo", ":%s/foo/X/g<CR>:%s<CR>", "X X\nX"); // No matches leaves the preceding edit intact.
    run("foo foo\nfoo", ":s/foo/X/<CR>j&", "X foo\nX");
    run("foo foo\nfoo", ":s/foo/X/g<CR>g&", "X X\nX");
    run("foo bar\nfoo", "qa/foo<CR>iX<Esc>q@a", "foo bar\nXXfoo");
    run("one\ntwo\nthree", "iX<Esc>/two<CR>.", "Xone\nXtwo\nthree");
    run("foo foo\nfoo", "qa:%s/foo/X/gc<CR>naq", "foo X\nX");
    // Incremental search cancels without changing text, cursor, Visual anchor or Undo.
    state.clear();load(editor,"foo bar foo\nfoo");vimKeys(editor,"vl/foo");
    QElapsedTimer wait;wait.start();while(editor.vimSearchRunning() && wait.elapsed()<5000)QTest::qWait(10);
    Q_ASSERT(editor.navigationPosition()==QPoint(1,9) && !editor.isModified());
    vimKeys(editor,"<Esc>");Q_ASSERT(editor.navigationPosition()==QPoint(1,2) && editor.selectedText()=="fo" && editor.vimMode()==VimController::Mode::Visual);
    vimKeys(editor,"<Esc>/foo<CR>:noh<CR>");Q_ASSERT(editor.navigationPosition()==QPoint(1,9));
    Q_ASSERT(state.searchPattern=="foo" && !state.searchHistory.isEmpty());
    // No match, invalid patterns and unsupported expressions never partially change a range.
    run("foo foo", ":%s/foo/X/g | nonsense | %d<CR>", "X X");
    run("foo foo", ":%s/\\(/X/g<CR>", "foo foo");
    run("foo foo", ":%s/foo/\\=1+1/g<CR>", "foo foo");
    run("foo foo", ":%s/absent/X/e | 1d<CR>", "");
    // Thousands of replacements use shared history payloads and retain mark positions.
    state.clear();std::string text;for(int n=0;n<3000;++n)text+="foo keep\n";text+="tail";
    editor.loadBuffer(PieceTreeBuffer(text),"large.txt");editor.setNavigationPosition(1500,5);vimKeys(editor,"ma:%s/foo/longer/g<CR>`a");
    Q_ASSERT(editor.navigationPosition()==QPoint(1500,8));vimKeys(editor,"u`a");Q_ASSERT(editor.document()->buffer().text()==text && editor.navigationPosition()==QPoint(1500,5));
    vimKeys(editor,"<C-r>`a");Q_ASSERT(editor.navigationPosition()==QPoint(1500,8));
    const auto descriptors=QDir("/proc/self/fd").entryList(QDir::AllEntries|QDir::NoDotAndDotDot).size();
    std::string many;for(int n=0;n<20000;++n)many+="foo ";editor.loadBuffer(PieceTreeBuffer(many),"many.txt");
    vimKeys(editor,":%s/foo/longer/g<CR>");Q_ASSERT(editor.byteSize()==140000 && QDir("/proc/self/fd").entryList(QDir::AllEntries|QDir::NoDotAndDotDot).size()<descriptors+16);
    vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()==many);
    // Long lines, CRLF/BOM, Unicode replacement case conversion, and a newline substitution.
    editor.loadBuffer(PieceTreeBuffer(std::string(2*1024*1024,'a')+"FOO"),"large.txt");vimKeys(editor,":%s/FOO/end/g<CR>");Q_ASSERT(editor.document()->buffer().slice(editor.byteSize()-3,3)=="end");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().slice(editor.byteSize()-3,3)=="FOO");
    vimKeys(editor,":s/.*/X/c<CR>y");Q_ASSERT(editor.document()->buffer().text()=="X");vimKeys(editor,"u");Q_ASSERT(editor.byteSize()==2*1024*1024+3);
    editor.loadBuffer(PieceTreeBuffer("\xef\xbb\xbf" "foo\r\nfoo\r\n"),"crlf.txt");vimKeys(editor,":%s/foo/X/g<CR>");Q_ASSERT(editor.document()->buffer().text()=="\xef\xbb\xbf" "X\r\nX\r\n");
    run("foo", ":s/foo/a\\rb/<CR>", "a\nb");
    run("Привет привет", ":%s/привет/\\U&/gi<CR>", "ПРИВЕТ ПРИВЕТ");

    const auto nvim=QStandardPaths::findExecutable("nvim");if(nvim.isEmpty()){qInfo("Neovim search editor oracle skipped");return;}
    struct Case {QString text,keys;};
    const QVector<Case> cases={
        {"foo foo\nfoo","/foo<CR>"},{"foo foo\nfoo","/foo<CR>n"},{"foo foo\nfoo","?foo<CR>"},{"foo foo\nfoo","/foo<CR>N"},
        {"foo foo\nfoo","2/foo<CR>"},{"foo foo\nfoo","/foo<CR>2n"},{"foo food foo","*"},{"foo food foo","g*"},{"foo foo\nfoo","l#"},{"foo!bar bar","3l*"},
        {"foo food foo","/#<Esc>"},{"foo foo\nfoo","/foo/e<CR>n"},{"foo foo\nfoo","/foo/+1<CR>"},
        {"foo foo\nfoo","/foo/s+1<CR>n"},{"foo foo\nfoo","/foo/e-1<CR>n"},{"foo\nfoo\nfoo","/foo/+1<CR>n"},
        {"foo foo\nfoo","/$<CR>n"},{"foo foo\nfoo","/^<CR>N"},{"ababa aba","/aba<CR>"},
        {"one two one","d/one<CR>"},{"one two one","c/one<CR>X<Esc>"},{"one\ntwo\nthree","d/two<CR>"},
        {"x one\ntwo\nthree","ld/two<CR>"},{"foo foo\nfoo","v/foo<CR>d"},{"foo foo\nfoo","d/foo/e<CR>"},
        {"foo foo\nfoo",":%s/foo/X/g<CR>"},{"foo foo\nfoo",":%s/foo/X/<CR>"},{"foo foo\nfoo",":%s/foo/X/gc<CR>yna"},
        {"foo foo\nfoo",":%s/foo/X/gc<CR>nnq"},{"foo foo\nfoo",":%s/foo/X/gc<CR>l"},{"foo foo\nfoo",":%s/foo/X/gc<CR>yq"},
        {"foo foo\nfoo",":%s/foo/X/gn<CR>"},{"foo foo\nfoo",":2s/foo/X/<CR>"},{"foo foo\nfoo",":s/foo/X/<CR>j&"},
        {"foo foo\nfoo",":s/foo/X/g<CR>g&"},{"one\nfoo\nend",":/foo/,$d<CR>"},{"one\nfoo\nend",":/foo/<CR>"},
        {"foo\nbar\nfoo",":1;/bar/d<CR>"},{"foo foo\nfoo",":%s/foo/X/g | $<CR>"},
        {"foo bar\nfoo","d/foo<CR>j."},{"foo\nfoo",":%s/foo/X/g<CR>u<C-r>"},{"foo foo\nfoo",":%s/foo/X/gc<CR>ya"},
        {"foo foo\nfoo",":%s#foo#x|y#g | $<CR>"},{"Foo foo",":set ic<CR>/foo<CR>"},
        {"foo bar\nfoo bar",":s/foo/X/<CR>:%s/bar/~/g<CR>"},{"foo foo\nfoo",":s/foo/X/g<CR>:&&<CR>"},
        {"foo foo\nfoo",":%s/foo/X/gcn<CR>"},{"foo foo\nfoo",":set gd<CR>:%s/foo/X/g<CR>"},
        {"foo foo\nfoo",":?foo?<CR>n"},{"foo foo\nfoo",":/foo/<CR>n"},{"one\nfoo\nend",":/foo<CR>"},
        {"foo",":s/foo/a\\rb/<CR>"},{"foo",":s/foo/a\\r/<CR>"},{"foo",":s/foo/a\\rb/c<CR>l"},
        {"one foo foo",":s/foo/X/gc<CR>l"},{"one foo foo",":s/foo/X/gc<CR>yq"},{"one foo foo",":s/foo/X/gc<CR>yy"},
        {"one foo foo",":s/foo/X/gc<CR>nn"}
    };
    for(const auto& test:cases){state.clear();const auto bytes=test.text.toUtf8();editor.loadBuffer(PieceTreeBuffer(std::string_view(bytes.data(),std::size_t(bytes.size()))),"sample.txt");editor.setFocus();vimKeys(editor,test.keys);
        const auto quoted=[](const QString& text){const auto bytes=QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);return QString::fromUtf8(bytes.mid(1,bytes.size()-2));};
        const auto script=QString("lua vim.o.startofline=true; vim.o.hlsearch=true; vim.api.nvim_buf_set_lines(0,0,-1,false,vim.split(vim.fn.json_decode([==[%1]==]),'\\n',{plain=true})); local k=vim.api.nvim_replace_termcodes(vim.fn.json_decode([==[%2]==]),true,false,true); vim.api.nvim_feedkeys(k,'xt',false); local p=vim.api.nvim_win_get_cursor(0); io.stdout:write('ORACLE:'..vim.fn.json_encode({lines=vim.api.nvim_buf_get_lines(0,0,-1,false),row=p[1],column=p[2]+1}))").arg(quoted(test.text),quoted(test.keys));
        QProcess process;process.start(nvim,{"--headless","-u","NONE","-i","NONE","-n","-c",script,"-c","qa!"});Q_ASSERT(process.waitForFinished(10000));
        const auto output=process.readAllStandardOutput();const auto start=output.indexOf("ORACLE:");if(start<0)qFatal("Search oracle unavailable: %s",process.readAllStandardError().constData());
        const auto result=QJsonDocument::fromJson(output.mid(start+7)).object();QStringList lines;for(const auto& line:result["lines"].toArray())lines<<line.toString();
        const auto actual=QString::fromStdString(editor.document()->buffer().text());
        const auto row=std::size_t(editor.navigationPosition().x()-1),at=editor.document()->buffer().lineStart(row);const auto column=int(editor.document()->buffer().byteAtUtf16(editor.document()->buffer().utf16Before(at)+editor.navigationPosition().y()-1)-at+1);
        if(actual!=lines.join('\n') || editor.navigationPosition().x()!=result["row"].toInt() || column!=result["column"].toInt())qFatal("Search oracle [%s]: got [%s] %d:%d vs [%s] %d:%d",qPrintable(test.keys),qPrintable(actual),editor.navigationPosition().x(),column,qPrintable(lines.join('\n')),result["row"].toInt(),result["column"].toInt());
    }
    qInfo("Vim search editor oracle: %lld scenarios passed",qint64(cases.size()));
}
