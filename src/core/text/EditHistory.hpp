#pragma once

#include "PieceTreeBuffer.hpp"

#include <cstdint>
#include <optional>
#include <functional>
#include <string_view>
#include <string>
#include <vector>

struct EditChange {
    std::size_t offset{};
    std::string removed;
    std::string inserted;
};

enum class EditKind : std::uint8_t {
    Insert, Backspace, DeleteForward, Standalone
};

struct HistoryResult {
    std::size_t cursorPosition{};
    std::vector<std::size_t> cursorPositions;
};

// Command history with typing/deletion coalescing and save-point tracking.
// Timestamps are monotonic milliseconds supplied by the UI layer.
class EditHistory final {
public:
    // Called after each applied change, with its pre-edit UTF-8 row/column.
    using EditApplied = std::function<void(std::size_t offset,std::size_t row,std::size_t column,
                                          std::string_view removed,std::string_view inserted)>;
    struct Limits {
        std::size_t memoryBytes{64u*1024u*1024u};
        bool spillToDisk{true};
        std::size_t diskBytes{0}; // zero means unlimited
    };
    void setLimits(Limits limits);
    [[nodiscard]] Limits limits() const { return limits_; }
    [[nodiscard]] std::size_t memoryBytes() const;
    [[nodiscard]] std::size_t diskBytes() const;
    [[nodiscard]] bool undoNeedsBackground() const;
    [[nodiscard]] bool redoNeedsBackground() const;
    [[nodiscard]] std::size_t nextUndoBytes() const;
    [[nodiscard]] std::size_t nextRedoBytes() const;
    std::string takeNotice() { return std::exchange(notice_,{}); }
    struct StoredChange {std::size_t offset,removed,inserted;};
    void recordDiskBatch(const std::vector<StoredChange>& changes,std::shared_ptr<ChunkedFile> payload,
                        std::vector<std::size_t> before,std::vector<std::size_t> after,std::int64_t timestampMs);
    void recordOnDisk(std::size_t offset,std::shared_ptr<ChunkedFile> payload,std::size_t removed,std::size_t inserted,
                      std::size_t cursorBefore,std::size_t cursorAfter,std::int64_t timestampMs);
    void clear() noexcept;
    [[nodiscard]] std::size_t retainedTextBytes() const noexcept;
    void record(std::size_t offset, std::string removed, std::string inserted,
                std::size_t cursorBefore, std::size_t cursorAfter,
                EditKind kind, std::int64_t timestampMs);
    void recordBatch(std::vector<EditChange> changes,
                     std::vector<std::size_t> cursorsBefore,
                     std::vector<std::size_t> cursorsAfter,
                     std::int64_t timestampMs);
    [[nodiscard]] std::optional<HistoryResult> undo(PieceTreeBuffer& buffer, const EditApplied& applied = {},const std::function<void()>& checkpoint = {});
    [[nodiscard]] std::optional<HistoryResult> redo(PieceTreeBuffer& buffer, const EditApplied& applied = {},const std::function<void()>& checkpoint = {});
    void markSaved() noexcept;
    void markUnsaved() noexcept { savedState_ = UINT64_MAX; }
    void breakCoalescing() noexcept { coalescingBarrier_ = true; }
    [[nodiscard]] bool isModified() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }

private:
    struct Command {
        std::shared_ptr<ChunkedFile> disk;
        std::vector<std::size_t> lengths;
        std::size_t offset{};
        std::string removed;
        std::string inserted;
        std::size_t cursorBefore{};
        std::size_t cursorAfter{};
        std::vector<EditChange> changes;
        std::vector<std::size_t> cursorsBefore;
        std::vector<std::size_t> cursorsAfter;
        std::uint64_t beforeState{};
        std::uint64_t afterState{};
        EditKind kind{EditKind::Standalone};
        std::int64_t timestampMs{};
    };

    Limits limits_;
    std::string notice_;
    static std::size_t payloadBytes(const Command& command);
    static void restore(Command& command);
    void enforceLimits();
    static constexpr std::int64_t kCoalesceWindowMs = 750;
    static bool canCoalesce(const Command& previous, std::size_t offset,
                            const std::string& removed, const std::string& inserted,
                            EditKind kind, std::int64_t timestampMs) noexcept;

    std::vector<Command> undo_;
    std::vector<Command> redo_;
    std::uint64_t currentState_{0};
    std::uint64_t savedState_{0};
    std::uint64_t nextState_{1};
    bool coalescingBarrier_{true};
};
