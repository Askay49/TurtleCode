#pragma once

#include <QInputMethodQueryEvent>

static void vimViewportTests() {
    QSettings().setValue("editor/vimMode",true);
    QSettings().setValue("editor/minimap",false);
    EditorWidget editor;
    const auto caret = [&] {
        QInputMethodQueryEvent query(Qt::ImCursorRectangle);
        QApplication::sendEvent(&editor,&query);
        return query.value(Qt::ImCursorRectangle).toRect();
    };
    const auto position = [&] {return editor.navigationPosition();};
    auto* scroll = editor.verticalScrollBar();
    std::string lines;
    for (int i=0; i<100; ++i) lines+="  line"+std::to_string(i+1)+"\n";
    load(editor,lines.c_str());
    editor.resize(500,caret().height()*10+2);
    QApplication::processEvents();
    const int page = scroll->pageStep();
    Q_ASSERT(page>=8 && page<=10);
    scroll->setValue(25);
    vimKeys(editor,"H");Q_ASSERT(position()==QPoint(26,3));
    vimKeys(editor,"M");Q_ASSERT(position()==QPoint(26+(page-1)/2,3));
    vimKeys(editor,"2L");Q_ASSERT(position()==QPoint(24+page,3));
    vimKeys(editor,"zt");Q_ASSERT(caret().y()==0);
    vimKeys(editor,"zz");Q_ASSERT(caret().y()/caret().height()==(page-1)/2);
    vimKeys(editor,"zb");Q_ASSERT(caret().y()/caret().height()==page-1);
    vimKeys(editor,"50zt");Q_ASSERT(position()==QPoint(50,3) && scroll->value()==49);
    vimKeys(editor,"2<C-e>");Q_ASSERT(scroll->value()==51 && position().x()==52);
    vimKeys(editor,"<C-y>");Q_ASSERT(scroll->value()==50 && position().x()==52);
    vimKeys(editor,"gg");Q_ASSERT(position().x()==1);
    vimKeys(editor,"3<C-d><C-u>");Q_ASSERT(scroll->value()==0 && position().x()==1);
    vimKeys(editor,"<C-f><C-b>");Q_ASSERT(scroll->value()==0);
    vimKeys(editor,"Gzt");Q_ASSERT(position().x()==100 && caret().y()==0);
    vimKeys(editor,"ggdL");Q_ASSERT(editor.document()->buffer().lineCount()==101-page);
    vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()==lines);

    load(editor,"one\n  two\nhidden needle\n  four\nfive\nsix\nseven\neight");
    vimKeys(editor,"jzf2j"); // Lines 2 through 4 become one display row.
    Q_ASSERT(editor.visualRowForLine(1)==editor.visualRowForLine(3));
    Q_ASSERT(editor.documentLineForVisualRow(2)==4 && !editor.isModified());
    vimKeys(editor,"j");Q_ASSERT(position().x()==5);
    vimKeys(editor,"k");Q_ASSERT(position().x()==2);
    vimKeys(editor,"V");Q_ASSERT(editor.selectedText()=="  two\nhidden needle\n  four\n");
    vimKeys(editor,"<Esc>zo]z");Q_ASSERT(position().x()==4);
    vimKeys(editor,"[zzc");Q_ASSERT(position().x()==2 && editor.visualRowForLine(3)==1);
    vimKeys(editor,"/needle");
    QElapsedTimer preview;preview.start();
    while (editor.vimSearchRunning() && preview.elapsed()<3000) QTest::qWait(5);
    Q_ASSERT(editor.visualRowForLine(3)==3);
    vimKeys(editor,"<Esc>");Q_ASSERT(editor.visualRowForLine(3)==1);
    vimKeys(editor,"/needle<CR>");Q_ASSERT(position().x()==3 && editor.visualRowForLine(3)==3);
    vimKeys(editor,"zM");Q_ASSERT(position().x()==2 && editor.visualRowForLine(3)==1);
    vimKeys(editor,"ggOnew<Esc>");Q_ASSERT(editor.visualRowForLine(4)==2);
    vimKeys(editor,"u");Q_ASSERT(editor.visualRowForLine(3)==1);
    vimKeys(editor,"jdd");Q_ASSERT(editor.document()->buffer().text()=="one\nfive\nsix\nseven\neight");
    vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="one\n  two\nhidden needle\n  four\nfive\nsix\nseven\neight");
    vimKeys(editor,"zMjzR");Q_ASSERT(editor.visualRowForLine(3)==3);
    vimKeys(editor,"ggzjzd");Q_ASSERT(position().x()==2);
    vimKeys(editor,"zM");Q_ASSERT(editor.visualRowForLine(3)==3);

    load(editor,"a\nb\nc\nd\ne\nf\ng\nh");
    vimKeys(editor,"j2zFggV4jzf");Q_ASSERT(editor.visualRowForLine(5)==0);
    vimKeys(editor,"zo");Q_ASSERT(editor.visualRowForLine(2)==1);
    vimKeys(editor,"zO");Q_ASSERT(editor.visualRowForLine(2)==2);
    vimKeys(editor,":7,8fold<CR>");Q_ASSERT(editor.visualRowForLine(7)==6);
    vimKeys(editor,"zMzn");Q_ASSERT(editor.visualRowForLine(5)==5);
    vimKeys(editor,"zN");Q_ASSERT(editor.visualRowForLine(5)==0);
    vimKeys(editor,"zE");Q_ASSERT(editor.visualRowForLine(5)==5 && editor.visualRowForLine(7)==7);

    const std::string wide=std::string(110,'a')+"\t界é"+std::string(120,'b')+"\r\nlast";
    load(editor,wide.c_str());
    editor.setFont(QFont("DejaVu Sans Mono",12));
    editor.resize(245,340);
    QApplication::processEvents();
    vimKeys(editor,"$zs");
    Q_ASSERT(editor.horizontalScrollBar()->value()>0);
    vimKeys(editor,"g0");const auto edge=position();
    vimKeys(editor,"g$");Q_ASSERT(position().y()>=edge.y());
    vimKeys(editor,"zH");Q_ASSERT(editor.horizontalScrollBar()->value()<editor.horizontalScrollBar()->maximum());
    vimKeys(editor,"0:set wrap<CR>");
    Q_ASSERT(editor.visualRowForLine(1)>4 && editor.horizontalScrollBar()->maximum()==0);
    const auto firstRow=caret();
    vimKeys(editor,"gj");const auto second=position();const auto secondRow=caret();
    Q_ASSERT(second.x()==1 && second.y()>1 && secondRow.y()==firstRow.y()+firstRow.height());
    vimKeys(editor,"g$");Q_ASSERT(caret().y()==secondRow.y() && position().y()>second.y());
    vimKeys(editor,"g0");Q_ASSERT(position()==second);
    const auto click=caret().center();
    vimKeys(editor,"j");Q_ASSERT(position().x()==2);
    QTest::mouseClick(editor.viewport(),Qt::LeftButton,Qt::NoModifier,click);
    Q_ASSERT(position()==second);
    vimKeys(editor,"gk");Q_ASSERT(position()==QPoint(1,1));
    const int wrappedRows=editor.visualRowForLine(1);
    vimKeys(editor,"Iextra<Esc>u");Q_ASSERT(editor.visualRowForLine(1)==wrappedRows);
    vimKeys(editor,"Onew<Esc>");Q_ASSERT(editor.visualRowForLine(2)==wrappedRows+1);
    vimKeys(editor,"ugg");Q_ASSERT(editor.visualRowForLine(1)==wrappedRows);
    vimKeys(editor,"dgj");Q_ASSERT(editor.document()->buffer().text()==wide.substr(second.y()-1));
    vimKeys(editor,"u:set nowrap<CR>");Q_ASSERT(editor.document()->buffer().text()==wide && editor.visualRowForLine(1)==1);
    const auto nvim=QStandardPaths::findExecutable("nvim");
    if (nvim.isEmpty()) return;
    const QStringList cases{
        "30ztH","30zt2H","30ztM","30ztL","30zt2L","30Gzt^3l50zt",
        "30Gzt^3l2<C-e><C-y>","30Gzt^3l<C-d>","30Gzt3<C-d><C-u>",
        "30Gzt<C-f>","30Gzt<C-f><C-b>","Gzt","30Gztzz","30Gztzb",
        "30Gz.","30Gz-","ggdL","30Gzt2jV2jy",
        "jzf2jjk","jzf2jdd","jzf2jggzj","jzf2jzo]z[z",
        "jzf2jzo]z","jzf2jzR","jzf2jzE","jzf2jzMzn","jzf2jzMznzN",
        "j2zFggV4jzfzo","j2zFggV4jzfzM","j2zFggV4jzfzD",
        ":2,4fold<CR>ggzj","jzf2j/line3$<CR>"
    };
    int mismatches=0;
    for (const auto& keys:cases) {
        VimState::Session::instance().clear();
        load(editor,lines.c_str());
        editor.resize(500,caret().height()*10+2);
        QApplication::processEvents();
        const int height=scroll->pageStep();
        vimKeys(editor,keys);
        const auto quote=QJsonDocument(QJsonArray{keys}).toJson(QJsonDocument::Compact);
        const auto script=QString(R"(lua vim.o.lines=%1; vim.o.cmdheight=1; vim.o.laststatus=0; vim.o.startofline=true;
local lines={}; for i=1,100 do lines[i]='  line'..i end; vim.api.nvim_buf_set_lines(0,0,-1,false,lines);
local keys=vim.fn.json_decode([==[%2]==])[1]; vim.api.nvim_feedkeys(vim.api.nvim_replace_termcodes(keys,true,false,true),'xt',false);
local folds={}; for i=1,10 do folds[i]=vim.fn.foldclosed(i) end;
io.stdout:write('ORACLE:'..vim.fn.json_encode({top=vim.fn.line('w0'), pos=vim.api.nvim_win_get_cursor(0), lines=vim.api.nvim_buf_get_lines(0,0,-1,false), folds=folds})))")
            .arg(height+1).arg(QString::fromUtf8(quote)).replace('\n',' ');
        QProcess process;
        process.start(nvim,{"--headless","-u","NONE","-i","NONE","-n","-c",script,"-c","qa!"});
        Q_ASSERT(process.waitForFinished(10000));
        const auto output=process.readAllStandardOutput();
        const auto at=output.indexOf("ORACLE:");
        if(at<0)qFatal("Viewport oracle failed: %s",process.readAllStandardError().constData());
        const auto result=QJsonDocument::fromJson(output.mid(at+7)).object();
        const auto expected=result["pos"].toArray();
        QStringList expectedLines;
        for (const auto& value:result["lines"].toArray()) expectedLines.append(value.toString());
        const int top=editor.documentLineForVisualRow(scroll->value())+1;
        const auto actual=QString::fromStdString(editor.document()->buffer().text());
        auto wanted=expectedLines.join('\n');
        if (actual.endsWith('\n')) wanted+='\n';
        bool foldsMatch=true;
        const auto folds=result["folds"].toArray();
        for (int line=0; line<folds.size(); ++line) {
            const int row=editor.visualRowForLine(line);
            const int first=editor.documentLineForVisualRow(row);
            const int next=editor.documentLineForVisualRow(row+1);
            const int closed=(first<line || next>line+1) ? first+1 : -1;
            if (closed!=folds[line].toInt()) foldsMatch=false;
        }
        if (position()!=QPoint(expected[0].toInt(),expected[1].toInt()+1) ||
            top!=result["top"].toInt() || actual!=wanted || !foldsMatch) {
            ++mismatches;
            qWarning("Viewport [%s]: got %d:%d top %d vs %d:%d top %d, text=%d folds=%d",
                qPrintable(keys),position().x(),position().y(),top,expected[0].toInt(),
                expected[1].toInt()+1,result["top"].toInt(),actual==wanted,foldsMatch);
        }
    }
    if (mismatches) qFatal("Viewport oracle: %d mismatches",mismatches);
    qInfo("Viewport oracle: %lld scenarios passed",qint64(cases.size()));
}
