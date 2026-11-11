#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <algorithm>
#include <git2.h>
#include <stdexcept>

// Raw lines include their terminators: EOF and CRLF are part of the comparison.
namespace GitDiff {
struct Row { int left{-1}, right{-1}; bool changed{false}; };
struct Hunk { int row, rows, left, leftCount, right, rightCount; };
struct Model {
    QVector<QByteArray> left, right;
    QVector<Row> rows;
    QVector<Hunk> hunks;
    bool coarse{false};
};
inline QVector<QByteArray> lines(const QByteArray& text) {
    QVector<QByteArray> result;
    for (qsizetype start = 0; start < text.size();) {
        auto end = text.indexOf('\n', start); if (end < 0) end = text.size() - 1;
        result.append(text.mid(start, end - start + 1)); start = end + 1;
    }
    return result;
}
inline QByteArray join(const QVector<QByteArray>& lines) { QByteArray out; for (const auto& line : lines) out += line; return out; }
inline Model compare(const QByteArray& a, const QByteArray& b, bool displayEol = false) {
    Model m; m.left = lines(a); m.right = lines(b);
    // The visual editor shows CRLF and LF identically. Compare that visible
    // text when requested, while retaining raw lines for snapshots and edits.
    auto oldBytes=a,newBytes=b;
    if(displayEol) { oldBytes.replace("\r\n","\n"); newBytes.replace("\r\n","\n"); }
    // libgit2's xdiff keeps separated edits precise even beyond the old LCS
    // matrix budget. Force text: raw terminators and invalid UTF-8 remain bytes.
    struct Library {
        Library() { if(git_libgit2_init()<0) throw std::runtime_error("Cannot initialize libgit2"); }
        ~Library() { git_libgit2_shutdown(); }
    } library;
    git_diff_options options;
    git_diff_options_init(&options,GIT_DIFF_OPTIONS_VERSION);
    options.flags=GIT_DIFF_FORCE_TEXT;
    options.context_lines=0; options.interhunk_lines=0;
    struct Changes { QVector<char> ops; int left=0,right=0; } changes;
    const auto hunk=[](const git_diff_delta*,const git_diff_hunk* h,void* payload)->int {
        auto& c=*static_cast<Changes*>(payload);
        const int left=h->old_start-(h->old_lines?1:0);
        const int right=h->new_start-(h->new_lines?1:0);
        while(c.left<left && c.right<right) { c.ops.append(' '); ++c.left; ++c.right; }
        return 0;
    };
    const auto line=[](const git_diff_delta*,const git_diff_hunk*,const git_diff_line* l,void* payload)->int {
        auto& c=*static_cast<Changes*>(payload);
        if(l->origin=='-') { c.ops.append('-'); ++c.left; }
        else if(l->origin=='+') { c.ops.append('+'); ++c.right; }
        else if(l->origin==' ') { c.ops.append(' '); ++c.left; ++c.right; }
        return 0;
    };
    if(git_diff_buffers(oldBytes.constData(),size_t(oldBytes.size()),"old",newBytes.constData(),size_t(newBytes.size()),"new",
                        &options,nullptr,nullptr,hunk,line,&changes)<0)
        throw std::runtime_error("Cannot calculate diff with libgit2");
    while(changes.left<m.left.size() && changes.right<m.right.size()) {
        changes.ops.append(' '); ++changes.left; ++changes.right;
    }
    const auto& ops=changes.ops;
    int i=0,j=0;
    for(int p=0;p<ops.size();) {
        if(ops[p]==' ') { m.rows.append({i++,j++,false}); ++p; continue; }
        Hunk h{int(m.rows.size()),0,i,0,j,0};
        while(p<ops.size() && ops[p]!=' ') { if(ops[p++]=='-') ++h.leftCount; else ++h.rightCount; }
        h.rows=qMax(h.leftCount,h.rightCount);
        for(int r=0;r<h.rows;++r) m.rows.append({r<h.leftCount?i+r:-1,r<h.rightCount?j+r:-1,true});
        i+=h.leftCount; j+=h.rightCount; m.hunks.append(h);
    }
    return m;
}
// Apply selected changed rows to the old version. Paired rows are atomic replacements.
inline QByteArray selected(const Model& m, int first, int last, bool reverse = false) {
    QByteArray result;
    for(int r=0;r<m.rows.size();++r) {
        const auto& row=m.rows[r]; const bool use=row.changed && r>=first && r<=last;
        const int index = (use != reverse) ? row.right : row.left;
        const auto& source = (use != reverse) ? m.right : m.left;
        if(index>=0) result+=source[index];
    }
    return result;
}
inline QByteArray quote(const QString& path) {
    const auto bytes=path.toUtf8(); QByteArray result="\"";
    for(unsigned char c:bytes) {
        if(c=='\\' || c=='"') { result+='\\'; result+=char(c); }
        else if(c<32 || c>=127) { result+='\\'; result+=char('0'+((c>>6)&7)); result+=char('0'+((c>>3)&7)); result+=char('0'+(c&7)); }
        else result+=char(c);
    }
    return result+'"';
}
// Full-file context deliberately rejects stale index contents; no fuzzy hunk offsets.
inline QByteArray patch(const QString& path, const QByteArray& oldText, const QByteArray& newText, bool oldExists = true, bool newExists = true, const QByteArray& mode = "100644") {
    if(oldText==newText && oldExists==newExists) return {};
    const auto a=lines(oldText), b=lines(newText);
    QByteArray out="diff --git "+quote("a/"+path)+" "+quote("b/"+path)+"\n";
    if(!oldExists) out+="new file mode "+mode+"\n";
    if(!newExists) out+="deleted file mode "+mode+"\n";
    if(a.isEmpty() && b.isEmpty()) return out;
    out+="--- "+(oldExists?quote("a/"+path):QByteArray("/dev/null"))+"\n+++ "+(newExists?quote("b/"+path):QByteArray("/dev/null"))+"\n";
    out+="@@ -"+QByteArray::number(a.isEmpty()?0:1)+","+QByteArray::number(a.size())+" +"+QByteArray::number(b.isEmpty()?0:1)+","+QByteArray::number(b.size())+" @@\n";
    const auto append=[&](char sign,const QVector<QByteArray>& values) { for(const auto& line:values) { out+=sign; out+=line; if(!line.endsWith('\n')) out+="\n\\ No newline at end of file\n"; } };
    append('-',a); append('+',b); return out;
}
inline QByteArray unified(const Model& m, const QString& path) {
    if(m.hunks.isEmpty()) return {};
    QByteArray out="--- "+quote("a/"+path)+"\n+++ "+quote("b/"+path)+"\n";
    QVector<QPair<int,int>> ranges;
    for(const auto& h:m.hunks) {
        const int start=qMax(0,h.row-3),end=qMin(int(m.rows.size()),h.row+h.rows+3);
        if(!ranges.isEmpty() && start<=ranges.last().second) ranges.last().second=end;
        else ranges.append({start,end});
    }
    int oldLine=0,newLine=0,position=0;
    const auto append=[&](char sign,const QByteArray& line) { out+=sign; out+=line; if(!line.endsWith('\n')) out+="\n\\ No newline at end of file\n"; };
    for(const auto& range:ranges) {
        while(position<range.first) { const auto& r=m.rows[position++]; oldLine+=r.left>=0; newLine+=r.right>=0; }
        int oldCount=0,newCount=0;
        for(int r=range.first;r<range.second;++r) { oldCount+=m.rows[r].left>=0; newCount+=m.rows[r].right>=0; }
        out+="@@ -"+QByteArray::number(oldLine+(oldCount?1:0))+","+QByteArray::number(oldCount)+" +"+QByteArray::number(newLine+(newCount?1:0))+","+QByteArray::number(newCount)+" @@\n";
        while(position<range.second) {
            const auto& r=m.rows[position];
            if(!r.changed) { append(' ',m.left[r.left]); ++position; continue; }
            const int begin=position;
            while(position<range.second && m.rows[position].changed) ++position;
            for(int row=begin;row<position;++row) if(m.rows[row].left>=0) append('-',m.left[m.rows[row].left]);
            for(int row=begin;row<position;++row) if(m.rows[row].right>=0) append('+',m.right[m.rows[row].right]);
        }
        oldLine+=oldCount; newLine+=newCount;
    }
    return out;
}
inline QString displayText(const QByteArray& text) { auto s=QString::fromUtf8(text); s.replace("\r\n","\n"); return s; }
inline bool textual(const QByteArray& text) {
    auto shown=displayText(text);
    return !text.contains('\0') && QString::fromUtf8(text).toUtf8()==text && !shown.contains('\r') &&
           !shown.contains(QChar(0x2028)) && !shown.contains(QChar(0x2029));
}
// Preserve original line endings outside the edited interval, including mixed EOL files.
inline QByteArray edited(const QByteArray& raw, const QString& shown, const QString& changed) {
    int prefix=0,suffix=0;
    while(prefix<shown.size() && prefix<changed.size() && shown[prefix]==changed[prefix]) ++prefix;
    while(suffix<shown.size()-prefix && suffix<changed.size()-prefix && shown[shown.size()-1-suffix]==changed[changed.size()-1-suffix]) ++suffix;
    const QString original=QString::fromUtf8(raw);
    const auto position=[&](int logical) { int p=0; for(int i=0;i<logical && p<original.size();++i,++p) if(original[p]=='\r' && p+1<original.size() && original[p+1]=='\n') ++p; return p; };
    auto inserted=changed.mid(prefix,changed.size()-prefix-suffix);
    if(raw.contains("\r\n")) inserted.replace("\n","\r\n");
    return (original.left(position(prefix))+inserted+original.mid(position(shown.size()-suffix))).toUtf8();
}
}
