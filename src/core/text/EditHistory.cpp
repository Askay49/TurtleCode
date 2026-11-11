#include "EditHistory.hpp"

#include <utility>
#include "ChunkedFile.hpp"
#include <algorithm>
#include <limits>

namespace {
void applyChange(PieceTreeBuffer& buffer,std::size_t offset,std::string_view removed,std::string_view inserted,
                 const EditHistory::EditApplied& applied) {
    const auto row=applied ? buffer.lineIndexAt(offset) : 0;
    const auto column=applied ? offset-buffer.lineStart(row) : 0;
    if(!removed.empty()) buffer.erase(offset,removed.size());
    if(!inserted.empty()) buffer.insert(offset,inserted);
    if(applied) applied(offset,row,column,removed,inserted);
}
}

void EditHistory::clear() noexcept {
    undo_.clear();
    redo_.clear(); notice_.clear();
    currentState_ = 0;
    savedState_ = 0;
    nextState_ = 1;
    coalescingBarrier_ = true;
}

bool EditHistory::canCoalesce(const Command& previous, std::size_t offset,
                              const std::string& removed, const std::string& inserted,
                              EditKind kind, std::int64_t timestampMs) noexcept {
    if (previous.disk || kind == EditKind::Standalone || previous.kind != kind ||
        timestampMs < previous.timestampMs ||
        timestampMs - previous.timestampMs > kCoalesceWindowMs ||
        (!removed.empty() && !inserted.empty())) return false;

    // Newlines are natural typing boundaries: undo them independently.
    if (inserted.find('\n') != std::string::npos || previous.inserted.find('\n') != std::string::npos ||
        removed.find('\n') != std::string::npos || previous.removed.find('\n') != std::string::npos)
        return false;

    switch (kind) {
    case EditKind::Insert:
        return removed.empty() && previous.removed.empty() &&
               offset == previous.offset + previous.inserted.size();
    case EditKind::Backspace:
        return inserted.empty() && previous.inserted.empty() &&
               offset + removed.size() == previous.offset;
    case EditKind::DeleteForward:
        return inserted.empty() && previous.inserted.empty() && offset == previous.offset;
    case EditKind::Standalone:
        return false;
    }
    return false;
}

void EditHistory::record(std::size_t offset, std::string removed, std::string inserted,
                         std::size_t cursorBefore, std::size_t cursorAfter,
                         EditKind kind, std::int64_t timestampMs) {
    redo_.clear();
    if (!coalescingBarrier_ && !undo_.empty() && undo_.back().afterState == currentState_ &&
        canCoalesce(undo_.back(), offset, removed, inserted, kind, timestampMs)) {
        Command& previous = undo_.back();
        if (kind == EditKind::Insert) {
            previous.inserted += inserted;
        } else if (kind == EditKind::Backspace) {
            previous.offset = offset;
            previous.removed = removed + previous.removed;
        } else if (kind == EditKind::DeleteForward) {
            previous.removed += removed;
        }
        previous.cursorAfter = cursorAfter;
        previous.timestampMs = timestampMs;
        previous.afterState = nextState_++;
        currentState_ = previous.afterState;
        coalescingBarrier_ = false; enforceLimits();
        return;
    }

    Command command;
    command.offset = offset;
    command.removed = std::move(removed);
    command.inserted = std::move(inserted);
    command.cursorBefore = cursorBefore;
    command.cursorAfter = cursorAfter;
    command.cursorsBefore = {cursorBefore};
    command.cursorsAfter = {cursorAfter};
    command.beforeState = currentState_;
    command.afterState = nextState_++;
    command.kind = kind;
    command.timestampMs = timestampMs;
    currentState_ = command.afterState;
    coalescingBarrier_ = true;
    undo_.push_back(std::move(command));
    coalescingBarrier_ = (kind == EditKind::Standalone);
    enforceLimits();
}

void EditHistory::recordBatch(std::vector<EditChange> changes,
                              std::vector<std::size_t> cursorsBefore,
                              std::vector<std::size_t> cursorsAfter,
                              std::int64_t timestampMs) {
    if (changes.empty()) return;
    redo_.clear();
    Command command;
    command.offset=changes.front().offset;
    // Single edits use the inline payload; batches own only their changes.
    if(changes.size()==1) {
        command.offset=changes.front().offset;
        command.removed=std::move(changes.front().removed);
        command.inserted=std::move(changes.front().inserted);
    }
    command.cursorBefore = cursorsBefore.empty() ? 0 : cursorsBefore.front();
    command.cursorAfter = cursorsAfter.empty() ? command.cursorBefore : cursorsAfter.front();
    command.beforeState = currentState_;
    command.afterState = nextState_++;
    command.kind = EditKind::Standalone;
    command.timestampMs = timestampMs;
    if(changes.size()>1) command.changes = std::move(changes);
    command.cursorsBefore = std::move(cursorsBefore);
    command.cursorsAfter = std::move(cursorsAfter);
    currentState_ = command.afterState;
    coalescingBarrier_ = true;
    undo_.push_back(std::move(command)); enforceLimits();
}

namespace {
void applyStoredCommand(PieceTreeBuffer& buffer,const std::shared_ptr<ChunkedFile>& file,const std::vector<std::size_t>& lengths,
                        std::size_t offset,const std::vector<EditChange>& changes,bool undo,const std::function<void()>& checkpoint) {
    struct Change {std::size_t at,removed,inserted,oldData,newData;};std::vector<Change> items;
    std::size_t position=0,index=0;std::ptrdiff_t delta=0;
    auto add=[&](std::size_t at) {
        const auto removed=lengths.at(index++),inserted=lengths.at(index++);
        const auto target=undo ? static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at)+delta) : at;
        items.push_back({target,removed,inserted,position,position+removed});position+=removed+inserted;
        delta+=static_cast<std::ptrdiff_t>(inserted)-static_cast<std::ptrdiff_t>(removed);
    };
    if(changes.empty())add(offset);else{position=lengths[0]+lengths[1];index=2;for(const auto& change:changes)add(change.offset);}
    for(auto it=items.rbegin();it!=items.rend();++it) {
        if(checkpoint)checkpoint();
        buffer.erase(it->at,undo?it->inserted:it->removed);
        const auto size=undo?it->removed:it->inserted,begin=undo?it->oldData:it->newData;
        for(std::size_t written=0;written<size;written+=64*1024) {
            if(checkpoint)checkpoint();
            const auto bytes=file->read(begin+written,std::min<std::size_t>(64*1024,size-written));
            buffer.insert(it->at+written,bytes);
        }
    }
    file->dropCache();
}
}

std::optional<HistoryResult> EditHistory::undo(PieceTreeBuffer& buffer, const EditApplied& applied,const std::function<void()>& checkpoint) {
    if (undo_.empty()) return std::nullopt;
    if(undo_.back().disk && !applied) {
        auto& command=undo_.back();applyStoredCommand(buffer,command.disk,command.lengths,command.offset,command.changes,true,checkpoint);
        currentState_=command.beforeState;const HistoryResult result{command.cursorBefore,command.cursorsBefore};
        redo_.push_back(std::move(command));undo_.pop_back();coalescingBarrier_=true;enforceLimits();return result;
    }
    try { restore(undo_.back()); } catch(const std::exception& error) { notice_=error.what();return std::nullopt; }
    Command command = std::move(undo_.back());
    undo_.pop_back();
    if (command.changes.size() > 1) {
        std::vector<std::size_t> adjustedOffsets;
        adjustedOffsets.reserve(command.changes.size());
        std::ptrdiff_t delta = 0;
        for (const auto& change : command.changes) {
            adjustedOffsets.push_back(delta >= 0
                ? change.offset + static_cast<std::size_t>(delta)
                : change.offset - static_cast<std::size_t>(-delta));
            delta += static_cast<std::ptrdiff_t>(change.inserted.size()) -
                     static_cast<std::ptrdiff_t>(change.removed.size());
        }
        for (std::size_t index = command.changes.size(); index-- > 0;) {
            const auto& change = command.changes[index];
            const auto offset = adjustedOffsets[index];
            applyChange(buffer,offset,change.inserted,change.removed,applied);
        }
    } else {
        applyChange(buffer,command.offset,command.inserted,command.removed,applied);
    }
    currentState_ = command.beforeState;
    coalescingBarrier_ = true;
    const auto cursor = command.cursorBefore;
    const auto cursors = command.cursorsBefore;
    redo_.push_back(std::move(command)); enforceLimits();
    return HistoryResult{cursor, cursors.empty() ? std::vector<std::size_t>{cursor} : cursors};
}

std::optional<HistoryResult> EditHistory::redo(PieceTreeBuffer& buffer, const EditApplied& applied,const std::function<void()>& checkpoint) {
    if (redo_.empty()) return std::nullopt;
    if(redo_.back().disk && !applied) {
        auto& command=redo_.back();applyStoredCommand(buffer,command.disk,command.lengths,command.offset,command.changes,false,checkpoint);
        currentState_=command.afterState;const HistoryResult result{command.cursorAfter,command.cursorsAfter};
        undo_.push_back(std::move(command));redo_.pop_back();coalescingBarrier_=true;enforceLimits();return result;
    }
    try { restore(redo_.back()); } catch(const std::exception& error) { notice_=error.what();return std::nullopt; }
    Command command = std::move(redo_.back());
    redo_.pop_back();
    if (command.changes.size() > 1) {
        for (auto it = command.changes.rbegin(); it != command.changes.rend(); ++it) {
            const auto& change = *it;
            applyChange(buffer,change.offset,change.removed,change.inserted,applied);
        }
    } else {
        applyChange(buffer,command.offset,command.removed,command.inserted,applied);
    }
    currentState_ = command.afterState;
    coalescingBarrier_ = true;
    const auto cursor = command.cursorAfter;
    const auto cursors = command.cursorsAfter;
    undo_.push_back(std::move(command)); enforceLimits();
    return HistoryResult{cursor, cursors.empty() ? std::vector<std::size_t>{cursor} : cursors};
}

void EditHistory::markSaved() noexcept { savedState_ = currentState_; coalescingBarrier_ = true; }
bool EditHistory::isModified() const noexcept { return currentState_ != savedState_; }


std::size_t EditHistory::payloadBytes(const Command& command) {
    if(command.disk) { std::size_t total=0;for(auto bytes:command.lengths)total+=bytes;return total; }
    std::size_t total=command.removed.size()+command.inserted.size();
    for(const auto& change:command.changes)total+=change.removed.size()+change.inserted.size();
    return total;
}
std::size_t EditHistory::retainedTextBytes() const noexcept {
    std::size_t total=0;for(const auto* stack:{&undo_,&redo_})for(const auto& command:*stack)total+=payloadBytes(command);return total;
}
std::size_t EditHistory::memoryBytes() const {
    std::size_t total=0;for(const auto* stack:{&undo_,&redo_})for(const auto& command:*stack)if(!command.disk)total+=payloadBytes(command);return total;
}
std::size_t EditHistory::diskBytes() const {
    std::size_t total=0;for(const auto* stack:{&undo_,&redo_})for(const auto& command:*stack)if(command.disk)total+=payloadBytes(command);return total;
}
std::size_t EditHistory::nextUndoBytes() const { return undo_.empty()?0:payloadBytes(undo_.back()); }
std::size_t EditHistory::nextRedoBytes() const { return redo_.empty()?0:payloadBytes(redo_.back()); }
void EditHistory::restore(Command& command) {
    if(!command.disk)return;
    std::size_t offset=0,index=0;
    const auto read=[&](std::string& text) { auto length=command.lengths.at(index++);text=command.disk->read(offset,length);offset+=length; };
    // Decode into a separate command so an I/O failure leaves the stored one intact.
    Command restored=command;read(restored.removed);read(restored.inserted);
    for(auto& change:restored.changes) {read(change.removed);read(change.inserted);}
    restored.disk.reset();restored.lengths.clear();command=std::move(restored);
}
void EditHistory::setLimits(Limits limits) { limits_=limits;enforceLimits();coalescingBarrier_=true; }
void EditHistory::enforceLimits() {
    const auto spill=[&](Command& command) {
        if(command.disk || !payloadBytes(command))return;
        auto file=std::make_shared<ChunkedFile>();std::vector<std::size_t> lengths;
        const auto write=[&](const std::string& text){lengths.push_back(text.size());file->append(text);};
        write(command.removed);write(command.inserted);for(const auto& change:command.changes){write(change.removed);write(change.inserted);}
        command.disk=std::move(file);command.lengths=std::move(lengths);
        std::string{}.swap(command.removed);std::string{}.swap(command.inserted);
        for(auto& change:command.changes){std::string{}.swap(change.removed);std::string{}.swap(change.inserted);}
    };
    auto memory=memoryBytes();
    if(limits_.spillToDisk && memory>limits_.memoryBytes) {
        try {
            for(auto* stack:{&undo_,&redo_})for(auto& command:*stack) {
                if(memory<=limits_.memoryBytes)break;
                if(!command.disk){auto bytes=payloadBytes(command);spill(command);memory-=bytes;}
            }
        } catch(const std::exception& error) {
            notice_=std::string("Could not store undo history on disk: ")+error.what();
            // Keep recoverable history even if storage is temporarily unavailable.
            return;
        }
    }
    bool trimmed=false;
    auto over=[&]{return (!limits_.spillToDisk && retainedTextBytes()>limits_.memoryBytes)
        || (limits_.diskBytes && diskBytes()>limits_.diskBytes);};
    while(over() && (!undo_.empty() || !redo_.empty())) {
        if(!undo_.empty())undo_.erase(undo_.begin());else redo_.erase(redo_.begin());trimmed=true;
    }
    if(!limits_.spillToDisk) {
        try {for(auto* stack:{&undo_,&redo_})for(auto& command:*stack)restore(command);}
        catch(const std::exception& error){notice_=error.what();return;}
    }
    if(trimmed){notice_="Oldest undo/redo actions were removed because the history limit was reached.";coalescingBarrier_=true;}
}
void EditHistory::recordOnDisk(std::size_t offset,std::shared_ptr<ChunkedFile> payload,std::size_t removed,std::size_t inserted,
                              std::size_t before,std::size_t after,std::int64_t timestamp) {
    redo_.clear();Command command;command.offset=offset;command.disk=std::move(payload);command.lengths={removed,inserted};
    command.cursorBefore=before;command.cursorAfter=after;command.cursorsBefore={before};command.cursorsAfter={after};
    command.beforeState=currentState_;command.afterState=nextState_++;command.kind=EditKind::Standalone;command.timestampMs=timestamp;
    currentState_=command.afterState;undo_.push_back(std::move(command));coalescingBarrier_=true;
    enforceLimits();
}

void EditHistory::recordDiskBatch(const std::vector<StoredChange>& changes,std::shared_ptr<ChunkedFile> payload,
                                std::vector<std::size_t> before,std::vector<std::size_t> after,std::int64_t timestamp) {
    if(changes.empty())return;
    if(changes.size()==1){recordOnDisk(changes[0].offset,std::move(payload),changes[0].removed,changes[0].inserted,before.empty()?0:before[0],after.empty()?0:after[0],timestamp);return;}
    redo_.clear();Command command;command.disk=std::move(payload);command.lengths={0,0};
    command.cursorsBefore=std::move(before);command.cursorsAfter=std::move(after);
    command.cursorBefore=command.cursorsBefore.empty()?0:command.cursorsBefore[0];command.cursorAfter=command.cursorsAfter.empty()?0:command.cursorsAfter[0];
    for(const auto& change:changes){command.changes.push_back({change.offset,{},{}});command.lengths.push_back(change.removed);command.lengths.push_back(change.inserted);}
    command.beforeState=currentState_;command.afterState=nextState_++;command.kind=EditKind::Standalone;command.timestampMs=timestamp;
    currentState_=command.afterState;undo_.push_back(std::move(command));coalescingBarrier_=true;
    enforceLimits();
}

bool EditHistory::undoNeedsBackground() const {return !undo_.empty() && (undo_.back().disk || undo_.back().changes.size()>256 || nextUndoBytes()>=1024*1024);}
bool EditHistory::redoNeedsBackground() const {return !redo_.empty() && (redo_.back().disk || redo_.back().changes.size()>256 || nextRedoBytes()>=1024*1024);}
