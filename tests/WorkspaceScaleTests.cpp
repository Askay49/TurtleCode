#include "project/WorkspaceFiles.hpp"
#include "project/WorkspaceIndex.hpp"
#include "search/SearchScanner.hpp"
#include "search/ProjectSearchService.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QDebug>
#include "project/DirectoryListing.hpp"
#include <QTextStream>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <thread>
#include <cassert>

static void write(const QString& path,const QByteArray& bytes) {QFile file(path);assert(file.open(QIODevice::WriteOnly));assert(file.write(bytes)==bytes.size());}
static FileQueryResult query(WorkspaceIndex& index,const QString& text) {
    QEventLoop loop;FileQueryResult result;bool done=false;
    index.query(text,{},{},&loop,[&](FileQueryResult value){result=std::move(value);done=true;loop.quit();});
    QTimer::singleShot(60000,&loop,&QEventLoop::quit);loop.exec();assert(done && result.error.isEmpty());return result;
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);app.setOrganizationName("TurtleCodeTests");app.setApplicationName("WorkspaceScale");
    QTemporaryDir temp;assert(temp.isValid());qputenv("XDG_CONFIG_HOME",temp.filePath("config").toUtf8());
    int files=2000,matches=20000,lineMiB=8;
    for(int i=1;i+1<argc;i+=2){const QString arg=QString::fromLocal8Bit(argv[i]);const int n=QString::fromLocal8Bit(argv[i+1]).toInt();if(arg=="--files")files=n;else if(arg=="--matches")matches=n;else if(arg=="--line-mib")lineMiB=n;}
    const auto root=temp.filePath("workspace");assert(QDir().mkpath(root+"/ignored"));assert(QDir().mkpath(root+"/nested"));
    write(root+"/.gitignore","ignored/\n*.tmp\n!important.tmp\n");write(root+"/nested/.gitignore","private.txt\n");write(root+"/.turtleignore","local.txt\n");
    write(root+"/ignored/no.txt","bad");write(root+"/nested/private.txt","bad");write(root+"/nested/public.txt","ok");write(root+"/ignore.tmp","bad");write(root+"/important.tmp","ok");write(root+"/local.txt","bad");
    WorkspaceFiles filter(root);QStringList accepted;filter.walk([&](const QFileInfo& file){accepted<<QDir(root).relativeFilePath(file.absoluteFilePath());});accepted.sort();assert(accepted==QStringList({"important.tmp","nested/public.txt"}));
    WorkspaceFiles overrideFilter(root,{true,true,{}});int all=0;overrideFilter.walk([&](const QFileInfo&){++all;});assert(all==9);
    assert(QProcess::execute("git",{"init","--quiet",root})==0);
    WorkspaceFiles gitFilter(root);QStringList gitAccepted;gitFilter.walk([&](const QFileInfo& file){gitAccepted<<QDir(root).relativeFilePath(file.absoluteFilePath());});gitAccepted.sort();assert(gitAccepted==accepted);
    for(int i=0;i<files;++i)write(root+QString("/needle%1.txt").arg(i,7,10,QChar('0')),"x");
    auto listing=DirectoryListing::scan(root,root,{});assert(listing->size()==files+2);
    assert(listing->page(0,500).size()==std::min(files+2,500));assert(listing->page(files,500).size()==2);
    WorkspaceIndex index;QElapsedTimer elapsed;elapsed.start();index.configure({root});
    FileQueryResult result;
    do{QThread::msleep(20);QCoreApplication::processEvents();result=query(index,"needle");}while(result.indexing && elapsed.elapsed()<60000);
    assert(!result.indexing && result.indexedFiles==files+2 && result.files.size()==std::min(100,files));
    const auto cold=elapsed.elapsed();elapsed.restart();result=query(index,QString("needle%1").arg(files-1,7,10,QChar('0')));assert(!result.files.isEmpty());const auto warm=elapsed.elapsed();
    assert(WorkspaceIndex::score(QString(2000,'x')+"/a", "a")>=0);
    const auto added=root+"/new-file.txt";write(added,"new");index.invalidate();
    elapsed.restart();do{QThread::msleep(20);QCoreApplication::processEvents();result=query(index,"new-file");}while(result.files.isEmpty() && elapsed.elapsed()<10000);assert(!result.files.isEmpty());
    assert(QFile::remove(added));index.invalidate();
    elapsed.restart();do{QThread::msleep(20);QCoreApplication::processEvents();result=query(index,"new-file");}while(!result.files.isEmpty() && elapsed.elapsed()<10000);assert(result.files.isEmpty());
    const auto searchRoot=temp.filePath("search");assert(QDir().mkpath(searchRoot));
    {QFile file(searchRoot+"/many.txt");assert(file.open(QIODevice::WriteOnly));for(int i=0;i<matches;++i)assert(file.write("needle\n")==7);}
    {QFile file(searchRoot+"/long.txt");assert(file.open(QIODevice::WriteOnly));const QByteArray block(1024*1024,'x');for(int i=0;i<lineMiB;++i)assert(file.write(block)==block.size());file.write("needle");}
    auto cancel=std::make_shared<std::atomic_bool>(false);ProjectSearchOptions options;options.roots={searchRoot};options.query="needle";
    elapsed.restart();int batches=0;auto found=scanProject(options,cancel,[&](const ProjectSearchResult&){++batches;});
    assert(found.error.isEmpty() && found.skippedFiles==0 && found.matches.size()==matches+1);
    bool longFound=false;for(const auto& match:found.matches)if(match.path.endsWith("/long.txt")){assert(match.byteOffset==qint64(lineMiB)*1024*1024);longFound=true;}
    assert(longFound);const auto searchMs=elapsed.elapsed();
    options.regularExpression=true;options.query="(x+)+y";options.filesToInclude="long.txt";
    auto pathological=scanProject(options,cancel,[](const ProjectSearchResult&){});assert(pathological.skippedFiles==1 && !pathological.warnings.isEmpty());
    options.regularExpression=false;options.query="needle";cancel->store(true);assert(scanProject(options,cancel,[](const ProjectSearchResult&){}).matches.isEmpty());
    cancel->store(false);elapsed.restart();std::thread cancelThread([cancel]{QThread::msleep(20);cancel->store(true);});
    scanProject(options,cancel,[](const ProjectSearchResult&){});cancelThread.join();const auto cancelMs=elapsed.elapsed();assert(cancelMs<2000);
    const QJsonObject report{{"files",files},{"cold_index_ms",cold},{"warm_query_ms",warm},{"matches",matches},{"search_ms",searchMs},{"long_line_mib",lineMiB},{"progress_batches",batches},{"cancel_ms",cancelMs}};
    QTextStream(stdout)<<QJsonDocument(report).toJson(QJsonDocument::Compact)<<Qt::endl;
}
