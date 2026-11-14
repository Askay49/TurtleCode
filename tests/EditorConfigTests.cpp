#include "editor/EditorConfig.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cassert>
#include <stdexcept>

static void write(const QString& path,const QByteArray& bytes) {
    QFile file(path);assert(file.open(QIODevice::WriteOnly));assert(file.write(bytes)==bytes.size());
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    QTemporaryDir root;assert(root.isValid());
    assert(QDir().mkpath(root.filePath("project/sub")));
    write(root.filePath(".editorconfig"),"root=true\n[*]\nindent_size=8\n");
    const auto config=root.filePath("project/.editorconfig");
    write(config,"root=true\n[*]\nindent_style=space\nindent_size=4\nend_of_line=lf\n"
                 "[*.{json,yml}]\nindent_size=2\n[sub/file{1..3}.cpp]\nindent_size=3\n");
    auto rules=EditorConfig::resolve(root.filePath("project/data.json"));
    assert(rules.warning.isEmpty() && rules.indentation()==2 && rules.tabs==false && rules.eol=="\n");
    assert(EditorConfig::resolve(root.filePath("project/sub/file2.cpp")).indentation()==3);
    assert(EditorConfig::resolve(root.filePath("project/sub/file8.cpp")).indentation()==4);
    write(root.filePath("project/sub/.editorconfig"),"[*]\nindent_size=unset\nend_of_line=unset\n"
          "indent_style=tab\ntab_width=6\n");
    rules=EditorConfig::resolve(root.filePath("project/sub/main.cpp"));
    assert(rules.tabs==true && rules.tabSize()==6 && !rules.eol);
    write(config,"root=true\n[*]\nindent_size=99999\ntab_width=0\nindent_style=unknown\nend_of_line=unknown\n");
    rules=EditorConfig::resolve(root.filePath("project/test.txt"));
    assert(!rules.indentSize && !rules.tabWidth && !rules.tabs && !rules.eol && rules.indentation()==4);
    write(config,"root=true\n[bad section\nindent_size=2\n");
    assert(!EditorConfig::resolve(root.filePath("project/test.txt")).warning.isEmpty());
    write(config,"root=true\n[*]\ncharset=utf-16le\nend_of_line=cr\n");
    rules=EditorConfig::resolve(root.filePath("project/test.txt"));
    assert(!rules.warning.isEmpty() && !rules.bom && !rules.eol);

    const std::string original="\xef\xbb\xbf" "α \t\r\nβ  \r\ntail \t";
    PieceTreeBuffer buffer(original);EditHistory history;
    EditorConfig::Settings settings;settings.bom=false;settings.trimWhitespace=true;
    settings.eol="\n";settings.finalNewline=true;
    const auto cursor=EditorConfig::normalize(buffer,history,{original.size()},settings);
    assert(buffer.text()=="α\nβ\ntail\n" && cursor.cursorPosition==buffer.size());
    assert(history.canUndo() && history.isModified());
    auto undone=history.undo(buffer);assert(undone && buffer.text()==original);
    assert(!history.isModified());
    assert(history.redo(buffer) && buffer.text()=="α\nβ\ntail\n");
    const auto state=history.stateId();
    EditorConfig::normalize(buffer,history,{buffer.size()},settings);
    assert(history.stateId()==state); // No extra Undo action for an already clean save.
    settings.bom=true;settings.eol="\r\n";
    EditorConfig::normalize(buffer,history,{0},settings);
    assert(buffer.text()=="\xef\xbb\xbf" "α\r\nβ\r\ntail\r\n");
    settings.finalNewline=false;
    EditorConfig::normalize(buffer,history,{buffer.size()},settings);
    assert(buffer.text()=="\xef\xbb\xbf" "α\r\nβ\r\ntail");

    for(const std::string text:{"", "\xef\xbb\xbf", " \t", "\r\n\r\n", "foo\r\n \t\r\n \t"}) {
        PieceTreeBuffer item(text);EditHistory edits;
        EditorConfig::normalize(item,edits,{0},settings);
        assert(item.text().empty() || item.text().back()!='\n');
        if(edits.canUndo()){assert(edits.undo(item));assert(item.text()==text);}
    }
    // A long whitespace run crossing storage/chunk boundaries stays disk-backed
    // in history and can be restored in one action.
    const std::string large="start"+std::string(200000,' ')+"\r\nend\t";
    PieceTreeBuffer largeBuffer(large);EditHistory largeHistory;
    settings.bom.reset();settings.finalNewline=true;settings.eol="\n";
    EditorConfig::normalize(largeBuffer,largeHistory,{large.size()},settings);
    assert(largeBuffer.text()=="start\nend\n");
    assert(largeHistory.undo(largeBuffer) && largeBuffer.text()==large);
    // Cancellation runs on a stage; the original remains untouched.
    auto staged=largeBuffer.snapshot();EditHistory stagedHistory;
    bool cancelled=false;
    try { EditorConfig::normalize(staged,stagedHistory,{0},settings,[](int){throw std::runtime_error("cancel");}); }
    catch(const std::runtime_error&){cancelled=true;}
    assert(cancelled && largeBuffer.text()==large && !stagedHistory.canUndo());
    // Files without EditorConfig keep their bytes, BOM, whitespace and EOLs.
    PieceTreeBuffer untouched(original);EditHistory untouchedHistory;
    EditorConfig::normalize(untouched,untouchedHistory,{0},{});
    assert(untouched.text()==original && !untouchedHistory.canUndo());
}
