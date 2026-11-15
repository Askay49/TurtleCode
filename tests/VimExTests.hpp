#pragma once
#include "editor/VimEx.hpp"
#include <QSignalSpy>
#include <QFile>

static void vimExTests(){
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString& message){fprintf(stderr,"%s\n",qPrintable(message));});
    using namespace VimEx;
    Registry registry;Context context;context.current=4;context.last=10;
    context.mark=[](QChar name)->std::optional<qint64>{if(name=='a' || name=='<')return 2;if(name=='b' || name=='>')return 7;return {};};
    struct RangeCase {const char* input;const char* command;qint64 first,last;};
    const RangeCase ranges[]={
        {"w","write",1,10},{".","",4,4},{"$","",10,10},{"0","",0,0},{"0p","print",1,1},
        {"%p","print",1,10},{"2,5p","print",2,5},{".,.+3nu","number",4,7},
        {"'a,'bp","print",2,7},{"*p","print",2,7},{"+2p","print",6,6},
        {"-p","print",3,3},{"2;+2p","print",2,4},{"2,+2p","print",2,6},
        {",p","print",4,4},{"1,2,3p","print",2,3},{"2d a 3","delete",2,4},
        {":: 2 +3 -1 p","print",4,4},{"2 + 1 p","print",4,4},{"up","update",1,10}
    };
    for(const auto& test:ranges){const auto parsed=registry.parse(test.input,context);
        if(!parsed.error.isEmpty() || parsed.command.definition.name!=test.command || parsed.command.first!=test.first || parsed.command.last!=test.last)
            qFatal("Ex parse [%s]: %s %s %lld,%lld",test.input,qPrintable(parsed.error),qPrintable(parsed.command.definition.name),parsed.command.first,parsed.command.last);
    }
    for(const auto& text:QStringList{"%quit","undo!","write! x y","11p","5,2d","-99p","999999999999999999999999p","'zp","noSuchCommand","p junk","d a junk","/foo/p"}){
        auto parsed=registry.parse(text,context);
        if(text=="write! x y"){QString error;fileArgument(parsed.command.arguments,{},&error);Q_ASSERT(!error.isEmpty());}
        else Q_ASSERT(!parsed.error.isEmpty());
    }
    Q_ASSERT(registry.parse("2d | $p",context).rest==" $p");
    Q_ASSERT(registry.parse("w target\\|name | q",context).command.arguments=="target\\|name");
    Q_ASSERT(registry.parse("p \" comment | d",context).rest.isEmpty());
    Q_ASSERT(registry.parse("pu 0",context).command.registerName=='0');
    Q_ASSERT(registry.parse("\" comment",context).empty);
    registry.add({"custom",3,true,true,false,Arguments::Raw,DefaultRange::Current,false,false});
    auto custom=registry.parse("2,3cus! x|y\"z",context);
    Q_ASSERT(custom.error.isEmpty() && custom.command.arguments=="x|y\"z" && custom.command.bang && custom.rest.isEmpty());
    context.search=[](const QString& p,bool forward,qint64)->std::optional<qint64>{return p=="foo" && forward?std::optional<qint64>(8):std::nullopt;};
    Q_ASSERT(registry.parse("/foo/-1p",context).command.last==7);
    QString error;Q_ASSERT(fileArgument("a\\ b\\|c",{},&error).endsWith("a b|c") && error.isEmpty());
    error.clear();Q_ASSERT(fileArgument("%",{},&error).isEmpty() && !error.isEmpty());
    for(const auto& unsupported:QStringList{"!cat",">>target","++enc=utf8 target","%:s/foo/bar/"}){error.clear();fileArgument(unsupported,"/tmp/current.txt",&error);Q_ASSERT(!error.isEmpty());}
    error.clear();Q_ASSERT(fileArgument("%:h/copy.txt","/tmp/current.txt",&error)=="/tmp/copy.txt" && error.isEmpty());

    auto& state=VimState::Session::instance();
    QSettings().setValue("editor/vimMode",true);QSettings().setValue("editor/minimap",false);QSettings().setValue("editor/autoPairs",false);
    EditorWidget editor;load(editor,"one\ntwo\nthree\nfour\nfive");
    QSignalSpy status(&editor,&EditorWidget::highlightingStatusChanged);
    vimKeys(editor,":2<CR>");Q_ASSERT(editor.navigationPosition()==QPoint(2,1));
    vimKeys(editor,":2,3d<CR>");Q_ASSERT(editor.document()->buffer().text()=="one\nfour\nfive");
    vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="one\ntwo\nthree\nfour\nfive");
    vimKeys(editor,":2,3y a<CR>:$pu a<CR>");
    Q_ASSERT(editor.document()->buffer().text()=="one\ntwo\nthree\nfour\nfive\ntwo\nthree");
    vimKeys(editor,":1d | $<CR>");Q_ASSERT(editor.navigationPosition().x()==6);
    const auto before=editor.document()->buffer().text();
    vimKeys(editor,":nonsense | 1d<CR>");Q_ASSERT(editor.document()->buffer().text()==before && status.last()[0].toString().startsWith("E492"));
    vimKeys(editor,":w<CR>");Q_ASSERT(!editor.vimCommandExecuting());
    // Cancel and text editing stay within command line, including IME.
    vimKeys(editor,":p");auto* input=editor.findChild<QLineEdit*>("vimCommandInput");Q_ASSERT(input && input->hasFocus());
    QTest::keyClick(input,Qt::Key_U,Qt::ControlModifier);Q_ASSERT(input->text().isEmpty());
    QInputMethodEvent ime;ime.setCommitString(QString::fromUtf8("Привет"));QApplication::sendEvent(input,&ime);Q_ASSERT(input->text()==QString::fromUtf8("Привет"));
    QTest::keyClick(input,Qt::Key_Escape);Q_ASSERT(editor.hasFocus() && !editor.vimCommandLineActive() && editor.document()->buffer().text()==before);
    vimKeys(editor,":");QTest::keyClick(input,Qt::Key_Backspace);Q_ASSERT(!editor.vimCommandLineActive());
    vimKeys(editor,":wr<Tab>");if(input->text()!="write")qFatal("Ex completion: [%s]",qPrintable(input->text()));QTest::keyClick(input,Qt::Key_Escape);
    vimKeys(editor,":wr");QTest::keyClick(input,Qt::Key_Tab);Q_ASSERT(input->text()=="write");QTest::keyClick(input,Qt::Key_Escape);
    Q_ASSERT(QDir().mkdir("ex files"));{QFile file("ex files/example| file.txt");Q_ASSERT(file.open(QIODevice::WriteOnly));}
    vimKeys(editor,":w ex\\ files/ex<Tab>");Q_ASSERT(input->text()=="w ex\\ files/example\\|\\ file.txt");QTest::keyClick(input,Qt::Key_Escape);
    vimKeys(editor,":");QTest::keyClick(input,Qt::Key_R,Qt::ControlModifier);QTest::keyClicks(input,"%");Q_ASSERT(input->text()=="sample.txt");QTest::keyClick(input,Qt::Key_Escape);
    vimKeys(editor,":");QTest::keyClick(input,Qt::Key_Up);Q_ASSERT(input->text()=="w");QTest::keyClick(input,Qt::Key_Down);Q_ASSERT(input->text().isEmpty());QTest::keyClick(input,Qt::Key_Escape);
    vimKeys(editor,"ggVj:");Q_ASSERT(input->text()=="'<,'>");vimKeys(editor,"d<CR>");Q_ASSERT(editor.document()->buffer().text()!=before);
    load(editor,"one\ntwo\nthree");vimKeys(editor,"qa:1d<CR>q@a");Q_ASSERT(editor.document()->buffer().text()=="three");
    vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()=="two\nthree");
    load(editor,"one\ntwo");vimKeys(editor,"iX<Esc>:2<CR>.");Q_ASSERT(editor.document()->buffer().text()=="Xone\nXtwo");
    load(editor,"a\nb\nc\nd");vimKeys(editor,":1d | nonsense | 1d<CR>");Q_ASSERT(editor.document()->buffer().text()=="b\nc\nd");
    // Shared history and ':' register are session state.
    Q_ASSERT(state.lastCommand=="1d | nonsense | 1d");
    const auto lastCommand=state.read(':');Q_ASSERT(lastCommand.text && lastCommand.text->read(0,lastCommand.text->size())=="1d | nonsense | 1d");
    load(editor,"a\nb\nc\nd");vimKeys(editor,":1d<CR>@:");Q_ASSERT(editor.document()->buffer().text()=="c\nd");
    // Large range edits keep the asynchronous path and a single Undo operation.
    const std::string huge=std::string(2*1024*1024,'a')+"\nend";editor.loadBuffer(PieceTreeBuffer(huge),"large.txt");editor.setFocus();
    vimKeys(editor,":1d | $<CR>");Q_ASSERT(editor.document()->buffer().text()=="end");vimKeys(editor,"u");Q_ASSERT(editor.document()->buffer().text()==huge);

    const auto nvim=QStandardPaths::findExecutable("nvim");
    if(nvim.isEmpty()){qInfo("Neovim Ex oracle skipped: nvim is not installed");return;}
    const QStringList sequences={
        ":2<CR>",":$<CR>",":0<CR>",":+2<CR>","jj:.-1<CR>",":%p<CR>",":2,3p<CR>",":2;+2p<CR>",":2,+2p<CR>",
        ":1d<CR>",":2,3d<CR>",":%d<CR>",":$d<CR>",":0d<CR>",":0p<CR>",":2d 2<CR>",":2d a 2<CR>",":2,3y a<CR>:$pu a<CR>",
        ":1y<CR>:0pu<CR>",":1,2y<CR>:$pu<CR>","yiw:pu<CR>",":1,2d<CR>:undo<CR>",":d | $<CR>",
        ":1d | 1d<CR>",":1d | 1d<CR>u",":2,3d | .p<CR>",":2y a | $pu a<CR>",":1,2,3p<CR>",":y<CR>:pu 0<CR>",
        "jmajjmb:'a,'bd<CR>","Vj:d<CR>","2:d<CR>",":2,3number<CR>",":list<CR>",":=<CR>",
        "qa:1d<CR>q@a",": 2 + 1 p<CR>",":1d \"comment | 1d<CR>",":1d | nonexistent | 1d<CR>",":1d<CR>@:",":d | undo | redo<CR>"
    };
    for(const auto& keys:sequences){
        state.clear();load(editor,"one\n  two\nthree\nfour\nfive");vimKeys(editor,keys);
        const auto keyJson=QJsonDocument(QJsonArray{keys}).toJson(QJsonDocument::Compact);const auto quoted=QString::fromUtf8(keyJson.mid(1,keyJson.size()-2));
        const auto script=QString("lua vim.o.startofline=true; vim.api.nvim_buf_set_lines(0,0,-1,false,{'one','  two','three','four','five'}); local k=vim.api.nvim_replace_termcodes(vim.fn.json_decode([==[%1]==]),true,false,true); vim.api.nvim_feedkeys(k,'xt',false); local p=vim.api.nvim_win_get_cursor(0); io.stdout:write('ORACLE:'..vim.fn.json_encode({lines=vim.api.nvim_buf_get_lines(0,0,-1,false),row=p[1],column=p[2]+1}))").arg(quoted);
        QProcess process;process.start(nvim,{"--headless","-u","NONE","-i","NONE","-n","-c",script,"-c","qa!"});
        Q_ASSERT(process.waitForFinished(10000) && process.exitCode()==0);
        const auto output=process.readAllStandardOutput();const auto index=output.indexOf("ORACLE:");Q_ASSERT(index>=0);
        const auto result=QJsonDocument::fromJson(output.mid(index+7)).object();QStringList lines;
        for(const auto& line:result["lines"].toArray())lines<<line.toString();
        auto actual=QString::fromStdString(editor.document()->buffer().text());if(actual.endsWith('\n'))actual.chop(1);
        if(actual!=lines.join('\n') || editor.navigationPosition()!=QPoint(result["row"].toInt(),result["column"].toInt()))
            qFatal("Ex oracle [%s]: text [%s] vs [%s], position %d:%d vs %d:%d",qPrintable(keys),qPrintable(actual),qPrintable(lines.join('\n')),editor.navigationPosition().x(),editor.navigationPosition().y(),result["row"].toInt(),result["column"].toInt());
    }
    qInfo("Neovim Ex oracle: %lld scenarios passed",qint64(sequences.size()));
}
