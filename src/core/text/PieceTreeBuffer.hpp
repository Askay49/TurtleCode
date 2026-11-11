#pragma once

#include <atomic>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

class ChunkedFile;

// UTF-8 byte-oriented editable buffer backed by an implicit treap of pieces.
// Offsets are byte offsets; callers must keep edits on UTF-8 code-point boundaries.
class PieceTreeBuffer final {
public:
    PieceTreeBuffer() = default;
    explicit PieceTreeBuffer(std::string_view text) { load(text); }
    ~PieceTreeBuffer() = default;

    PieceTreeBuffer(const PieceTreeBuffer&) = delete;
    PieceTreeBuffer& operator=(const PieceTreeBuffer&) = delete;
    PieceTreeBuffer(PieceTreeBuffer&& other) noexcept;
    PieceTreeBuffer& operator=(PieceTreeBuffer&& other) noexcept;

    void load(std::string_view text);
    bool loadFile(const std::string& path,std::string* error = nullptr,const std::function<bool()>& cancelled = {},
                  const std::function<void(std::size_t,std::size_t,std::string_view)>& progress = {});
    [[nodiscard]] bool isFileBacked() const noexcept { return static_cast<bool>(backing_); }
    [[nodiscard]] std::size_t cachedFileBytes() const;
    bool forEachChunk(const std::function<bool(std::string_view)>& visit) const;
    void clear();
    // Optional observer is local to this instance and is never copied into snapshots.
    std::function<void(std::size_t,std::size_t,std::size_t,std::size_t)> lineEditObserver;
    void insert(std::size_t byteOffset, std::string_view text);
    void erase(std::size_t byteOffset, std::size_t byteLength);

    [[nodiscard]] std::size_t size() const noexcept;
    // Backing string capacities, excluding nodes. Useful for resource budgets.
    [[nodiscard]] std::size_t storageBytes() const noexcept;
    [[nodiscard]] std::size_t lineCount() const noexcept;
    [[nodiscard]] std::size_t lineIndexAt(std::size_t byteOffset) const noexcept;
    [[nodiscard]] std::size_t lineStart(std::size_t line) const noexcept;
    [[nodiscard]] std::size_t lineEnd(std::size_t line) const noexcept; // excludes '\n'
    [[nodiscard]] std::string line(std::size_t line) const;
    void forEachLineBounded(std::size_t maxBytes,const std::function<bool(std::size_t,std::string_view,bool)>& visit) const;
    void forEachLine(const std::function<bool(std::size_t, std::string_view)>& visit) const;
    [[nodiscard]] std::string slice(std::size_t byteOffset, std::size_t byteLength) const;
    [[nodiscard]] std::string text() const;
    [[nodiscard]] PieceTreeBuffer snapshot(bool preserveStorage = false) const;
    struct RecoveryPiece { std::uint64_t source; std::size_t start, length, position; };
    // Stable storage identities allow recovery to append only newly referenced pieces.
    [[nodiscard]] std::vector<RecoveryPiece> recoveryPieces() const;
    [[nodiscard]] std::size_t utf16Before(std::size_t offset) const;
    [[nodiscard]] std::size_t byteAtUtf16(std::size_t index) const;
    [[nodiscard]] std::size_t codePointsBefore(std::size_t offset) const;
    [[nodiscard]] std::size_t byteAtCodePoint(std::size_t index) const;
    [[nodiscard]] char byteAt(std::size_t byteOffset) const noexcept;

private:
    inline static std::atomic<std::uint64_t> nextStorageId_{1};
    std::uint64_t originalId_{nextStorageId_++}, addId_{nextStorageId_++}, diskId_{nextStorageId_++};
    enum class Source : std::uint8_t { Original, Add, AddedDisk };
    struct Node {
        Source source;
        std::size_t start;
        std::size_t length;
        std::uint32_t priority;
        std::size_t subtreeBytes;
        std::size_t ownNewlines;
        std::size_t subtreeNewlines;
        std::size_t subtreeDiskBytes;
        std::size_t ownCodePoints,subtreeCodePoints,ownUtf16,subtreeUtf16;
        std::unique_ptr<Node> left;
        std::unique_ptr<Node> right;
        Node(Source src, std::size_t begin, std::size_t len, std::uint32_t p, std::size_t nl = 0,std::size_t cp = static_cast<std::size_t>(-1),std::size_t utf16 = static_cast<std::size_t>(-1));
    };

    using NodePtr = std::unique_ptr<Node>;
    static std::size_t bytes(const NodePtr& node) noexcept;
    static std::size_t newlines(const NodePtr& node) noexcept;
    static void refresh(Node& node);
    std::pair<NodePtr, NodePtr> split(NodePtr root, std::size_t offset);
    static NodePtr merge(NodePtr left, NodePtr right);
    void appendRange(const Node* node, std::size_t begin, std::size_t end,
                            std::size_t base, std::string& out) const;
    std::size_t countNewlinesBefore(const Node* node,std::size_t offset) const noexcept;
    std::size_t findNewline(const Node* node, std::size_t ordinal,std::size_t base) const noexcept;
    std::uint32_t nextPriority() noexcept;
    void compactIfWasteful();
    void buildOriginalPieces();
    void spillAddedArena();

    struct SourceView {
        std::shared_ptr<const std::string> owner;
        std::string_view bytes;
    };
    SourceView sourceView(Source source,std::size_t start,std::size_t length) const;
    std::shared_ptr<ChunkedFile> backing_,addedBacking_;
    std::string original_;
    std::string add_;
    NodePtr root_;
    std::uint32_t randomState_{0x9e3779b9u};
    std::shared_ptr<std::recursive_mutex> mutex_{std::make_shared<std::recursive_mutex>()};
};

[[nodiscard]] std::size_t utf8PreviousBoundary(std::string_view text, std::size_t offset) noexcept;
[[nodiscard]] std::size_t utf8NextBoundary(std::string_view text, std::size_t offset) noexcept;
