#pragma once

#include <QObject>
#include "language/HighlightCache.hpp"
#include <QFutureWatcher>
#include <QString>
#include <QTimer>
#include <QVector>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class PieceTreeBuffer;
class Document;
using DocumentPtr = std::shared_ptr<Document>;

// UI-facing adapter. One persistent Tree-sitter session lives on the worker
// pool and receives ordered edits plus immutable UTF-8 snapshots.
class SyntaxHighlighter final : public QObject {
    Q_OBJECT
public:
    enum class Kind { Keyword, Type, Function, Variable, String, Comment, Number, Preprocessor, Constant, Property, BuiltinType, Boolean, Operator, Punctuation, Parameter };
    struct Span { int start{}; int length{}; Kind kind{Kind::Keyword}; QString capture; };

    explicit SyntaxHighlighter(QObject* parent = nullptr);
    ~SyntaxHighlighter() override;
    SyntaxHighlighter(const SyntaxHighlighter&) = delete;
    SyntaxHighlighter& operator=(const SyntaxHighlighter&) = delete;

    void setLanguageForPath(const QString& path);
    void setDocument(DocumentPtr document) { document_ = std::move(document); }
    void setLimits(HighlightLimits limits);
    [[nodiscard]] std::size_t cachedLineCount() const { return cache_.size(); }
    [[nodiscard]] std::size_t cachedBytes() const { return cache_.bytes(); }
    [[nodiscard]] std::size_t workerCachedBytes() const { return workerCacheBytes_; }
    [[nodiscard]] std::size_t workerCachedLineCount() const { return workerCacheRows_; }
    [[nodiscard]] HighlightLimit resourceLimit() const { return lastLimit_; }
    void setVisibleRange(std::size_t firstLine, std::size_t lastLine);
    [[nodiscard]] bool highlightingComplete() const;
    void invalidateFromLine(std::size_t line);
    void clear();
    void setDocumentText(const PieceTreeBuffer& buffer);
    void applyEdit(std::size_t offset, std::size_t startRow, std::size_t startColumn,
                   std::string_view removed, std::string_view inserted, const PieceTreeBuffer& buffer);
    [[nodiscard]] bool usesTreeSitter() const noexcept;
    [[nodiscard]] bool enabled() const noexcept { return usesTreeSitter(); }
    [[nodiscard]] QVector<Span> highlightLine(std::size_t lineIndex, std::size_t lineStartByte,
                                               const QString& text,
                                               const std::function<QString(std::size_t)>& lineProvider);

signals:
    void highlightingReady();
    void highlightingCancelled();
    void resourceLimitReached(HighlightLimit reason);

private:
    struct PendingEdit {
        std::size_t offset{};
        std::size_t startRow{};
        std::size_t startColumn{};
        std::string removed;
        std::string inserted;
    };
    struct WorkerState;
    struct CancellationState;
    struct ParseResult {
        quint64 revision{0};
        quint64 generation{0};
        bool fullParse{false};
        bool cancelled{false};
        QString languageId;
        std::size_t firstLine{0};
        std::size_t totalLines{0};
        int nextPendingLine{-1};
        bool resetCache{false};
        std::size_t cacheBytes{0},cacheRows{0};
        HighlightLimit limit{HighlightLimit::None};
        QVector<QVector<Span>> lines;
    };

    bool inputFitsBudget(const PieceTreeBuffer& buffer);
    void requestParse(const PieceTreeBuffer& buffer);
    void captureAndQueueSnapshot();
    void startPendingParse();
    void handleParseFinished();

    QString languageId_;
    bool grammarAvailable_{false};
    HighlightCache<Span> cache_;
    HighlightLimits limits_;
    std::size_t workerCacheBytes_{0},workerCacheRows_{0};
    HighlightLimit lastLimit_{HighlightLimit::None};
    bool parseLimited_{false};
    const PieceTreeBuffer* buffer_{nullptr};
    DocumentPtr document_;
    QTimer debounce_;
    QFutureWatcher<ParseResult> watcher_;
    quint64 revision_{0};
    quint64 generation_{0};
    quint64 pendingRevision_{0};
    std::vector<PendingEdit> queuedEdits_;
    std::size_t queuedEditBytes_{0};
    std::shared_ptr<WorkerState> workerState_;
    std::shared_ptr<CancellationState> cancellation_;
    bool needsFullParse_{true};
    bool hasPendingParse_{false};
    std::size_t visibleFirst_{0}, visibleLast_{255};
    bool viewportPending_{true};
    int nextPendingLine_{-1};
    bool backgroundComplete_{true};
    bool jobActive_{false};
};
