#pragma once
#include <QInputMethodEvent>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>

static void vimKeys(EditorWidget& editor,const QString& keys) {
    for(int i=0;i<keys.size();) {
        if(keys.mid(i,5)=="<Esc>"){QTest::keyClick(&editor,Qt::Key_Escape);i+=5;}
        else if(keys.mid(i,4)=="<CR>"){QTest::keyClick(&editor,Qt::Key_Return);i+=4;}
        else if(keys.mid(i,4)=="<BS>"){QTest::keyClick(&editor,Qt::Key_Backspace);i+=4;}
        else if(keys.mid(i,5)=="<Tab>"){QTest::keyClick(&editor,Qt::Key_Tab);i+=5;}
        else if(keys.mid(i,5)=="<C-r>"){QTest::keyClick(&editor,Qt::Key_R,Qt::ControlModifier);i+=5;}
        else if(keys.mid(i,5)=="<C-v>"){QTest::keyClick(&editor,Qt::Key_V,Qt::ControlModifier);i+=5;}
        else if(keys.mid(i,5)=="<C-o>"){QTest::keyClick(&editor,Qt::Key_O,Qt::ControlModifier);i+=5;}
        else if(keys.mid(i,5)=="<C-i>"){QTest::keyClick(&editor,Qt::Key_I,Qt::ControlModifier);i+=5;}
        else if(keys.mid(i,3)=="<C-" && i+4<keys.size() && keys[i+4]=='>'){QTest::keyClick(&editor,Qt::Key(Qt::Key_A+keys[i+3].toLower().unicode()-'a'),Qt::ControlModifier);i+=5;}
        else if(keys.mid(i,6)=="<Left>"){QTest::keyClick(&editor,Qt::Key_Left);i+=6;}
        else if(keys.mid(i,7)=="<Right>"){QTest::keyClick(&editor,Qt::Key_Right);i+=7;}
        else if(keys[i].unicode()>127){auto size=keys[i].isHighSurrogate() && i+1<keys.size() && keys[i+1].isLowSurrogate()?2:1;QKeyEvent event(QEvent::KeyPress,Qt::Key_unknown,Qt::NoModifier,keys.mid(i,size));QApplication::sendEvent(&editor,&event);i+=size;}
        else {QTest::keyClicks(&editor,keys.mid(i,1));++i;}
        QElapsedTimer wait;wait.start();while((editor.isBusy() || editor.isVimReplaying() || (editor.vimCommandExecuting() && !editor.vimSubstitutionConfirming())) && wait.elapsed()<10000)QTest::qWait(5);if(editor.isBusy() || editor.isVimReplaying() || (editor.vimCommandExecuting() && !editor.vimSubstitutionConfirming()))qFatal("Vim key wait: [%s], through [%s], busy=%d replay=%d ex=%d confirm=%d",qPrintable(keys),qPrintable(keys.left(i)),editor.isBusy(),editor.isVimReplaying(),editor.vimCommandExecuting(),editor.vimSubstitutionConfirming());
    }
}
static void vimTests() {
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString& text){fprintf(stderr,"%s\n",qPrintable(text));});
    using Mode=VimController::Mode;using Command=VimController::Command;
    VimController controller;
    QKeyEvent h(QEvent::KeyPress,Qt::Key_H,Qt::NoModifier,"h");
    Q_ASSERT(controller.command(h)==Command::Pass);controller.setEnabled(true);
    Q_ASSERT(controller.command(h)==Command::Left);
    QKeyEvent insert(QEvent::KeyPress,Qt::Key_I,Qt::NoModifier,"i");
    Q_ASSERT(controller.command(insert)==Command::Insert);controller.setMode(Mode::Insert);
    Q_ASSERT(controller.command(h)==Command::Pass);controller.setEnabled(false);Q_ASSERT(controller.acceptsText());
    QSettings().setValue("editor/vimMode",true);QSettings().setValue("editor/minimap",false);QSettings().setValue("editor/autoPairs",false);
    EditorWidget editor;load(editor,"abc\nxy");
    Q_ASSERT(editor.vimEnabled() && editor.vimMode()==Mode::Normal && !editor.isModified());
    vimKeys(editor,"z123<Esc>");Q_ASSERT(editor.document()->buffer().text()=="abc\nxy");
    QTest::keyClick(&editor,Qt::Key_Delete,Qt::ControlModifier);
    QTest::keyClick(&editor,Qt::Key_Backspace,Qt::AltModifier);
    QTest::keyClick(&editor,Qt::Key_Return,Qt::ControlModifier);
    QTest::keyClick(&editor,Qt::Key_X,Qt::MetaModifier);
    Q_ASSERT(editor.document()->buffer().text()=="abc\nxy");
    vimKeys(editor,"iZ<Esc>");Q_ASSERT(editor.document()->buffer().text()=="Zabc\nxy");
    Q_ASSERT(editor.vimMode()==Mode::Normal && editor.navigationPosition()==QPoint(1,1));
    QTest::keyClick(&editor,Qt::Key_Escape);Q_ASSERT(editor.navigationPosition()==QPoint(1,1));
    QTest::keyClick(&editor,Qt::Key_Z,Qt::ControlModifier);Q_ASSERT(editor.document()->buffer().text()=="abc\nxy" && !editor.isModified());
    QTest::keyClick(&editor,Qt::Key_Y,Qt::ControlModifier);Q_ASSERT(editor.document()->buffer().text()=="Zabc\nxy");
    load(editor,"abc\nxy");vimKeys(editor,"vl");Q_ASSERT(editor.vimMode()==Mode::Visual && editor.selectedText()=="ab");
    vimKeys(editor,"ll");Q_ASSERT(editor.selectedText()=="abc" && editor.navigationPosition()==QPoint(1,3));
    vimKeys(editor,"hh");Q_ASSERT(editor.selectedText()=="a");
    vimKeys(editor,"j");Q_ASSERT(editor.selectedText()=="abc\nx");
    QTest::keyClick(&editor,Qt::Key_C,Qt::ControlModifier);Q_ASSERT(QApplication::clipboard()->text()=="abc\nx");
    QApplication::clipboard()->setText("blocked");QTest::keyClick(&editor,Qt::Key_V,Qt::ControlModifier);
    Q_ASSERT(editor.document()->buffer().text()=="abc\nxy");
    vimKeys(editor,"<Esc>");Q_ASSERT(editor.selectedText().isEmpty() && !editor.isModified());
    editor.setNavigationPosition(1,1);vimKeys(editor,"lvh");Q_ASSERT(editor.selectedText()=="ab");
    editor.setVimEnabled(false);Q_ASSERT(editor.selectedText()=="ab");
    QTest::keyClicks(&editor,"X");Q_ASSERT(editor.document()->buffer().text()=="Xc\nxy");
    editor.setVimEnabled(true);load(editor,"abc\r\nxy");vimKeys(editor,"llll");Q_ASSERT(editor.navigationPosition()==QPoint(1,3));
    vimKeys(editor,"jjjj");Q_ASSERT(editor.navigationPosition()==QPoint(2,2));
    editor.setNavigationPosition(1,1);vimKeys(editor,"aZ<Esc>");Q_ASSERT(editor.document()->buffer().text()=="aZbc\r\nxy");
    const auto unicode=QString::fromUtf8("a😀éz\nabc").toUtf8();editor.loadBuffer(PieceTreeBuffer(std::string(unicode.constData(),unicode.size())),"sample.txt");
    vimKeys(editor,"lv");Q_ASSERT(editor.selectedText()==QString::fromUtf8("😀"));
    vimKeys(editor,"l");Q_ASSERT(editor.selectedText()==QString::fromUtf8("😀é"));vimKeys(editor,"<Esc>");
    Q_ASSERT(editor.navigationPosition()==QPoint(1,4)); // UTF-16, at the combining cluster start.
    QInputMethodEvent ime;ime.setCommitString(QString::fromUtf8("Привет"));QApplication::sendEvent(&editor,&ime);
    Q_ASSERT(editor.document()->buffer().text()==std::string(unicode.constData(),unicode.size()));
    vimKeys(editor,"i");QInputMethodEvent typed;typed.setCommitString(QString::fromUtf8("Я"));QApplication::sendEvent(&editor,&typed);
    Q_ASSERT(editor.document()->buffer().text().find(QString::fromUtf8("Я").toStdString())!=std::string::npos);
    QTest::keyClick(&editor,Qt::Key_BracketLeft,Qt::ControlModifier);Q_ASSERT(editor.vimMode()==Mode::Normal);
    load(editor,"");vimKeys(editor,"hjklv<Esc>iA<Esc>");Q_ASSERT(editor.document()->buffer().text()=="A");
    load(editor,"abc");vimKeys(editor,"v");editor.showFindBar();
    auto* find=editor.findChild<QLineEdit*>("fileFindInput");
    if(!find)for(auto* input:editor.findChildren<QLineEdit*>())if(input->hasFocus())find=input;
    Q_ASSERT(find && find->hasFocus());QTest::keyClicks(find,"jiv");Q_ASSERT(editor.document()->buffer().text()=="abc");
    QTest::keyClick(find,Qt::Key_Escape);Q_ASSERT(editor.hasFocus() && editor.vimMode()==Mode::Normal);
    load(editor,"abc");vimKeys(editor,"i");editor.showFindBar();
    for(auto* input:editor.findChildren<QLineEdit*>())if(input->hasFocus())find=input;
    QTest::keyClick(find,Qt::Key_Escape);Q_ASSERT(editor.vimMode()==Mode::Insert);
    QTest::keyClick(&editor,Qt::Key_Escape);Q_ASSERT(editor.vimMode()==Mode::Normal);
    load(editor,"ab");vimKeys(editor,"la");
    const QJsonArray completion{QJsonObject{{"label","alpha"}}};
    editor.showCompletions(completion);auto* popup=editor.findChild<QCompleter*>();Q_ASSERT(popup && popup->popup()->isVisible());
    QTest::keyClick(popup->popup(),Qt::Key_Escape);Q_ASSERT(editor.vimMode()==Mode::Insert && !popup->popup()->isVisible());
    editor.showCompletions(completion);QTest::keyClick(popup->popup(),Qt::Key_Tab);
    Q_ASSERT(editor.vimMode()==Mode::Insert && editor.document()->buffer().text()=="alpha");
    QTest::keyClick(&editor,Qt::Key_Escape);Q_ASSERT(editor.vimMode()==Mode::Normal);
    // A huge line still uses bounded navigation context and cursor measurement.
    editor.loadBuffer(PieceTreeBuffer(std::string(8*1024*1024,'a')),"large.txt");
    editor.setNavigationPosition(1,4000000);QElapsedTimer timer;timer.start();vimKeys(editor,"vll<Esc>iX<Esc>");
    Q_ASSERT(timer.elapsed()<2000 && editor.byteSize()==8*1024*1024+1);
    // Compare independent edit/motion sequences against clean Neovim.
    const auto nvim=QStandardPaths::findExecutable("nvim");
    if(nvim.isEmpty())qInfo("Neovim oracle skipped: nvim is not installed");
    else for(const auto& keys:QStringList{"iZ<Esc>","laZ<Esc>","jliXY<Esc>","vll<Esc>","lliQ<Esc>hh"}) {
        load(editor,"abc\nxy");vimKeys(editor,keys);
        const QString script=QString("lua vim.api.nvim_buf_set_lines(0,0,-1,false,{'abc','xy'}); local k=vim.api.nvim_replace_termcodes([=[%1]=],true,false,true); vim.api.nvim_feedkeys(k,'xt',false); local p=vim.api.nvim_win_get_cursor(0); io.stdout:write('ORACLE:'..vim.fn.json_encode({lines=vim.api.nvim_buf_get_lines(0,0,-1,false),row=p[1],column=p[2]+1}))").arg(keys);
        QProcess process;process.start(nvim,{"--headless","-u","NONE","-i","NONE","-n","-c",script,"-c","qa!"});
        Q_ASSERT(process.waitForFinished(10000) && process.exitCode()==0);
        const auto output=process.readAllStandardOutput();const int start=output.indexOf("ORACLE:");Q_ASSERT(start>=0);
        const auto result=QJsonDocument::fromJson(output.mid(start+7)).object();QStringList lines;
        for(const auto& line:result["lines"].toArray())lines<<line.toString();
        Q_ASSERT(QString::fromStdString(editor.document()->buffer().text())==lines.join('\n'));
        Q_ASSERT(editor.navigationPosition()==QPoint(result["row"].toInt(),result["column"].toInt()));
    }
}
