#pragma once
#include <algorithm>
// One-based line anchor through a replacement starting at zero-based row/column.
// Deleted anchors collapse to the replacement boundary; insertion at column zero
// precedes the old line. Independent of Qt, buffers and debugger state.
inline int projectLineAnchor(int line,int row,int column,int removed,int inserted) {
    if(line>row+1+removed) line+=inserted-removed;
    else if(line>row+1 || (line==row+1 && column==0)) line=row+1+inserted;
    return std::max(1,line);
}
