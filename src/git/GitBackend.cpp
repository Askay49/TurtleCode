#include "GitBackend.hpp"
#include <git2.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace {
void check(int code) {
    if(code>=0) return;
    const auto* error=git_error_last();
    throw std::runtime_error(error && error->message?error->message:"libgit2 operation failed");
}
struct Library {
    Library() { check(git_libgit2_init()); }
    ~Library() { git_libgit2_shutdown(); }
};
template<class T, void (*Free)(T*)> struct Handle {
    T* p=nullptr;
    ~Handle() { Free(p); }
    operator T*() const { return p; }
    T** out() { return &p; }
};
using Repository=Handle<git_repository,git_repository_free>;
using Index=Handle<git_index,git_index_free>;
using Tree=Handle<git_tree,git_tree_free>;
using Reference=Handle<git_reference,git_reference_free>;
constexpr qsizetype limit=8*1024*1024;
void open(Repository& repo,const QString& path) { check(git_repository_open(repo.out(),QFile::encodeName(path).constData())); }
void headTree(Tree& tree,git_repository* repo) {
    Handle<git_commit,git_commit_free> commit;
    git_oid oid;
    const int code=git_reference_name_to_id(&oid,repo,"HEAD");
    if(code==GIT_ENOTFOUND || code==GIT_EUNBORNBRANCH) return;
    check(code); check(git_commit_lookup(commit.out(),repo,&oid)); check(git_commit_tree(tree.out(),commit));
}
QByteArray blob(git_repository* repo,const git_oid* oid,GitDiffSnapshot& snapshot) {
    Handle<git_blob,git_blob_free> value; check(git_blob_lookup(value.out(),repo,oid));
    const auto size=git_blob_rawsize(value);
    if(size>limit) { snapshot.special=true; return {}; }
    return QByteArray(static_cast<const char*>(git_blob_rawcontent(value)),qsizetype(size));
}
void mode(GitDiffSnapshot& snapshot,unsigned value) {
    snapshot.mode=QByteArray::number(value,8);
    snapshot.special |= value!=GIT_FILEMODE_BLOB && value!=GIT_FILEMODE_BLOB_EXECUTABLE;
}
QByteArray fromIndex(git_repository* repo,git_index* index,const QString& path,int stage,GitDiffSnapshot& snapshot,bool optional) {
    const auto* entry=git_index_get_bypath(index,QFile::encodeName(path).constData(),stage);
    if(!entry) {
        if(optional) return {};
        throw std::runtime_error("The index changed. Refresh the diff and retry.");
    }
    mode(snapshot,entry->mode);
    if(entry->mode==GIT_FILEMODE_COMMIT) return {};
    return blob(repo,&entry->id,snapshot);
}
QByteArray fromHead(git_repository* repo,git_tree* tree,const QString& path,GitDiffSnapshot& snapshot) {
    if(!tree) return {};
    Handle<git_tree_entry,git_tree_entry_free> entry;
    check(git_tree_entry_bypath(entry.out(),tree,QFile::encodeName(path).constData()));
    mode(snapshot,git_tree_entry_filemode(entry));
    if(git_tree_entry_type(entry)!=GIT_OBJECT_BLOB) return {};
    return blob(repo,git_tree_entry_id(entry),snapshot);
}
int printLine(const git_diff_delta*,const git_diff_hunk*,const git_diff_line* line,void* payload) {
    auto& bytes=*static_cast<QByteArray*>(payload);
    if(bytes.size()>=limit || line->content_len>size_t(limit-bytes.size()-1)) return GIT_EUSER;
    if(line->origin==' ' || line->origin=='+' || line->origin=='-') bytes+=line->origin;
    bytes.append(line->content,qsizetype(line->content_len)); return 0;
}
}
QString GitBackend::discover(const QString& root) {
    Library library; Repository repo;
    const int code=git_repository_open_ext(repo.out(),QFile::encodeName(root).constData(),0,nullptr);
    if(code==GIT_ENOTFOUND) return {};
    check(code);
    const char* workdir=git_repository_workdir(repo);
    return workdir?QDir::cleanPath(QFile::decodeName(workdir)):QString{};
}
GitStatus GitBackend::status(const QString& repository) {
    Library library; Repository repo; open(repo,repository);
    GitStatus result; Reference head;
    const int headCode=git_repository_head(head.out(),repo);
    result.unborn=headCode==GIT_EUNBORNBRANCH;
    if(result.unborn) {
        Reference symbolic; check(git_reference_lookup(symbolic.out(),repo,"HEAD"));
        result.currentBranch=QString::fromUtf8(git_reference_symbolic_target(symbolic)).mid(11);
        result.branch="No commits yet on "+result.currentBranch;
    } else {
        check(headCode);
        if(git_repository_head_detached(repo)==1) result.branch="HEAD (no branch)";
        else {
            result.branch=result.currentBranch=QString::fromUtf8(git_reference_shorthand(head));
            Reference upstream;
            const int code=git_branch_upstream(upstream.out(),head);
            if(code==0) {
                result.branch+="..."+QString::fromUtf8(git_reference_shorthand(upstream));
                size_t ahead=0,behind=0;
                if(git_graph_ahead_behind(&ahead,&behind,repo,git_reference_target(head),git_reference_target(upstream))==0) {
                    QStringList counts; if(ahead) counts<<QString("ahead %1").arg(ahead); if(behind) counts<<QString("behind %1").arg(behind);
                    if(!counts.isEmpty()) result.branch+=" ["+counts.join(", ")+"]";
                }
            } else if(code!=GIT_ENOTFOUND) check(code);
        }
    }
    Handle<git_branch_iterator,git_branch_iterator_free> branches;
    check(git_branch_iterator_new(branches.out(),repo,GIT_BRANCH_LOCAL));
    for(;;) {
        Reference branch; git_branch_t type;
        const int code=git_branch_next(branch.out(),&type,branches);
        if(code==GIT_ITEROVER) break;
        check(code); const char* name=nullptr; check(git_branch_name(&name,branch)); result.branches<<QString::fromUtf8(name);
    }
    std::sort(result.branches.begin(),result.branches.end());
    if(result.unborn && !result.branches.contains(result.currentBranch)) result.branches.prepend(result.currentBranch);
    Index index; check(git_repository_index(index.out(),repo));
    git_status_options options; check(git_status_options_init(&options,GIT_STATUS_OPTIONS_VERSION));
    options.flags=GIT_STATUS_OPT_INCLUDE_UNTRACKED|GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS|
        GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX|GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;
    Handle<git_status_list,git_status_list_free> list; check(git_status_list_new(list.out(),repo,&options));
    for(size_t i=0;i<git_status_list_entrycount(list);++i) {
        const auto* value=git_status_byindex(list,i); const auto flags=value->status;
        const auto* hi=value->head_to_index; const auto* iw=value->index_to_workdir;
        GitEntry entry;
        const char* path=iw?iw->new_file.path:hi->new_file.path;
        if(!path) path=iw?iw->old_file.path:hi->old_file.path;
        entry.path=QFile::decodeName(path);
        if(flags&GIT_STATUS_CONFLICTED) {
            const auto bytes=QFile::encodeName(entry.path);
            const bool base=git_index_get_bypath(index,bytes.constData(),1),ours=git_index_get_bypath(index,bytes.constData(),2),theirs=git_index_get_bypath(index,bytes.constData(),3);
            if(base) { entry.index=ours?'U':'D'; entry.worktree=theirs?'U':'D'; }
            else { entry.index=ours?'A':'U'; entry.worktree=theirs?'A':'U'; }
        } else if(flags&GIT_STATUS_WT_NEW && !(flags&GIT_STATUS_INDEX_NEW)) { entry.index=entry.worktree='?'; }
        else {
            entry.index=flags&GIT_STATUS_INDEX_RENAMED?'R':flags&GIT_STATUS_INDEX_NEW?'A':flags&GIT_STATUS_INDEX_DELETED?'D':flags&GIT_STATUS_INDEX_TYPECHANGE?'T':flags&GIT_STATUS_INDEX_MODIFIED?'M':' ';
            entry.worktree=flags&GIT_STATUS_WT_DELETED?'D':flags&GIT_STATUS_WT_TYPECHANGE?'T':flags&(GIT_STATUS_WT_MODIFIED|GIT_STATUS_WT_UNREADABLE)?'M':' ';
            if(entry.index=='R') entry.originalPath=QFile::decodeName(hi->old_file.path);
        }
        result.entries<<entry;
    }
    result.valid=true; return result;
}
GitDiffSnapshot GitBackend::versions(GitDiffSnapshot snapshot) {
    Library library; Repository repo; open(repo,snapshot.repository);
    Index index; check(git_repository_index(index.out(),repo)); Tree tree; headTree(tree,repo);
    const auto& e=snapshot.entry;
    if(e.conflict()) {
        snapshot.base=fromIndex(repo,index,e.path,1,snapshot,true);
        snapshot.ours=fromIndex(repo,index,e.path,2,snapshot,true);
        snapshot.theirs=fromIndex(repo,index,e.path,3,snapshot,true);
        snapshot.left=snapshot.ours;
    } else if(snapshot.staged) {
        if(e.index!='A') snapshot.left=fromHead(repo,tree,e.originalPath.isEmpty()?e.path:e.originalPath,snapshot);
        if(e.index!='D') snapshot.right=fromIndex(repo,index,e.path,0,snapshot,false);
    } else if(!e.untracked() && e.index!='D') snapshot.left=fromIndex(repo,index,e.path,0,snapshot,false);
    const QFileInfo info(snapshot.repository+"/"+e.path);
    snapshot.special |= info.isSymLink() || info.isDir();
    if(!snapshot.staged && info.exists() && !snapshot.special) {
        QFile file(info.absoluteFilePath());
        if(!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
        if(file.size()>limit) snapshot.special=true;
        else { snapshot.right=file.read(limit+1); if(snapshot.right.size()>limit) { snapshot.right.clear(); snapshot.special=true; } }
        if(e.untracked()) snapshot.mode=info.permission(QFile::ExeOwner)?"100755":"100644";
    }
    // Preserve patch metadata (renames, modes, binary/submodule changes) as well
    // as the bounded text versions, without launching a Git process.
    git_diff_options options; check(git_diff_options_init(&options,GIT_DIFF_OPTIONS_VERSION));
    options.flags=GIT_DIFF_INCLUDE_UNTRACKED|GIT_DIFF_RECURSE_UNTRACKED_DIRS|GIT_DIFF_SHOW_UNTRACKED_CONTENT|GIT_DIFF_DISABLE_PATHSPEC_MATCH|GIT_DIFF_INCLUDE_TYPECHANGE;
    options.max_size=limit;
    auto paths=e.paths(); QVector<QByteArray> encoded; for(const auto& path:paths) encoded<<QFile::encodeName(path);
    QVector<char*> pointers; for(auto& path:encoded) pointers<<path.data();
    options.pathspec={pointers.data(),size_t(pointers.size())};
    Handle<git_diff,git_diff_free> diff;
    if(snapshot.staged) check(git_diff_tree_to_index(diff.out(),repo,tree,index,&options));
    else check(git_diff_index_to_workdir(diff.out(),repo,index,&options));
    git_diff_find_options find; check(git_diff_find_options_init(&find,GIT_DIFF_FIND_OPTIONS_VERSION)); find.flags=GIT_DIFF_FIND_RENAMES;
    check(git_diff_find_similar(diff,&find));
    QByteArray patch; const int code=git_diff_print(diff,GIT_DIFF_FORMAT_PATCH,printLine,&patch);
    if(code==GIT_EUSER) patch+="\n[Diff truncated at 8 MiB]\n"; else check(code);
    snapshot.patch=QString::fromUtf8(patch); return snapshot;
}
