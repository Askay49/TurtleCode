#pragma once
#include "GitRuntime.hpp"
#include "core/text/LineAlignment.hpp"
#include <QByteArray>
#include <QString>
#include <QVector>
#include <algorithm>
#include <git2.h>
#include <stdexcept>

// Raw lines include their terminators: EOF and CRLF are part of the comparison.
namespace GitDiff {
struct Hunk { int row, rows, left, leftCount, right, rightCount; };
struct Alignment {
    LineAlignment left, right;

    void append(int leftCount, int rightCount) {
        const int rows = std::max(leftCount, rightCount);
        left.appendLines(leftCount);
        left.appendPadding(rows - leftCount);
        right.appendLines(rightCount);
        right.appendPadding(rows - rightCount);
    }
};
struct Model {
    QVector<QByteArray> left, right;
    Alignment alignment;
    QVector<Hunk> hunks;
    int rowCount{0}; // Rows backed by raw lines, before the editor's final empty line.
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
        Library() { if(GitRuntime::ensureInitialized()<0 || git_libgit2_init()<0) throw std::runtime_error("Cannot initialize libgit2"); }
        ~Library() { git_libgit2_shutdown(); }
    } library;
    git_diff_options options;
    git_diff_options_init(&options,GIT_DIFF_OPTIONS_VERSION);
    options.flags=GIT_DIFF_FORCE_TEXT;
    options.context_lines=0; options.interhunk_lines=0;
    const auto hunk = [](const git_diff_delta*, const git_diff_hunk* h, void* payload) -> int {
        auto& hunks = *static_cast<QVector<Hunk>*>(payload);
        // A zero-length range addresses the line before the insertion point.
        const int left = h->old_start - (h->old_lines != 0);
        const int right = h->new_start - (h->new_lines != 0);
        if (!hunks.isEmpty()) {
            auto& previous = hunks.last();
            if (previous.left + previous.leftCount == left && previous.right + previous.rightCount == right) {
                previous.leftCount += h->old_lines;
                previous.rightCount += h->new_lines;
                return 0;
            }
        }
        hunks.append({0, 0, left, h->old_lines, right, h->new_lines});
        return 0;
    };
    if(git_diff_buffers(oldBytes.constData(),size_t(oldBytes.size()),"old",newBytes.constData(),size_t(newBytes.size()),"new",
                        &options,nullptr,nullptr,hunk,nullptr,&m.hunks)<0)
        throw std::runtime_error("Cannot calculate diff with libgit2");
    const auto appendUnchanged = [&](int leftEnd, int rightEnd) {
        const int count = leftEnd - m.alignment.left.lineCount();
        if (count < 0 || count != rightEnd - m.alignment.right.lineCount())
            throw std::runtime_error("Invalid diff line ranges from libgit2");
        m.alignment.append(count, count);
    };
    for (auto& h : m.hunks) {
        appendUnchanged(h.left, h.right);
        h.row = m.alignment.left.rowCount();
        h.rows = std::max(h.leftCount, h.rightCount);
        m.alignment.append(h.leftCount, h.rightCount);
    }
    appendUnchanged(int(m.left.size()), int(m.right.size()));
    m.rowCount = m.alignment.left.rowCount();
    m.alignment.append(a.isEmpty() || a.endsWith('\n'), b.isEmpty() || b.endsWith('\n'));
    return m;
}
// Apply selected changed rows to the old version. Paired rows are atomic replacements.
inline QByteArray selected(const Model& m, int first, int last, bool reverse = false) {
    const auto& base = reverse ? m.right : m.left;
    const auto& other = reverse ? m.left : m.right;
    QByteArray result;
    int line = 0;
    for (const auto& h : m.hunks) {
        const int start = reverse ? h.right : h.left;
        const int count = reverse ? h.rightCount : h.leftCount;
        const int otherStart = reverse ? h.left : h.right;
        const int otherCount = reverse ? h.leftCount : h.rightCount;
        while (line < start) result += base[line++];
        for (int offset = 0; offset < h.rows; ++offset) {
            if (h.row + offset >= first && h.row + offset <= last) {
                if (offset < otherCount) result += other[otherStart + offset];
            } else if (offset < count) {
                result += base[start + offset];
            }
        }
        line = start + count;
    }
    while (line < base.size()) result += base[line++];
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
    const auto append=[&](char sign,const QByteArray& line) { out+=sign; out+=line; if(!line.endsWith('\n')) out+="\n\\ No newline at end of file\n"; };
    constexpr int context = 3;
    for (qsizetype first = 0; first < m.hunks.size();) {
        const auto contextEnd = [&](const Hunk& h) { return std::min(m.rowCount, h.row + h.rows + context); };
        const int start = std::max(0, m.hunks[first].row - context);
        int end = contextEnd(m.hunks[first]);
        qsizetype next = first + 1;
        while (next < m.hunks.size() && m.hunks[next].row - context <= end)
            end = contextEnd(m.hunks[next++]);
        int oldLine = m.alignment.left.lineAtOrAfter(start);
        const int newLine = m.alignment.right.lineAtOrAfter(start);
        const int oldEnd = m.alignment.left.lineAtOrAfter(end);
        const int oldCount = oldEnd - oldLine;
        const int newCount = m.alignment.right.lineAtOrAfter(end) - newLine;
        out+="@@ -"+QByteArray::number(oldLine+(oldCount?1:0))+","+QByteArray::number(oldCount)+" +"+QByteArray::number(newLine+(newCount?1:0))+","+QByteArray::number(newCount)+" @@\n";
        for (; first < next; ++first) {
            const auto& h = m.hunks[first];
            while (oldLine < h.left) append(' ', m.left[oldLine++]);
            while (oldLine < h.left + h.leftCount) append('-', m.left[oldLine++]);
            for (int line = h.right; line < h.right + h.rightCount; ++line) append('+', m.right[line]);
        }
        while (oldLine < oldEnd) append(' ', m.left[oldLine++]);
    }
    return out;
}
inline QString displayText(const QByteArray& text) { auto s=QString::fromUtf8(text); s.replace("\r\n","\n"); return s; }
inline QString displayLine(const QByteArray& bytes) {
    auto text = displayText(bytes);
    if (text.endsWith('\n')) text.chop(1);
    return text;
}
inline std::pair<int, int> commonColumns(QStringView left, QStringView right) {
    int prefix = 0, suffix = 0;
    const auto length = std::min(left.size(), right.size());
    while (prefix < length && left[prefix] == right[prefix]) ++prefix;
    while (suffix < length - prefix && left[left.size() - suffix - 1] == right[right.size() - suffix - 1]) ++suffix;
    if (prefix > 0 && left[prefix - 1].isHighSurrogate()) --prefix;
    if (suffix > 0 && left[left.size() - suffix].isLowSurrogate()) --suffix;
    return {prefix, suffix};
}
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
