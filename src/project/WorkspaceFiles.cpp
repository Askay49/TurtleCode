#include "WorkspaceFiles.hpp"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSettings>
#include <vector>
#include <git2.h>

namespace {
QString globRegex(const QString& glob) {
    QString out;
    for (int i=0;i<glob.size();++i) {
        const QChar ch=glob[i];
        if(ch=='\\' && i+1<glob.size()) { out+=QRegularExpression::escape(glob.mid(++i,1)); }
        else if(ch=='*') {
            if(i+1<glob.size() && glob[i+1]=='*') {
                ++i;
                if(i+1<glob.size() && glob[i+1]=='/') { ++i; out+="(?:.*/)?"; }
                else out+=".*";
            } else out+="[^/]*";
        } else if(ch=='?') out+="[^/]";
        else if(ch=='[') {
            const int end=glob.indexOf(']',i+1);
            if(end<0) out+="\\[";
            else { QString chars=glob.mid(i+1,end-i-1); if(chars.startsWith('!')) chars[0]='^'; out+='['+chars+']'; i=end; }
        } else out+=QRegularExpression::escape(QString(ch));
    }
    return out;
}
}
WorkspaceFiles::WorkspaceFiles(QString root, WorkspaceFileOptions options)
    : root_(QDir::cleanPath(QFileInfo(root).absoluteFilePath())), options_(std::move(options)) {
    if(git_libgit2_init()>=0) {
        git_repository* repository=nullptr;
        if(git_repository_open_ext(&repository,QFile::encodeName(root_).constData(),0,nullptr)==0 && git_repository_workdir(repository)) {
            gitRoot_=QFile::decodeName(git_repository_workdir(repository));
            gitRepository_=std::shared_ptr<void>(repository,[](void* value){git_repository_free(static_cast<git_repository*>(value));git_libgit2_shutdown();});
        } else {if(repository)git_repository_free(repository);git_libgit2_shutdown();}
    }
    // Explicit project excludes supplement Git-style ignore files.
    options_.excludes += QSettings().value("project/excludes").toStringList();
}
void WorkspaceFiles::readRules(const QString& path,const QString& base,QList<Rule>& result) {
    QFile input(path); if(!input.open(QIODevice::ReadOnly)) return;
    while(!input.atEnd()) {
        QString pattern=QString::fromUtf8(input.readLine());
        while(pattern.endsWith('\n') || pattern.endsWith('\r')) pattern.chop(1);
        while(pattern.endsWith(' ') && !pattern.endsWith("\\ ")) pattern.chop(1);
        if(pattern.isEmpty() || pattern.startsWith('#')) continue;
        const bool negative=pattern.startsWith('!'); if(negative) pattern.remove(0,1);
        const bool directory=pattern.endsWith('/'); if(directory) pattern.chop(1);
        const bool anchored=pattern.startsWith('/'); if(anchored) pattern.remove(0,1);
        if(pattern.isEmpty()) continue;
        const QString prefix=QRegularExpression::escape(base)+(anchored || pattern.contains('/')?QString{}:QStringLiteral("(?:.*/)?"));
        QRegularExpression regex("^"+prefix+globRegex(pattern)+"$");
        if(regex.isValid()) result.push_back({regex,negative,directory});
    }
}
const QList<WorkspaceFiles::Rule>& WorkspaceFiles::rules(const QString& directory) {
    auto found=rules_.constFind(directory); if(found!=rules_.cend()) return found.value();
    QList<Rule> result;
    if(directory!=root_) result=rules(QFileInfo(directory).absolutePath());
    else {
        for(const auto& pattern:options_.excludes) {
            QRegularExpression re("^(?:.*/)?"+globRegex(pattern.endsWith('/')?pattern.left(pattern.size()-1):pattern)+"$");
            if(re.isValid()) result.push_back({re,false,pattern.endsWith('/')});
        }
        if(!gitRepository_)readRules(root_+"/.git/info/exclude",{},result);
    }
    QString base=QDir(root_).relativeFilePath(directory); if(base==".") base.clear(); else base+='/';
    if(!gitRepository_)readRules(directory+"/.gitignore",base,result);
    readRules(directory+"/.turtleignore",base,result);
    return rules_.insert(directory,std::move(result)).value();
}
bool WorkspaceFiles::accepts(const QFileInfo& info) {
    const auto name=info.fileName();
    if(name==".git" || info.isSymLink()) return false;
    if(!options_.hidden && name.startsWith('.')) return false;
    if(options_.ignored) return true;
    const QString relative=QDir(root_).relativeFilePath(info.absoluteFilePath());
    if(relative.startsWith("../")) return false;
    if(gitRepository_) {
        int ignored=0;const auto path=QDir(gitRoot_).relativeFilePath(info.absoluteFilePath()).toUtf8();
        if(git_ignore_path_is_ignored(&ignored,static_cast<git_repository*>(gitRepository_.get()),path.constData())==0 && ignored)return false;
    }
    bool ignored=false;
    for(const auto& rule:rules(info.absolutePath()))
        if((!rule.directory || info.isDir()) && rule.expression.match(relative).hasMatch()) ignored=!rule.negative;
    return !ignored;
}
void WorkspaceFiles::walk(const std::function<void(const QFileInfo&)>& file,
                          const std::shared_ptr<std::atomic_bool>& cancel,
                          const std::function<void(const QString&)>& directory) {
    struct Frame {QString path;std::unique_ptr<QDirIterator> iterator;};
    std::vector<Frame> stack;
    const auto enter=[&](const QString& path){if(directory)directory(path);stack.push_back({path,std::make_unique<QDirIterator>(path,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System|QDir::NoSymLinks)});};
    enter(root_);
    while(!stack.empty() && (!cancel || !cancel->load())) {
        auto& frame=stack.back();
        if(!frame.iterator->hasNext()){rules_.remove(frame.path);stack.pop_back();continue;}
        frame.iterator->next();const auto info=frame.iterator->fileInfo();
        if(!accepts(info))continue;
        if(info.isDir())enter(info.absoluteFilePath());else if(info.isFile())file(info);
    }
}
