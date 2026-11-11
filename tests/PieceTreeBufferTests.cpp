#include "core/text/PieceTreeBuffer.hpp"

#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include "core/text/EditHistory.hpp"
#include <fstream>
#include <algorithm>
#include <cstring>
#include <unistd.h>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void testBasicEdits() {
    PieceTreeBuffer buffer("alpha\nbeta");
    check(buffer.size() == 10, "initial byte size");
    check(buffer.lineCount() == 2, "initial line count");
    check(buffer.line(1) == "beta", "read second line");
    buffer.insert(5, "!\nnew");
    check(buffer.text() == "alpha!\nnew\nbeta", "insert in middle");
    check(buffer.lineCount() == 3, "line count after insert");
    buffer.erase(5, 5);
    check(buffer.text() == "alpha\nbeta", "erase inserted range");
    buffer.erase(0, 1000);
    check(buffer.text().empty(), "erase clamps to end");
    check(buffer.lineCount() == 1, "empty buffer has one logical line");
}
void testUnicodeAndBoundaries() {
    const std::string value = "A\xF0\x9F\x8C\x8D" "B"; // A🌍B
    PieceTreeBuffer buffer(value);
    check(utf8NextBoundary(value, 1) == 5, "advance over four-byte code point");
    check(utf8PreviousBoundary(value, 5) == 1, "retreat over four-byte code point");
    buffer.insert(5, "!");
    check(buffer.text() == "A\xF0\x9F\x8C\x8D" "!B", "unicode-safe insertion at boundary");
}
void testRandomizedAgainstString() {
    std::mt19937 rng(123456u);
    PieceTreeBuffer buffer;
    std::string reference;
    for (int i = 0; i < 4000; ++i) {
        if (reference.empty() || (rng() % 3u) != 0u) {
            const std::size_t pos = reference.empty() ? 0 : rng() % (reference.size() + 1);
            const std::string value = (rng() % 5u == 0u) ? "x\ny" : "abc";
            buffer.insert(pos, value);
            reference.insert(pos, value);
        } else {
            const std::size_t pos = rng() % reference.size();
            const std::size_t count = rng() % 12u;
            buffer.erase(pos, count);
            reference.erase(pos, count);
        }
        if (buffer.text() != reference) { check(false, "randomized edits match std::string"); return; }
        std::size_t lines = 1;
        for (char c : reference) if (c == '\n') ++lines;
        if (buffer.lineCount() != lines) { check(false, "randomized line count"); return; }
        std::size_t lineNo = 0;
        std::size_t start = 0;
        for (std::size_t pos = 0; pos <= reference.size(); ++pos) {
            if (buffer.lineIndexAt(pos) != lineNo) { check(false, "line index at byte offset"); return; }
            if (pos < reference.size() && reference[pos] == '\n') {
                if (buffer.lineStart(lineNo) != start) { check(false, "line start offset"); return; }
                if (buffer.line(lineNo) != reference.substr(start, pos - start)) { check(false, "line contents"); return; }
                ++lineNo;
                start = pos + 1;
            }
        }
        if (buffer.lineStart(lineNo) != start || buffer.line(lineNo) != reference.substr(start)) {
            check(false, "last line contents"); return;
        }
    }
}
void testSnapshotIsIndependent() {
    PieceTreeBuffer buffer("before\ntext");
    PieceTreeBuffer snapshot = buffer.snapshot();
    buffer.insert(0, "changed ");
    check(snapshot.text() == "before\ntext", "snapshot remains stable after source edit");
    check(buffer.text() == "changed before\ntext", "source remains editable after snapshot");
}
void testSequentialLinesAcrossPieces() {
    PieceTreeBuffer buffer("ab\ncd\n");
    buffer.insert(1, "X");
    buffer.insert(5, "Y");
    std::vector<std::pair<std::size_t, std::string>> lines;
    buffer.forEachLine([&](std::size_t start, std::string_view line) {
        lines.emplace_back(start, line);
        return true;
    });
    check(lines == std::vector<std::pair<std::size_t, std::string>>{
        {0, "aXb"}, {4, "cYd"}, {8, ""}}, "sequential line offsets across pieces");
    std::size_t visited = 0;
    buffer.forEachLine([&](std::size_t, std::string_view) {
        ++visited;
        return false;
    });
    check(visited == 1, "sequential line scan can stop early");
}
void testLiveOnlySnapshotsAndClear() {
    PieceTreeBuffer buffer("keep\nмир\n");
    buffer.insert(0,std::string(512*1024,'x')); buffer.erase(0,512*1024);
    check(buffer.storageBytes()>512*1024,"small discarded arena remains below compaction threshold");
    auto copy=buffer.snapshot();
    check(copy.text()==buffer.text() && copy.storageBytes()<1024,"snapshot copies only referenced text");
    buffer.clear(); check(buffer.storageBytes()<1024 && buffer.size()==0,"clear releases string capacity");
    check(copy.line(1)=="мир","compacted snapshot keeps line and UTF-8 semantics");
}

void testChunkedFiles() {
    char name[]="/tmp/turtle-code-chunk-test-XXXXXX";const int descriptor=mkstemp(name);
    check(descriptor>=0,"create chunked test input"); if(descriptor<0)return;close(descriptor);
    const std::string path=name;
    std::string original="\xEF\xBB\xBF";
    const std::string line="строка 😀 \r\n";
    while(original.size()<5u*1024u*1024u)original+=line;
    original+="tail without newline";
    {std::ofstream file(path,std::ios::binary);file.write(original.data(),original.size());}
    PieceTreeBuffer buffer;std::string error;
    check(buffer.loadFile(path,&error),"large file loads through disk backing");
    check(buffer.isFileBacked() && buffer.size()==original.size(),"large original is not copied into resident string storage");
    check(buffer.storageBytes()<1024 && buffer.cachedFileBytes()==0,"index construction does not fill the text cache");
    for(std::size_t offset:{0u,4094u,4096u,65530u,65536u,2u*1024u*1024u,4u*1024u*1024u})
        check(buffer.slice(offset,111)==original.substr(offset,111),"bytes remain exact across piece and page boundaries");
    for(std::size_t offset=0;offset<original.size();offset+=65536)check(buffer.byteAt(offset)==original[offset],"random page access reads exact bytes");
    check(buffer.cachedFileBytes()<=2u*1024u*1024u,"file text cache stays within two MiB");
    const auto lines=static_cast<std::size_t>(std::count(original.begin(),original.end(),'\n'))+1;
    check(buffer.lineCount()==lines,"chunked newline index matches original");
    check(buffer.line(0)==std::string("\xEF\xBB\xBF")+line.substr(0,line.size()-1),"UTF-8 BOM and CRLF survive loading");
    check(buffer.line(lines-1)=="tail without newline","far-end line is addressable without materializing all text");
    const auto boundary=original.find("строка",65530);
    check(buffer.lineIndexAt(boundary)==(boundary-3)/line.size(),"line lookup uses chunk index");
    auto snapshot=buffer.snapshot();
    check(snapshot.isFileBacked() && snapshot.storageBytes()<1024,"file-backed snapshot shares immutable original rather than loading it");
    EditHistory history;
    buffer.erase(boundary,line.size());buffer.insert(boundary,"новая 😀\r\n");
    history.record(boundary,line,"новая 😀\r\n",boundary,boundary,EditKind::Standalone,0);
    check(history.undo(buffer).has_value() && buffer.slice(boundary,line.size())==line,"undo restores chunk-backed text");
    check(history.redo(buffer).has_value() && buffer.slice(boundary,strlen("новая 😀\r\n"))=="новая 😀\r\n","redo works across disk and added pieces");
    // Mutating the original path cannot change an open immutable snapshot.
    {std::ofstream file(path,std::ios::trunc|std::ios::binary);file<<"external replacement";}
    check(snapshot.slice(65530,100)==original.substr(65530,100),"external truncation cannot invalidate snapshot text");
    buffer.clear();
    check(snapshot.slice(original.size()-20,20)==original.substr(original.size()-20),"snapshot owns backing after original document closes");
    std::string streamed;
    const bool complete=snapshot.forEachChunk([&](std::string_view bytes){check(bytes.size()<=4096,"streamed original pieces stay bounded");streamed.append(bytes);return true;});
    check(complete && streamed==original,"stream output preserves all bytes including mixed UTF-8 boundaries");
    int visited=0;check(!snapshot.forEachChunk([&](std::string_view){++visited;return false;}) && visited==1,"stream output propagates writer failure immediately");
    PieceTreeBuffer unchanged("keep");int polls=0;
    {std::ofstream file(path,std::ios::binary);file.write(original.data(),original.size());}
    check(!unchanged.loadFile(path,&error,[&]{return ++polls>=3;}) && unchanged.text()=="keep","cancellation leaves prior buffer unchanged");
    const auto* configured=std::getenv("TMPDIR");const std::string oldTemp=configured?configured:"";const bool hadTemp=configured!=nullptr;
    setenv("TMPDIR","/path-that-does-not-exist/turtle-code",1);
    check(!unchanged.loadFile(path,&error) && unchanged.text()=="keep","temporary storage failure preserves existing document");
    if(hadTemp)setenv("TMPDIR",oldTemp.c_str(),1);else unsetenv("TMPDIR");
    check(!unchanged.loadFile(path+".missing",&error) && !error.empty() && unchanged.text()=="keep","failed load does not replace text");
    unlink(path.c_str());
    check(snapshot.byteAt(original.size()-1)==original.back(),"snapshot survives source path deletion");
}

void testDiskInsertsAndCodePointIndex() {
    const std::string unicode="a😀м\n";PieceTreeBuffer buffer(unicode);
    check(buffer.codePointsBefore(unicode.size())==4,"code point index counts multibyte characters");
    check(buffer.byteAtCodePoint(1)==1 && buffer.byteAtCodePoint(2)==5 && buffer.byteAtCodePoint(4)==unicode.size(),"code point navigation returns UTF-8 boundaries");
    check(buffer.utf16Before(unicode.size())==5 && buffer.byteAtUtf16(1)==1 && buffer.byteAtUtf16(2)==1 && buffer.byteAtUtf16(3)==5,"UTF-16 index preserves surrogate boundaries");
    std::string pasted(2*1024*1024,'x');buffer.insert(1,pasted);
    check(buffer.storageBytes()<1024,"large insertion is stored on disk");
    check(buffer.codePointsBefore(1+pasted.size())==1+pasted.size(),"large disk insert updates code point metadata");
    auto snapshot=buffer.snapshot();buffer.erase(1,pasted.size());
    check(buffer.text()==unicode && buffer.codePointsBefore(buffer.size())==4,"disk deletion restores Unicode indexes");
    check(snapshot.utf16Before(snapshot.size())==5+pasted.size(),"UTF-16 metadata survives a disk-backed snapshot");
    check(snapshot.slice(1,10)==std::string(10,'x'),"snapshots retain immutable disk insert bytes");
    std::string small(500*1024,'s');for(int i=0;i<10;++i)buffer.insert(1,small);
    check(buffer.storageBytes()<5*1024*1024,"accumulated small inserts spill without an unbounded append arena");
    bool preview=false;char name[]="/tmp/blink-progress-test-XXXXXX";int fd=mkstemp(name);
    const std::string file(5*1024*1024,'p');write(fd,file.data(),file.size());close(fd);
    PieceTreeBuffer loaded;std::string error;
    check(loaded.loadFile(name,&error,{},[&](std::size_t done,std::size_t total,std::string_view prefix){if(!prefix.empty()){preview=done<total && prefix.size()<=65536;}}) && preview,"file loading reports an initial bounded preview before full indexing");
    unlink(name);
    std::size_t longest=0;bool truncated=false;loaded.forEachLineBounded(1024,[&](std::size_t,std::string_view line,bool clipped){longest=std::max(longest,line.size());truncated|=clipped;return true;});
    check(longest<=1024 && truncated,"bounded line scanning does not allocate a whole giant line");
}

}
int main() {
    testLiveOnlySnapshotsAndClear();
    testChunkedFiles();
    testDiskInsertsAndCodePointIndex();
    testBasicEdits();
    testUnicodeAndBoundaries();
    testRandomizedAgainstString();
    testSnapshotIsIndependent();
    testSequentialLinesAcrossPieces();
    if (failures) return EXIT_FAILURE;
    std::cout << "All PieceTreeBuffer tests passed.\n";
    return EXIT_SUCCESS;
}
