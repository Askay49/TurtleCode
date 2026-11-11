#include "core/text/EditHistory.hpp"
#include "core/text/ChunkedFile.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

void testTypingCoalesces() {
    PieceTreeBuffer buffer;
    EditHistory history;
    buffer.insert(0, "a");
    history.record(0, {}, "a", 0, 1, EditKind::Insert, 10);
    buffer.insert(1, "b");
    history.record(1, {}, "b", 1, 2, EditKind::Insert, 20);
    buffer.insert(2, "c");
    history.record(2, {}, "c", 2, 3, EditKind::Insert, 30);
    check(buffer.text() == "abc", "typing creates text");
    auto undone = history.undo(buffer);
    check(undone && undone->cursorPosition == 0, "undo typing restores initial cursor");
    check(buffer.text().empty(), "consecutive typing undoes as one command");
    auto redone = history.redo(buffer);
    check(redone && redone->cursorPosition == 3, "redo typing restores final cursor");
    check(buffer.text() == "abc", "redo restores typed text");
}

void testBackspaceAndForwardDeleteCoalesce() {
    {
        PieceTreeBuffer buffer("abcd");
        EditHistory history;
        buffer.erase(3, 1);
        history.record(3, "d", {}, 4, 3, EditKind::Backspace, 10);
        buffer.erase(2, 1);
        history.record(2, "c", {}, 3, 2, EditKind::Backspace, 20);
        auto result = history.undo(buffer);
        check(result && result->cursorPosition == 4, "backspace undo restores cursor");
        check(buffer.text() == "abcd", "consecutive backspaces undo together");
    }
    {
        PieceTreeBuffer buffer("abcd");
        EditHistory history;
        buffer.erase(1, 1);
        history.record(1, "b", {}, 1, 1, EditKind::DeleteForward, 10);
        buffer.erase(1, 1);
        history.record(1, "c", {}, 1, 1, EditKind::DeleteForward, 20);
        const auto undoResult = history.undo(buffer);
        check(undoResult.has_value(), "forward delete undo exists");
        check(buffer.text() == "abcd", "consecutive forward deletes undo together");
    }
}

void testSavePointAndStandaloneBoundary() {
    PieceTreeBuffer buffer;
    EditHistory history;
    buffer.insert(0, "a");
    history.record(0, {}, "a", 0, 1, EditKind::Insert, 10);
    history.markSaved();
    check(!history.isModified(), "markSaved clears modified state");
    buffer.insert(1, "b");
    history.record(1, {}, "b", 1, 2, EditKind::Insert, 20);
    buffer.insert(2, "c");
    history.record(2, {}, "c", 2, 3, EditKind::Insert, 30);
    check(history.undo(buffer).has_value(), "undo after save exists");
    check(buffer.text() == "a", "undo after save preserves saved text");
    check(!history.isModified(), "undo back to save point clears modified state");
    check(history.redo(buffer).has_value(), "redo after save exists");
    check(history.isModified(), "redo away from save point marks modified");

    history.breakCoalescing();
    buffer.insert(3, "!");
    history.record(3, {}, "!", 3, 4, EditKind::Standalone, 40);
    const auto standaloneUndo = history.undo(buffer);
    check(standaloneUndo.has_value(), "standalone undo exists");
    check(buffer.text() == "abc", "standalone command is independently undoable");
}

void testTimeoutSplitsGroups() {
    PieceTreeBuffer buffer;
    EditHistory history;
    buffer.insert(0, "x");
    history.record(0, {}, "x", 0, 1, EditKind::Insert, 0);
    buffer.insert(1, "y");
    history.record(1, {}, "y", 1, 2, EditKind::Insert, 900);
    auto first = history.undo(buffer);
    check(first && buffer.text() == "x", "typing after timeout is separate undo step");
    const auto secondUndo = history.undo(buffer);
    check(secondUndo.has_value(), "second timeout undo exists");
    check(buffer.text().empty(), "second undo removes earlier typing");
}

void testBatchCursorHistory() {
    PieceTreeBuffer buffer("aa aa");
    EditHistory history;
    history.recordBatch({{0, {}, "X"}, {3, {}, "X"}}, {0, 3}, {1, 4}, 10);
    buffer.insert(3, "X");
    buffer.insert(0, "X");
    check(buffer.text() == "Xaa Xaa", "batch edit applies all cursor changes");
    const auto undone = history.undo(buffer);
    check(undone && undone->cursorPositions.size() == 2, "batch undo restores all cursors");
    check(buffer.text() == "aa aa", "batch undo removes all cursor changes");
    const auto redone = history.redo(buffer);
    check(redone && redone->cursorPositions.size() == 2, "batch redo restores all cursors");
    check(buffer.text() == "Xaa Xaa", "batch redo reapplies all cursor changes");
}
void testBatchUndoWithDifferentLengths() {
    PieceTreeBuffer buffer("ab--cd--ef");
    EditHistory history;
    std::vector<EditChange> changes{{0, "ab", "XYZ"}, {4, "cd", ""}, {8, "ef", "Q"}};
    history.recordBatch(changes, {0}, {3}, 10);
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        buffer.erase(it->offset, it->removed.size());
        buffer.insert(it->offset, it->inserted);
    }
    check(buffer.text() == "XYZ----Q", "batch changes with different lengths apply");
    check(history.undo(buffer).has_value(), "batch undo exists");
    check(buffer.text() == "ab--cd--ef", "batch undo restores different-length changes");
    check(history.redo(buffer).has_value(), "batch redo exists");
    check(buffer.text() == "XYZ----Q", "batch redo restores different-length changes");
}
void testAppliedEditNotifications() {
    const std::string original="aa\nмир\ncc\n";
    PieceTreeBuffer buffer(original); EditHistory history;
    const std::vector<EditChange> changes{{0,"aa","A\nA"},{3,"мир","имя\n!"},{10,"cc","C"}};
    for(auto it=changes.rbegin();it!=changes.rend();++it) {
        buffer.erase(it->offset,it->removed.size()); buffer.insert(it->offset,it->inserted);
    }
    const auto modified=buffer.text();
    history.recordBatch(changes,{0},{1},0);
    PieceTreeBuffer mirror(modified); int notifications=0;
    const auto applied=[&](std::size_t offset,std::size_t row,std::size_t column,
                           std::string_view removed,std::string_view inserted) {
        ++notifications;
        check(row==mirror.lineIndexAt(offset) && column==offset-mirror.lineStart(row),"history reports pre-edit byte coordinates");
        check(mirror.slice(offset,removed.size())==removed,"history reports exact removed bytes");
        mirror.erase(offset,removed.size()); mirror.insert(offset,inserted);
        check(mirror.text()==buffer.text(),"history observers see each applied change in order");
    };
    check(history.undo(buffer,applied).has_value() && buffer.text()==original,"notified undo restores UTF-8 multiline batches");
    check(history.redo(buffer,applied).has_value() && buffer.text()==modified,"notified redo restores UTF-8 multiline batches");
    check(notifications==6,"undo and redo report every batch change once");
}

void testSinglePayloadAndCompaction() {
    PieceTreeBuffer buffer("base"); EditHistory history;
    const std::string pasted(17u*1024u*1024u,'x');
    buffer.insert(4,pasted); history.record(4,{},pasted,4,4+pasted.size(),EditKind::Standalone,0);
    check(history.retainedTextBytes()==pasted.size(),"single edit stores its payload once");
    check(history.undo(buffer).has_value() && buffer.text()=="base","large paste undo preserves original text");
    check(buffer.storageBytes()<1024,"undo compacts discarded add storage");
    check(history.retainedTextBytes()==pasted.size(),"redo payload remains available after buffer compaction");
    check(history.redo(buffer).has_value() && buffer.size()==4+pasted.size() && buffer.byteAt(4)=='x',"redo restores text after compaction");
    history.clear(); check(history.retainedTextBytes()==0,"clearing history releases payloads");
    history.recordBatch({{4,pasted,"small"}},{4},{9},0);
    check(history.retainedTextBytes()==pasted.size()+5,"single-change batch stores payload once");
    history.clear(); history.recordBatch({{0,"a","bb"},{8,"ccc","d"}},{0,8},{2,9},1);
    check(history.retainedTextBytes()==7,"multi-change batch does not duplicate first payload");
}

void testHistoryBudgets() {
    PieceTreeBuffer buffer("start");EditHistory history;EditHistory::Limits limits;limits.memoryBytes=4;history.setLimits(limits);
    const std::string text="abcdefghij";buffer.insert(5,text);history.record(5,{},text,5,15,EditKind::Standalone,0);
    check(history.memoryBytes()==0 && history.diskBytes()==text.size(),"history spills payload over its RAM budget");
    check(history.undo(buffer).has_value() && buffer.text()=="start","spilled undo streams removed content");
    check(history.redo(buffer).has_value() && buffer.text()=="start"+text,"spilled redo streams inserted content");
    history.markSaved();limits.diskBytes=5;history.setLimits(limits);
    check(!history.canUndo() && !history.isModified() && !history.takeNotice().empty(),"disk cap prunes oldest history without changing saved-state tracking");
    limits.diskBytes=0;limits.memoryBytes=100;history.setLimits(limits);
    buffer.insert(15,"x");history.record(15,{},"x",15,16,EditKind::Insert,1);
    buffer.insert(16,"y");history.record(16,{},"y",16,17,EditKind::Insert,2);
    check(history.undo(buffer).has_value() && buffer.text()=="start"+text,"coalescing still works with budget policy");
    limits.memoryBytes=0;limits.spillToDisk=false;history.setLimits(limits);
    check(!history.canRedo() && history.retainedTextBytes()==0,"RAM-only history respects its limit and releases disk commands");
}

void testStoredBatch() {
    PieceTreeBuffer buffer("A--BC--D");EditHistory history;
    auto payload=std::make_shared<ChunkedFile>();payload->append("AoneBCxDtail");
    buffer.erase(7,1);buffer.insert(7,"tail");buffer.erase(3,2);buffer.insert(3,"x");buffer.erase(0,1);buffer.insert(0,"one");
    history.recordDiskBatch({{0,1,3},{3,2,1},{7,1,4}},payload,{0,3,7},{3,6,12},0);
    history.markSaved();
    check(buffer.text()=="one--x--tail","prepare unequal-length disk batch");
    check(history.undo(buffer).has_value() && buffer.text()=="A--BC--D","streamed batch undo adjusts later offsets");
    check(history.isModified(),"undo moves away from disk batch saved state");
    check(history.redo(buffer).has_value() && buffer.text()=="one--x--tail" && !history.isModified(),"streamed batch redo restores saved state");
    auto limits=history.limits();limits.memoryBytes=2;limits.spillToDisk=false;history.setLimits(limits);
    check(!history.canUndo() && !history.isModified(),"disabling disk spill prunes an oversized stored batch without restoring its payload");
}

}

int main() {
    testAppliedEditNotifications();
    testSinglePayloadAndCompaction();
    testHistoryBudgets();
    testStoredBatch();
    testTypingCoalesces();
    testBackspaceAndForwardDeleteCoalesce();
    testSavePointAndStandaloneBoundary();
    testTimeoutSplitsGroups();
    testBatchCursorHistory();
    testBatchUndoWithDifferentLengths();
    if (failures) return EXIT_FAILURE;
    std::cout << "All EditHistory tests passed.\n";
    return EXIT_SUCCESS;
}
