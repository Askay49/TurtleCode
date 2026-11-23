#pragma once
#include <QString>
#include <QStringView>
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Project the last rendered spans onto edited text while a fresh parse is
// pending. Deleted characters lose their spans; surviving text keeps its color.
// These spans are display-only and never fed back into the parser's valid cache.
struct HighlightEdit {
    std::size_t row,oldEndRow,newEndRow;
    int column,oldEndColumn,newEndColumn;
    HighlightEdit(std::size_t first, int col, std::size_t oldRow, int oldColumn, std::size_t newRow, int newColumn)
        : row(first), oldEndRow(oldRow), newEndRow(newRow), column(col), oldEndColumn(oldColumn), newEndColumn(newColumn) {}
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

// Edits applied from the end of the buffer share the original coordinates.
// Map the surviving ranges directly to the final coordinates of the batch.
class HighlightEditBatch {
    using Position = std::pair<std::size_t, int>;
    struct Change {
        Position start, end, finalEnd;
    };
    std::vector<Change> changes_;

    static Position map(Position position, const Change& previous) {
        const auto [row, column] = position;
        return {previous.finalEnd.first + (row - previous.end.first),
                row == previous.end.first ? previous.finalEnd.second + column - previous.end.second : column};
    }

    auto after(Position position) const {
        return std::upper_bound(changes_.begin(), changes_.end(), position,
            [](Position point, const Change& change) { return point < change.end; });
    }

public:
    static bool follows(const HighlightEdit& previous, const HighlightEdit& next) {
        return Position{next.oldEndRow, next.oldEndColumn} <= Position{previous.row, previous.column};
    }

    explicit HighlightEditBatch(std::span<const HighlightEdit> edits) {
        changes_.reserve(edits.size());
        Change previous{};
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            const auto& edit = *it;
            const Position start{edit.row, edit.column};
            const auto [row, column] = map(start, previous);
            const Position end{edit.oldEndRow, edit.oldEndColumn};
            const Position finalEnd{row + (edit.newEndRow - edit.row),
                                    edit.newEndRow == edit.row
                                        ? column + edit.newEndColumn - edit.column : edit.newEndColumn};
            changes_.push_back({start, end, finalEnd});
            previous = changes_.back();
        }
    }

    // An empty cached row can be reused only if no edit touched it.
    std::optional<std::size_t> untouchedRow(std::size_t row) const {
        const Position start{row, 0};
        const auto it = std::lower_bound(changes_.begin(), changes_.end(), start,
            [](const Change& change, Position point) { return change.end < point; });
        if (it != changes_.end() && it->start.first == row) return {};
        if (it != changes_.end() && it->start < start) return {};
        return it == changes_.begin() ? row : map(start, *std::prev(it)).first;
    }

    template<class Span, class Publish>
    void project(std::size_t row, const Span& span, Publish publish) const {
        Position start{row, span.start};
        const Position end{row, span.start + span.length};
        auto it = after(start);
        const auto emitSpan = [&](Position first, Position last) {
            if (last <= first) return;
            const auto [target, column] = it == changes_.begin() ? first : map(first, *std::prev(it));
            auto copy = span;
            copy.start = column;
            copy.length = last.second - first.second;
            if constexpr (requires { copy.line = 0; }) copy.line = static_cast<int>(target);
            publish(target, std::move(copy));
        };
        for (; it != changes_.end() && it->start < end; ++it) {
            emitSpan(start, it->start);
            start = std::max(start, it->end);
            if (start >= end) return;
        }
        emitSpan(start, end);
    }
};
