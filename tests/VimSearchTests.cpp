#include "editor/VimSearch.hpp"
#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QDir>
#include <QElapsedTimer>
#include <cstdio>
#include <cassert>
using namespace VimSearch;
static QString json(const QString& text){auto bytes=QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);return QString::fromUtf8(bytes.mid(1,bytes.size()-2));}
static std::string substituted(const PieceTreeBuffer& buffer,const Subject& subject,const Plan& plan){
    std::string result;std::size_t at=0;
    for(std::size_t n=0;n<plan.count;++n){const auto edit=plan.at(n);result+=buffer.slice(at,edit.begin-at);result+=plan.text->read(edit.offset,edit.length);at=edit.end;}
    result+=buffer.slice(at,buffer.size()-at);return result;
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);QTemporaryDir directory;assert(directory.isValid());assert(QDir::setCurrent(directory.path()));
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString& value){fprintf(stderr,"%s\n",qPrintable(value));});
    struct Case {QString text,pattern,replacement;bool global{true};};
    const QVector<Case> cases={
        {"foo foo\nfoo","foo","bar"},{"foo foo\nfoo","foo","bar",false},
        {"a+b aab a.b","a+b","X"},{"a+b aab a.b","a\\+b","X"},{"a+b aab a.b","a.b","X"},
        {"ab abb ac","ab\\?","X"},{"ab abb ac","ab\\=","X"},{"aaa ab a","a\\{2,3}","X"},
        {"aaaa ab","a\\{-}","X"},{"aaaa ab","a\\{-1,3}","X"},
        {"one two one","\\(one\\|two\\)","[\\1]"},{"ab abba","\\(a\\)\\(b\\)","\\2\\1"},
        {"foobar foobaz","foo\\zsbar","X"},{"foobar foobaz","foo\\zebar","X"},
        {"foo food xfoo foo_","\\<foo\\>","X"},{"foo food xfoo foo_","\\v<foo>","X"},
        {"a.b aXb","\\Va.b","X"},{"a.b aXb","\\Ma.b","X"},{"a.b aXb","\\Ma\\.b","X"},
        {"a.b aXb","\\m.a","X"},{"Foo foo FOO","foo\\c","X"},{"Foo foo FOO","foo\\C","X"},
        {"abc 123 é","[[:alpha:]]\\+","X"},{"abc 123 é","\\d\\+","X"},{"abc 123 é","\\k\\+","X"},
        {"a\nb\nc","^","X"},{"a\nb\nc","$","X"},{"ab\ncd","\\n","X"},
        {"a\nb\nc","a\\_.\\{-}c","X"},{"a\nb\nc","a\\_s\\+b","X"},
        {"foobar fooqux","\\(foo\\)\\@=foo","X"},{"foobar fooqux","foo\\(bar\\)\\@!","X"},
        {"foobar fooqux","\\(foo\\)\\@<=bar","X"},{"foobar xxbar","\\(foo\\)\\@<!bar","X"},
        {"abca aac","a.*\\&.*c","X"},{"x a y","\\%d97","X"},
        {"foo Foo","\\(foo\\)","\\U\\1\\E"},{"FOO FOO","FOO","\\l&"},
        {"foo foo","foo","\\u&"},{"a a","a","&\\&\\0"},
        {"foo","foo","a\\rb"},{"Привет привет","привет\\c","\\U&"},
        {"aaaa","a*","X"},{"abc","a*","X"},{"abc","x*","X"},
        {"ababa","aba","X"},{"foo\nbar","foo\\nbar","X"},{"a\nb\nc","\\n","X",false},
        {"foo\nbar\nfoo\nbar","foo\\nbar","X",false},{"foo foo","foo\\ze foo","X"},
        {"é e","e","X"},{"a😀éz",".","X"},{"é e","é","X"},
        {"é e","e\\%C","X"},{"é e","e\\Z","X"},{"ée","[e]","X"},{"ée","́","X"}
    };
    const auto nvim=QStandardPaths::findExecutable("nvim");int compared=0;
    for(const auto& test:cases){
        const auto bytes=test.text.toUtf8();PieceTreeBuffer buffer(std::string_view(bytes.data(),std::size_t(bytes.size())));Subject subject(buffer,false);Pattern pattern(test.pattern);
        if(!pattern.error().isEmpty())qFatal("Compile [%s]: %s",qPrintable(test.pattern),qPrintable(pattern.error()));
        const auto edits=plan(buffer,subject,pattern,test.replacement,0,buffer.lineCount()-1,test.global,false,"\n");
        const auto actual=QString::fromUtf8(substituted(buffer,subject,*edits).c_str());
        if(nvim.isEmpty())continue;
        auto expression=test.pattern;expression.replace("/","\\/");auto replacement=test.replacement;replacement.replace("/","\\/");
        const auto command=QString("%s/%1/%2/%3e").arg(expression,replacement,test.global?"g":"");
        const auto script=QString("lua local t=vim.fn.json_decode([==[%1]==]); vim.api.nvim_buf_set_lines(0,0,-1,false,vim.split(t,'\\n',{plain=true})); vim.cmd(vim.fn.json_decode([==[%2]==])); io.stdout:write('RESULT:'..vim.fn.json_encode(vim.api.nvim_buf_get_lines(0,0,-1,false)))").arg(json(test.text),json(command));
        QProcess process;process.start(nvim,{"--headless","-u","NONE","-i","NONE","-n","-c",script,"-c","qa!"});assert(process.waitForFinished(10000));
        const auto output=process.readAllStandardOutput();const auto start=output.indexOf("RESULT:");
        if(start<0)qFatal("No oracle result: %s",process.readAllStandardError().constData());
        QStringList lines;for(const auto line:QJsonDocument::fromJson(output.mid(start+7)).array())lines<<line.toString();
        if(actual!=lines.join('\n'))qFatal("Pattern [%s], replacement [%s], initial [%s]: got [%s] vs [%s], oracle errors [%s]",qPrintable(test.pattern),qPrintable(test.replacement),qPrintable(test.text),qPrintable(actual),qPrintable(lines.join('\n')),process.readAllStandardError().constData());
        ++compared;
    }
    PieceTreeBuffer buffer("ababa\nfoo\nfoo");Subject subject(buffer,false);Pattern pattern("aba");
    assert(navigate(subject,pattern,0,true).match->begin==0 && navigate(subject,pattern,0,true).wrapped);assert(navigate(subject,pattern,2,false).match->begin==0);
    assert(navigate(subject,pattern,2,true,3).match->begin==0);
    Pattern invalid("\\(");assert(!invalid.error().isEmpty());
    auto parsed=parseSubstitute("#a|b#x\"y#g | $p");assert(parsed.pattern=="a|b" && parsed.replacement=="x\"y" && parsed.flags=="g" && parsed.rest==" $p");
    QString error;assert(expandReplacement("~!","old",&error)=="old!");expandReplacement("\\=1+1",{},&error);assert(!error.isEmpty());
    PieceTreeBuffer crlf("\xef\xbb\xbf" "a\r\nb\r\n");Subject encoded(crlf,true);Pattern anchors("$");const auto edits=plan(crlf,encoded,anchors,"X",0,1,true,false,"\r\n");
    assert(substituted(crlf,encoded,*edits)=="\xef\xbb\xbf" "aX\r\nbX\r\n");
    PieceTreeBuffer large(std::string(8*1024*1024,'a')+"END");Subject mapped(large,false);Pattern end("END");assert(navigate(mapped,end,0,true).match->begin==8*1024*1024);
    Pattern whole(".*");const auto wholePlan=plan(large,mapped,whole,"X",0,0,true,false,"\n");assert(wholePlan->count==1 && wholePlan->at(0).end==large.size());
    Pattern repeated("a\\+");assert(repeated.find(mapped,0)->end==8*1024*1024);
    bool cancelled=false;try{Pattern hard("\\(a\\+\\)\\+b");hard.find(mapped,0,[]{throw std::runtime_error("cancelled");});}catch(const std::exception&){cancelled=true;}assert(cancelled);
    PieceTreeBuffer hostile(std::string(5000,'a')+"!");Subject adversarial(hostile,false);Pattern hard("\\(a\\+\\)\\+$");
    QElapsedTimer timer;timer.start();int checks=0;std::optional<Match> hostileMatch;
    try{hostileMatch=hard.find(adversarial,0,[&]{if(++checks>3)throw std::runtime_error("cancelled while matching");});}catch(const std::exception&){}
    assert(!hostileMatch && timer.elapsed()<3000);
    PieceTreeBuffer damaged(std::string("\xff",1)+"foo");Subject invalidUtf8(damaged,false);Pattern recoverable("foo");assert(recoverable.find(invalidUtf8,0)->begin==1);
    fprintf(stderr,"Vim pattern/substitute oracle: %d scenarios passed\n",compared);
}
