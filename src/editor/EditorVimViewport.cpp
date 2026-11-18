#include "EditorWidget.hpp"

#include <QScrollBar>
#include <algorithm>

void EditorWidget::createVimFold(int first, int last) {
    if (!diffRows_.isEmpty() || first >= last || vimFolds_.size() >= 2048) return;
    const int top = documentLineForVisualRow(verticalScrollBar()->value());
    // Crossing ranges cannot form a tree. Extend the new parent to contain
    // any range it intersects, retaining those folds as children.
    bool expanded;
    do {
        expanded = false;
        for (const auto& fold : vimFolds_) {
            const auto [begin,end] = foldLines(fold);
            if ((begin < first && first <= end && end < last) ||
                (first < begin && begin <= last && last < end)) {
                first = std::min(first,begin);
                last = std::max(last,end);
                expanded = true;
            }
        }
    } while (expanded);
    if (vim_.mode() == VimController::Mode::Visual) switchVimMode(VimController::Mode::Normal);
    auto& session = VimState::Session::instance();
    auto existing = std::find_if(vimFolds_.begin(),vimFolds_.end(),[&](const VimFold& fold){
        return foldLines(fold) == std::pair{first,last};
    });
    if (existing != vimFolds_.end()) existing->closed = true;
    else vimFolds_.push_back({
        session.anchor(document_.get(),filePath_,buffer_.lineStart(first),QPoint(first+1,1)),
        session.anchor(document_.get(),filePath_,lineContentEnd(last),QPoint(last+1,1)), true
    });
    vimFoldEnabled_ = true;
    refreshScreenLayout(top);
    if (int(currentLine()) >= first && int(currentLine()) <= last)
        moveCursor(vimLineStart(documentLineForVisualRow(visualRowForLine(first))),false);
}

void EditorWidget::editVimFolds(QChar command, int count) {
    if (!diffRows_.isEmpty()) return;
    const int row = int(currentLine());
    const int top = documentLineForVisualRow(verticalScrollBar()->value());
    std::vector<std::size_t> containing;
    for (std::size_t i=0; i<vimFolds_.size(); ++i) {
        const auto [first,last] = foldLines(vimFolds_[i]);
        if (first < last && first <= row && row <= last) containing.push_back(i);
    }
    const auto bounds = [&](std::size_t i){return foldLines(vimFolds_[i]);};
    std::stable_sort(containing.begin(),containing.end(),[&](auto a,auto b){
        const auto [af,al] = bounds(a);
        const auto [bf,bl] = bounds(b);
        return al-af > bl-bf;
    });

    if (command == 'E') vimFolds_.clear();
    else if (command == 'n') vimFoldEnabled_ = false;
    else if (command == 'N') vimFoldEnabled_ = true;
    else if (command == 'i') vimFoldEnabled_ = !vimFoldEnabled_;
    else if (command == 'M' || command == 'R') {
        for (auto& fold : vimFolds_) fold.closed = command == 'M';
        vimFoldEnabled_ = true;
    } else {
        if (containing.empty()) return;
        const auto closed = std::find_if(containing.begin(),containing.end(),
            [&](auto i){ return vimFoldEnabled_ && vimFolds_[i].closed; });
        const auto visible = closed == containing.end() ? containing.back() : *closed;
        const bool toggle = command == 'a' || command == 'A';
        const bool opening = command == 'o' || command == 'O' ||
            (toggle && closed != containing.end());
        const bool recursive = command.isUpper();
        const auto [first,last] = bounds(visible);
        if (command == 'd' || command == 'D') {
            if (!recursive) vimFolds_.erase(vimFolds_.begin()+visible);
            else std::erase_if(vimFolds_,[&](const VimFold& fold){
                const auto [a,b]=foldLines(fold);
                return first<=a && b<=last;
            });
        } else if (recursive) {
            for (auto& fold : vimFolds_) {
                const auto [a,b] = foldLines(fold);
                if (first<=a && b<=last) fold.closed = !opening;
            }
            if (!opening) vimFoldEnabled_ = true;
        } else if (opening) {
            for (auto i : containing) {
                if (vimFolds_[i].closed) {
                    vimFolds_[i].closed = false;
                    if (--count == 0) break;
                }
            }
        } else {
            // Close the nearest open parent, starting with the innermost
            // visible fold. Hidden children do not count as open levels.
            for (auto it=std::make_reverse_iterator(
                    closed == containing.end() ? containing.end() : closed);
                 it != containing.rend(); ++it) {
                auto& fold = vimFolds_[*it];
                if (!fold.closed || !vimFoldEnabled_) {
                    fold.closed = true;
                    if (--count == 0) break;
                }
            }
            vimFoldEnabled_ = true;
        }
    }
    refreshScreenLayout(top);
    const int visibleLine = documentLineForVisualRow(visualRowForLine(row));
    if (visibleLine != row) moveCursor(vimLineStart(visibleLine),
        vim_.mode() == VimController::Mode::Visual);
}

void EditorWidget::moveToVimFold(QChar command,int count) {
    if (!diffRows_.isEmpty()) return;
    const bool forward = command == 'j' || command == ']';
    const bool boundary = command == '[' || command == ']';
    const int row = int(currentLine());
    int target = row;
    for (int step=0; step<count; ++step) {
        std::optional<int> next;
        for (const auto& fold : vimFolds_) {
            const auto [first,last] = foldLines(fold);
            if (first >= last) continue;
            const bool closed = vimFoldEnabled_ && fold.closed;
            if (boundary && (closed || target < first || target > last)) continue;
            if (!boundary && documentLineForVisualRow(visualRowForLine(first)) != first) continue;
            const int edge = boundary ? (forward ? last : first)
                                      : (forward || closed ? first : last);
            if (forward ? edge <= target : edge >= target) continue;
            if (!next || (forward ? edge < *next : edge > *next)) next=edge;
        }
        if (!next) break;
        target=*next;
    }
    if (target == row) return;
    moveCursor(vimLineStart(target),vim_.mode()==VimController::Mode::Visual);
}

bool EditorWidget::handleVimViewport(const VimController::Action& action) {
    using C = VimController::Command;
    const auto command = action.command;
    if (command != C::Scroll && command != C::ZCommand && command != C::FoldJump &&
        (command < C::ScreenHigh || command > C::ScreenMiddleColumn)) return false;
    const bool visual = vim_.mode() == VimController::Mode::Visual;
    const int lastLine = int(VimMotions::lastLine(buffer_,vimFinalNewline_));
    auto* scroll = verticalScrollBar();
    auto* horizontal = horizontalScrollBar();
    const int page = std::max(1,scroll->pageStep());
    const int lastRow = std::max(0,screenRowForPosition(lineContentEnd(lastLine)));
    const int top = scroll->value();
    const int cursorRow = screenRowForPosition(cursorPosition_);

    if (command == C::ZCommand && action.operation == C::Consume) {
        const QChar key = action.character;
        if (QStringLiteral("aocdAOCDMREniN").contains(key)) {
            editVimFolds(key,action.count);
        } else if (key == 'j' || key == 'k') {
            moveToVimFold(key,action.count);
        } else if (key == 'F' || (visual && key == 'f')) {
            const auto [begin,end] = selectionRange();
            const int first = visual ? int(buffer_.lineIndexAt(begin)) : int(currentLine());
            const int last = visual ? int(buffer_.lineIndexAt(end > begin ? end-1 : end))
                                    : std::min(lastLine,first+action.count-1);
            createVimFold(first,last);
        } else if (QStringLiteral("ztb.-+^\r\n").contains(key)) {
            if (action.explicitCount || QStringLiteral(".-+^\r\n").contains(key)) {
                int line = action.explicitCount ? std::min(lastLine,action.count-1) : int(currentLine());
                if (key == '+' && !action.explicitCount)
                    line = screenSlice(std::min(lastRow,top+page)).line;
                if (key == '^' && !action.explicitCount)
                    line = screenSlice(std::max(0,top-1)).line;
                auto target=VimMotions::firstText(buffer_,line);
                if (key=='z' || key=='t' || key=='b')
                    target=screenPositionAtX(visualRowForLine(line),cursorPixelColumn());
                moveCursor(target,visual);
            }
            const int middle = (key == 'z' || key == '.') ? (page-1)/2 : 0;
            const int bottom = (key == 'b' || key == '-' || key == '^') ? page-1 : 0;
            const int desired = std::max(0,screenRowForPosition(cursorPosition_)-middle-bottom);
            vimScrollPadding_ = std::max(0,desired-std::max(0,screenRowCount()-page));
            updateScrollBars();
            scroll->setValue(desired);
        } else if (QStringLiteral("hlHLse").contains(key)) {
            if (vimWrap_ && screenLayoutActive() && horizontal->maximum()==0) return true;
            const int cell = std::max(1,fontMetrics().horizontalAdvance(QLatin1Char(' ')));
            int value = horizontal->value();
            const int x = cursorPixelColumn();
            const int width = std::max(1,textAreaWidth()-8);
            if (key == 's') value=x;
            else if (key == 'e') value=x+vimCaretWidth()-width;
            else {
                const int step = key.isUpper() ? std::max(1,width/2) : cell;
                value += (key.toLower()=='l' ? 1 : -1)*step*action.count;
            }
            horizontal->setValue(value);
            if (key != 's' && key != 'e') {
                const int left = horizontal->value();
                const int target = std::clamp(x,left,left+width-1);
                if (target != x) moveCursor(screenPositionAtX(cursorRow,target),visual);
                horizontal->setValue(value);
            }
        }
        return true;
    }
    if (command == C::FoldJump && action.operation == C::Consume) {
        moveToVimFold(action.character,action.count);
        return true;
    }
    if (command == C::Scroll && action.operation == C::Consume) {
        const auto key = action.character;
        int rows = action.count;
        const bool down = key=='D' || key=='F' || key=='E';
        if (key=='D' || key=='U') {
            if (action.explicitCount) vimHalfPage_=action.count;
            rows = vimHalfPage_ ? vimHalfPage_ : std::max(1,page/2);
        } else if (key=='F' || key=='B') rows *= std::max(1,page-2);
        const int shift = down ? rows : -rows;
        const int normalMaximum=std::max(0,lastRow-page+1);
        // Page forward can leave blank screen rows after EOF. Half-page and
        // single-line scroll stop once the last document row is on screen.
        const int maximum=key=='F' ? lastRow : std::max(top,normalMaximum);
        const int nextTop=int(std::clamp<qint64>(qint64(top)+shift,0,maximum));
        int row = cursorRow;
        if (key=='D' || key=='U') row+=shift;
        if (key=='F') row=nextTop;
        if (key=='B') row=nextTop+page-1;
        row=std::clamp(row,std::min(lastRow,nextTop),std::min(lastRow,nextTop+page-1));
        if (row != cursorRow) {
            auto target=screenPositionAtX(row,cursorPixelColumn()-screenOriginX(cursorPosition_));
            if (key=='F' || key=='B' || key=='D' || key=='U') target=std::max(screenSlice(row).begin,VimMotions::firstText(buffer_,screenSlice(row).line));
            moveCursor(target,visual);
        }
        vimScrollPadding_=std::max(0,nextTop-std::max(0,screenRowCount()-page));
        updateScrollBars();
        scroll->setValue(nextTop);
        return true;
    }

    int row = cursorRow;
    bool linewise = false, inclusive = false;
    switch (command) {
    case C::ScreenHigh: row=top+std::min(action.count-1,page-1);linewise=true;break;
    case C::ScreenMiddle: row=top+(std::min(page,lastRow-top+1)-1)/2;linewise=true;break;
    case C::ScreenLow: row=std::max(top,std::min(lastRow,top+page-1)-action.count+1);linewise=true;break;
    case C::ScreenUp: row-=action.count;break;
    case C::ScreenDown: row+=action.count;break;
    case C::ScreenEnd: row+=action.count-1;inclusive=true;break;
    case C::ScreenStart: case C::ScreenFirstText: case C::ScreenMiddleColumn: break;
    default: return false;
    }
    row=std::clamp(row,0,lastRow);
    const auto slice=screenSlice(row);
    const auto last=slice.begin<slice.end ? graphemeBoundaryAt(slice.end,false) : slice.begin;
    const auto leading=std::clamp(VimMotions::firstText(buffer_,slice.line),slice.begin,last);
    const bool wrapped = slice.left || slice.right >= 0;
    const int left=wrapped ? 0 : horizontal->value();
    const int width=std::max(1,textAreaWidth()-8);
    const bool vertical = command==C::ScreenDown || command==C::ScreenUp;
    if (vertical && vimScreenPreferredX_<0)
        vimScreenPreferredX_=std::max(0,cursorPixelColumn()-screenOriginX(cursorPosition_));
    std::size_t target = leading;
    if (vertical) target=screenPositionAtX(row,vimScreenPreferredX_+left);
    else if (command==C::ScreenStart) target=screenPositionAtX(row,left);
    else if (command==C::ScreenFirstText) {
        target=screenPositionAtX(row,left);
        while (target<last && (buffer_.byteAt(target)==' ' || buffer_.byteAt(target)=='\t'))
            target=graphemeBoundaryAt(target,true);
    } else if (command==C::ScreenMiddleColumn) target=screenPositionAtX(row,left+width/2);
    else if (command==C::ScreenEnd) target=wrapped ? last : screenPositionAtX(row,left+width-1);

    VimMotions::Range range;
    range.valid=true;
    range.target=target;
    range.linewise=linewise;
    range.begin=std::min(cursorPosition_,target);
    range.end=std::max(cursorPosition_,target);
    if (linewise) {
        const int first=std::min(int(currentLine()),slice.line);
        const int last=closedFoldEnd(std::max(int(currentLine()),slice.line));
        range.begin=vimLineStart(first);
        range.end=last+1<int(buffer_.lineCount()) ? buffer_.lineStart(last+1) : buffer_.size();
        if (action.operation==C::Consume) pushVimJump();
    } else if (inclusive && range.end<buffer_.size()) {
        range.end=std::max(range.end,std::min(lineContentEnd(buffer_.lineIndexAt(range.end)),
                                              graphemeBoundaryAt(range.end,true)));
    }
    const int preferred=vimScreenPreferredX_;
    applyVimAction(action,range);
    if (vertical && action.operation==C::Consume) vimScreenPreferredX_=preferred;
    return true;
}
