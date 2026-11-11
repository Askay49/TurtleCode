#pragma once
#include <QString>
#include <QStringView>
#include <algorithm>
#include <cstddef>

// Project the last rendered spans onto edited text while a fresh parse is
// pending. Deleted characters lose their spans; surviving text keeps its color.
// These spans are display-only and never fed back into the parser's valid cache.
struct HighlightEdit {
    std::size_t row,oldEndRow,newEndRow;
    int column,oldEndColumn,newEndColumn;
    HighlightEdit(std::size_t first,int col,const QString& removed,const QString& inserted)
        : row(first),oldEndRow(first),newEndRow(first),column(col),oldEndColumn(col),newEndColumn(col) {
        auto end=[](const QString& text,std::size_t& row,int& column) {
            for(auto ch:text) { if(ch=='\n') { ++row; column=0; } else ++column; }
        };
        end(removed,oldEndRow,oldEndColumn); end(inserted,newEndRow,newEndColumn);
    }
    template<class Span,class Publish> void project(std::size_t sourceRow,const Span& span,Publish publish) const {
        const int begin=span.start,end=span.start+span.length;
        auto output=[&](std::size_t target,int a,int b) {
            if(b<=a) return;
            auto copy=span; copy.start=a; copy.length=b-a;
            if constexpr(requires {copy.line=0;}) copy.line=static_cast<int>(target);
            publish(target,std::move(copy));
        };
        if(sourceRow<row) { output(sourceRow,begin,end); return; }
        if(sourceRow>oldEndRow) { output(sourceRow-oldEndRow+newEndRow,begin,end); return; }
        if(sourceRow==row) output(row,begin,std::min(end,column));
        // Typing/replacement strictly inside a token inherits its display style.
        if(row==oldEndRow && row==newEndRow && sourceRow==row && begin<column && end>oldEndColumn)
            output(row,column,newEndColumn);
        if(sourceRow==oldEndRow) {
            const int a=std::max(begin,oldEndColumn);
            output(newEndRow,newEndColumn+a-oldEndColumn,newEndColumn+end-oldEndColumn);
        }
    }
};
