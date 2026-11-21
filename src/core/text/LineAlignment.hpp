#pragma once

#include <QVector>
#include <algorithm>
#include <utility>

// A document's projection onto comparison rows. Consecutive lines share a
// range, and spaces between ranges are padding with no document line.
class LineAlignment {
public:
    struct Range {
        int row, line, count;
        bool operator==(const Range&) const = default;
    };
    struct Anchor { int line, offset; };

    bool isEmpty() const { return rows_ == 0; }
    int rowCount() const { return rows_; }
    int lineCount() const { return lines_; }
    const QVector<Range>& ranges() const { return ranges_; }
    bool operator==(const LineAlignment&) const = default;

    void appendLines(int count) {
        if (count == 0) return;
        if (!ranges_.isEmpty() && ranges_.constLast().row + ranges_.constLast().count == rows_)
            ranges_.last().count += count;
        else
            ranges_.append({rows_, lines_, count});
        rows_ += count;
        lines_ += count;
    }

    void appendPadding(int count) { rows_ += count; }

    int rowForLine(int line) const {
        if (line < 0 || line >= lines_) return -1;
        const auto range = std::lower_bound(ranges_.cbegin(), ranges_.cend(), line,
            [](const Range& range, int line) { return range.line + range.count <= line; });
        return range->row + line - range->line;
    }

    int lineAt(int row) const {
        const auto range = rangeAtOrAfter(row);
        return range != ranges_.cend() && row >= range->row ? range->line + row - range->row : -1;
    }

    // At the end of a pane, return the line count when no real line follows.
    int lineAtOrAfter(int row) const {
        const auto range = rangeAtOrAfter(row);
        return range == ranges_.cend() ? lines_ : range->line + std::max(0, row - range->row);
    }

    Anchor anchorAt(int row) const {
        if (lines_ == 0) return {0, 0};
        const int line = std::min(lineAtOrAfter(row), lines_ - 1);
        return {line, row - rowForLine(line)};
    }

    int rowForAnchor(Anchor anchor) const {
        if (lines_ == 0) return 0;
        const int line = std::min(anchor.line, lines_ - 1);
        const int row = rowForLine(line);
        const int before = line > 0 ? rowForLine(line - 1) + 1 : 0;
        const int after = line + 1 < lines_ ? rowForLine(line + 1) - 1 : rows_ - 1;
        return std::clamp(row + anchor.offset, before, after);
    }

    void projectEdit(int firstLine, int removedLines, int insertedLines) {
        const int prefixEnd = rowForLine(firstLine) + 1;
        const int suffixLine = firstLine + removedLines + 1;
        const int suffixBegin = suffixLine < lines_ ? rowForLine(suffixLine) : rows_;
        // Removed lines leave padding. New lines reuse the whole gap after
        // the edited line before adding any extra comparison rows.
        LineAlignment updated;
        updated.appendRows(*this, 0, prefixEnd);
        updated.appendLines(insertedLines);
        updated.appendPadding(std::max(0, suffixBegin - prefixEnd - insertedLines));
        updated.appendRows(*this, suffixBegin, rows_);
        *this = std::move(updated);
    }

private:
    QVector<Range>::const_iterator rangeAtOrAfter(int row) const {
        return std::lower_bound(ranges_.cbegin(), ranges_.cend(), row,
            [](const Range& range, int row) { return range.row + range.count <= row; });
    }

    void appendRows(const LineAlignment& source, int begin, int end) {
        int row = begin;
        for (auto range = source.rangeAtOrAfter(begin); range != source.ranges_.cend() && range->row < end; ++range) {
            const int start = std::max(row, range->row);
            appendPadding(start - row);
            row = std::min(end, range->row + range->count);
            appendLines(row - start);
        }
        appendPadding(end - row);
    }

    int rows_{0}, lines_{0};
    QVector<Range> ranges_;
};
