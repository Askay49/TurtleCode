#include "EditorWidget.hpp"

#include <QFontMetricsF>
#include <QScrollBar>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <algorithm>

bool EditorWidget::screenLayoutActive() const {
    return diffAlignment_.isEmpty() && vim_.enabled() &&
           (vimWrap_ || (vimFoldEnabled_ && !vimFolds_.empty()));
}

std::pair<int,int> EditorWidget::foldLines(const VimFold& fold) const {
    return {int(buffer_.lineIndexAt(std::min(fold.first->offset, buffer_.size()))),
            int(buffer_.lineIndexAt(std::min(fold.last->offset, buffer_.size())))};
}

void EditorWidget::adjustScreenWraps(std::size_t row,std::size_t removed,std::size_t inserted) {
    if (screenWrapFull_) return;
    screenLayoutDirty_=true;
    // Keep unchanged wrap boundaries. A newline edit shifts the keys that
    // follow the affected lines, including lines awaiting remeasurement.
    const int first=int(row), last=int(row+removed), shift=int(inserted)-int(removed);
    auto wrap=screenWraps_.lower_bound(first);
    wrap=screenWraps_.erase(wrap,screenWraps_.upper_bound(last));
    auto dirty=screenDirtyLines_.lower_bound(first);
    dirty=screenDirtyLines_.erase(dirty,screenDirtyLines_.upper_bound(last));
    if (shift) {
        decltype(screenWraps_) shifted;
        while (wrap!=screenWraps_.end()) {
            auto node=screenWraps_.extract(wrap++);
            node.key()+=shift;
            shifted.insert(shifted.end(),std::move(node));
        }
        screenWraps_.merge(shifted);
        decltype(screenDirtyLines_) shiftedDirty;
        while (dirty!=screenDirtyLines_.end()) {
            auto node=screenDirtyLines_.extract(dirty++);
            node.value()+=shift;
            shiftedDirty.insert(shiftedDirty.end(),std::move(node));
        }
        screenDirtyLines_.merge(shiftedDirty);
    }
    if (inserted>2048) {
        screenWrapFull_=true;
        screenDirtyLines_.clear();
    } else {
        for (std::size_t n=0;n<=inserted;++n) screenDirtyLines_.insert(first+int(n));
    }
}

void EditorWidget::rebuildScreenWraps(int width) const {
    const QFontMetricsF metrics(font());
    const auto measure=[&](int line,std::string_view bytes) {
        if (bytes.empty()) return;
        if (bytes.back()=='\r') bytes.remove_suffix(1);
        const auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));
        int column=0;
        const auto display=expandedTabs(text,column);
        if (metrics.horizontalAdvance(display)<=width) return;

        // Match the glyph positions used by whole-line painting and break
        // only at grapheme boundaries, including tabs and combining marks.
        QTextLayout layout(display,font());
        layout.beginLayout();
        const auto shaped=layout.createLine();
        layout.endLayout();
        if (!shaped.isValid()) return;
        QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme,text);
        QVector<WrapBreak> breaks;
        int segmentX=0, displayOffset=0;
        std::size_t byte=0;
        qsizetype previous=0;
        column=0;
        for (auto next=boundaries.toNextBoundary();next>=0;next=boundaries.toNextBoundary()) {
            const auto cluster=text.mid(previous,next-previous);
            const int startX=qRound(shaped.cursorToX(displayOffset));
            displayOffset+=expandedTabs(cluster,column).size();
            const int endX=qRound(shaped.cursorToX(displayOffset));
            if (startX>segmentX && endX-segmentX>width) {
                breaks.append({byte,startX});
                segmentX=startX;
            }
            byte+=std::size_t(cluster.toUtf8().size());
            previous=next;
        }
        if (!breaks.isEmpty()) screenWraps_.insert_or_assign(line,std::move(breaks));
    };
    if (screenWrapFull_ || width!=screenLayoutWidth_) {
        screenWraps_.clear();
        int line=0;
        buffer_.forEachLineBounded(64*1024,[&](std::size_t,std::string_view bytes,bool truncated) {
            if (!truncated) measure(line,bytes);
            return ++line<INT_MAX-1;
        });
    } else {
        for (const auto line : screenDirtyLines_) {
            if (std::size_t(line)>=buffer_.lineCount()) continue;
            const auto length=buffer_.lineEnd(line)-buffer_.lineStart(line);
            if (length<=64*1024) measure(line,buffer_.line(line));
        }
    }
    screenDirtyLines_.clear();
    screenWrapFull_=false;
}

void EditorWidget::rebuildScreenLayout() const {
    const int width=std::max(1,textAreaWidth()-8);
    if (!screenLayoutDirty_ && screenLayoutWidth_==width) return;
    if (vimWrap_) rebuildScreenWraps(width);
    else if (screenLayoutWidth_!=width) screenWrapFull_=true;
    screenLayoutDirty_=false;
    screenLayoutWidth_=width;
    screenSpans_.clear();

    std::vector<std::pair<int,int>> closed;
    if (vimFoldEnabled_) {
        for (const auto& fold : vimFolds_) {
            if (!fold.closed) continue;
            const auto [first,last]=foldLines(fold);
            if (first<last) closed.emplace_back(first,last);
        }
        std::sort(closed.begin(),closed.end());
    }
    const int lines=int(std::min<std::size_t>(buffer_.lineCount(),INT_MAX-1));
    int delta=0;
    auto wrap=screenWraps_.begin();
    for (auto fold=closed.begin();fold!=closed.end();) {
        const int first=fold->first;
        int last=fold++->second;
        while (fold!=closed.end() && fold->first<=last) last=std::max(last,fold++->second);
        if (vimWrap_) {
            while (wrap!=screenWraps_.end() && wrap->first<first) {
                const auto& [line,breaks]=*wrap++;
                if (breaks.size()>INT_MAX-1-lines-delta) continue;
                screenSpans_.push_back({line,line,line+delta,line+delta+int(breaks.size()),breaks});
                delta+=int(breaks.size());
            }
            wrap=screenWraps_.upper_bound(last);
        }
        screenSpans_.push_back({first,last,first+delta,first+delta,{}});
        delta-=last-first;
    }
    if (vimWrap_) {
        for (;wrap!=screenWraps_.end();++wrap) {
            const auto& [line,breaks]=*wrap;
            if (breaks.size()>INT_MAX-1-lines-delta) continue;
            screenSpans_.push_back({line,line,line+delta,line+delta+int(breaks.size()),breaks});
            delta+=int(breaks.size());
        }
    }
    screenRowCount_=lines+delta;
}

int EditorWidget::screenRowCount() const {
    if (!diffAlignment_.isEmpty()) return diffAlignment_.rowCount();
    if (!screenLayoutActive()) return int(std::min<std::size_t>(buffer_.lineCount(), INT_MAX-1));
    rebuildScreenLayout();
    return screenRowCount_;
}

int EditorWidget::visualRowForLine(int line) const {
    if (const int row = diffAlignment_.rowForLine(line); row >= 0) return row;
    if (!screenLayoutActive()) return line;
    rebuildScreenLayout();
    const auto span = std::lower_bound(screenSpans_.begin(),screenSpans_.end(),line,
        [](const ScreenSpan& span,int line){return span.lastLine < line;});
    if (span != screenSpans_.end() && line >= span->firstLine) return span->firstRow;
    if (span == screenSpans_.begin()) return line;
    const auto& previous = *std::prev(span);
    return previous.lastRow + (line - previous.lastLine);
}

int EditorWidget::documentLineForVisualRow(int row) const {
    if (!diffAlignment_.isEmpty()) {
        const int lines = diffAlignment_.lineCount();
        if (row >= diffAlignment_.rowCount()) return lines;
        return std::min(diffAlignment_.lineAtOrAfter(row), std::max(0, lines - 1));
    }
    if (!screenLayoutActive()) return row;
    rebuildScreenLayout();
    const auto span = std::lower_bound(screenSpans_.begin(),screenSpans_.end(),row,
        [](const ScreenSpan& span,int row){return span.lastRow < row;});
    if (span != screenSpans_.end() && row >= span->firstRow) return span->firstLine;
    if (span == screenSpans_.begin()) return row;
    const auto& previous = *std::prev(span);
    return previous.lastLine + (row - previous.lastRow);
}

EditorWidget::ScreenSlice EditorWidget::screenSlice(int row) const {
    const int line = std::clamp(documentLineForVisualRow(row),0,int(buffer_.lineCount()-1));
    const auto start = buffer_.lineStart(line);
    ScreenSlice slice{line,start,lineContentEnd(line)};
    if (!screenLayoutActive() || !vimWrap_) return slice;
    const auto span = std::lower_bound(screenSpans_.begin(),screenSpans_.end(),row,
        [](const ScreenSpan& span,int row){return span.lastRow < row;});
    if (span == screenSpans_.end() || row < span->firstRow || span->breaks.isEmpty()) return slice;
    const int part = row - span->firstRow;
    if (part) {
        slice.begin += span->breaks[part-1].byte;
        slice.left = span->breaks[part-1].pixel;
    }
    if (part < span->breaks.size()) {
        slice.end = start + span->breaks[part].byte;
        slice.right = span->breaks[part].pixel;
    }
    return slice;
}

int EditorWidget::screenRowForPosition(std::size_t position) const {
    const int line = int(buffer_.lineIndexAt(position));
    const int row = visualRowForLine(line);
    if (!screenLayoutActive() || !vimWrap_) return row;
    const auto span = std::lower_bound(screenSpans_.begin(),screenSpans_.end(),row,
        [](const ScreenSpan& span,int row){return span.lastRow < row;});
    if (span == screenSpans_.end() || span->firstLine != line) return row;
    const auto byte = position - buffer_.lineStart(line);
    const auto part = std::upper_bound(span->breaks.begin(),span->breaks.end(),byte,
        [](std::size_t byte,const WrapBreak& br){return byte < br.byte;});
    return row + int(part - span->breaks.begin());
}

int EditorWidget::screenOriginX(std::size_t position) const {
    if (!screenLayoutActive() || !vimWrap_) return horizontalScrollBar()->value();
    const auto slice = screenSlice(screenRowForPosition(position));
    if (slice.begin != buffer_.lineStart(slice.line) || slice.right >= 0) return slice.left;
    return horizontalScrollBar()->value();
}

std::size_t EditorWidget::screenPositionAtX(int row, int x, bool insert) const {
    const auto slice = screenSlice(row);
    const auto start = buffer_.lineStart(slice.line);
    const auto offset = byteColumnAtX(gutterWidth()+12+x+slice.left-horizontalScrollBar()->value(),slice.line);
    const auto last = slice.end > slice.begin && (!insert || slice.right >= 0)
        ? graphemeBoundaryAt(slice.end,false) : slice.end;
    return std::clamp(start+offset,slice.begin,std::max(slice.begin,last));
}

std::size_t EditorWidget::positionAtPoint(int x,int y,int line) const {
    if (screenLayoutActive() && vimWrap_) {
        const int row = verticalScrollBar()->value()+std::max(0,y)/lineHeight();
        const auto slice = screenSlice(row);
        if (slice.left || slice.right >= 0)
            return screenPositionAtX(row,x-gutterWidth()-12,true);
    }
    return buffer_.lineStart(line)+byteColumnAtX(x,line);
}

int EditorWidget::closedFoldEnd(int line) const {
    if (!screenLayoutActive() || !vimFoldEnabled_ || vimFolds_.empty()) return line;
    rebuildScreenLayout();
    const auto span = std::lower_bound(screenSpans_.begin(),screenSpans_.end(),line,
        [](const ScreenSpan& span,int line){return span.lastLine < line;});
    if (span != screenSpans_.end() && span->firstLine <= line && span->breaks.isEmpty())
        return span->lastLine;
    return line;
}

bool EditorWidget::revealVimFolds(int line) {
    if (!screenLayoutActive() || !vimFoldEnabled_ || vimFolds_.empty()) return false;
    bool opened = false;
    for (std::size_t i=0;i<vimFolds_.size();++i) {
        auto& fold=vimFolds_[i];
        const auto [first,last] = foldLines(fold);
        if (fold.closed && first < line && line <= last) {
            if (vimExActive_ && vimCommandPrompt_!=':') previewOpenedFolds_.push_back(i);
            fold.closed = false;
            opened = true;
        }
    }
    if (opened) screenLayoutDirty_ = true;
    return opened;
}

void EditorWidget::restoreVimPreviewFolds() {
    if (previewOpenedFolds_.empty()) return;
    for (const auto i:previewOpenedFolds_) vimFolds_[i].closed=true;
    previewOpenedFolds_.clear();
    screenLayoutDirty_=true;
    updateScrollBars();
    viewport()->update();
}

void EditorWidget::refreshScreenLayout(int topLine) {
    screenLayoutDirty_ = true;
    invalidateScreenMinimap();
    updateScrollBars();
    verticalScrollBar()->setValue(visualRowForLine(topLine));
    viewport()->update();
    updateVimSearchHighlights();
    emit semanticViewportChanged();
}
