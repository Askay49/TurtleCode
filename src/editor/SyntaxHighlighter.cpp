#include "SyntaxHighlighter.hpp"
#include "core/text/PieceTreeBuffer.hpp"
#include "document/Document.hpp"
#include "language/GrammarManager.hpp"
#include "language/LanguageRegistry.hpp"

#include <QFileInfo>
#include <QFuture>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <utility>
#include <atomic>
#include <QThreadPool>

#ifdef TURTLE_CODE_WITH_TREE_SITTER
#include "language/TreeSitterCpp.hpp"
#endif

namespace {
QThreadPool& syntaxPool() {
    static QThreadPool pool;
    static const bool initialized=[] { pool.setMaxThreadCount(2); pool.setExpiryTimeout(30000); return true; }();
    (void)initialized; return pool;
}
}

struct SyntaxHighlighter::WorkerState {
#ifdef TURTLE_CODE_WITH_TREE_SITTER
    std::unique_ptr<TreeSitterCpp> parser;
    std::string source;
    bool pendingUiReset{false};
    bool discardedRange{false};
    std::size_t discardedFirst{},discardedLast{};
#endif
};

struct SyntaxHighlighter::CancellationState {
    std::atomic<quint64> document{0},viewport{0};
};

SyntaxHighlighter::SyntaxHighlighter(QObject* parent) : QObject(parent), workerState_(std::make_shared<WorkerState>()),
    cancellation_(std::make_shared<CancellationState>()) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(90);
    connect(&debounce_, &QTimer::timeout, this, &SyntaxHighlighter::captureAndQueueSnapshot);
    connect(&watcher_, &QFutureWatcher<ParseResult>::finished,
            this, &SyntaxHighlighter::handleParseFinished);
}

SyntaxHighlighter::~SyntaxHighlighter() {
    cancellation_->document.fetch_add(1,std::memory_order_relaxed);
    debounce_.stop();
    // The worker captures only owned snapshots and shared worker state, never
    // this object or the mutable editor buffer. Its state survives tab closure.
}

void SyntaxHighlighter::setLanguageForPath(const QString& path) {
    const auto* definition = LanguageRegistry::languageForPath(path);
    const QString next = definition && LanguageRegistry::isEnabled(definition->id)
        ? definition->id : QString{};
    bool available = false;
#ifdef TURTLE_CODE_WITH_TREE_SITTER
    available = !next.isEmpty() && GrammarManager::isInstalled(next);
#endif
    if (languageId_ != next || grammarAvailable_ != available) {
        languageId_ = next;
        grammarAvailable_ = available;
        clear();
    }
}

void SyntaxHighlighter::clear() {
    cancellation_->document.fetch_add(1,std::memory_order_relaxed);
    ++revision_;
    ++generation_;
    debounce_.stop();
    buffer_ = nullptr;
    queuedEdits_.clear(); queuedEditBytes_=0;
    hasPendingParse_ = false;
    viewportPending_=true; nextPendingLine_=-1; backgroundComplete_=true;
    needsFullParse_ = true;
    cache_.clear();
    workerCacheBytes_=0; workerCacheRows_=0; lastLimit_=HighlightLimit::None; parseLimited_=false;
    workerState_ = std::make_shared<WorkerState>();
}

void SyntaxHighlighter::setDocumentText(const PieceTreeBuffer& buffer) {
    if (!usesTreeSitter() && !grammarAvailable_) { clear(); return; }
    if(!inputFitsBudget(buffer)) return;
    if (!usesTreeSitter()) { clear(); return; }
    // Loading/replacing a buffer establishes a new base snapshot; the worker discards
    // its previous tree and performs one fresh parse for this revision.
    queuedEdits_.clear(); queuedEditBytes_=0;
    needsFullParse_ = true;
    requestParse(buffer);
}

void SyntaxHighlighter::applyEdit(std::size_t offset, std::size_t startRow, std::size_t startColumn,
                                  std::string_view removed, std::string_view inserted,
                                  const PieceTreeBuffer& buffer) {
    if(!inputFitsBudget(buffer)) return;
    if (!usesTreeSitter()) { clear(); return; }
    if (!needsFullParse_) {
        const auto bytes=sizeof(PendingEdit)+removed.size()+inserted.size();
        if(bytes>limits_.cacheBytes-queuedEditBytes_ || queuedEdits_.size()>=4096) {
            queuedEdits_.clear(); queuedEditBytes_=0; needsFullParse_=true;
        } else {
            queuedEdits_.push_back({offset,startRow,startColumn,std::string(removed),std::string(inserted)});
            queuedEditBytes_+=bytes;
        }
    }
    const auto prefix=buffer.slice(buffer.lineStart(startRow),startColumn);
    const int column=QString::fromUtf8(prefix.data(),static_cast<qsizetype>(prefix.size())).size();
    cache_.projectEdit(HighlightEdit(startRow,column,
        QString::fromUtf8(removed.data(),static_cast<qsizetype>(removed.size())),
        QString::fromUtf8(inserted.data(),static_cast<qsizetype>(inserted.size()))),visibleFirst_,visibleLast_);
    requestParse(buffer);
}

bool SyntaxHighlighter::usesTreeSitter() const noexcept {
    return grammarAvailable_ && !languageId_.isEmpty();
}

void SyntaxHighlighter::invalidateFromLine(std::size_t line) {
    // Keep the last committed styles until a matching range is ready.
    (void)line;
}

QVector<SyntaxHighlighter::Span> SyntaxHighlighter::highlightLine(
    std::size_t lineIndex, std::size_t lineStartByte, const QString& text,
    const std::function<QString(std::size_t)>& lineProvider) {
    (void)lineStartByte;
    (void)text;
    (void)lineProvider;
    if(const auto* spans=cache_.get(lineIndex)) return *spans;
    return {};
}

void SyntaxHighlighter::requestParse(const PieceTreeBuffer& buffer) {
    cancellation_->document.fetch_add(1,std::memory_order_relaxed);
    ++revision_;
    backgroundComplete_=false; parseLimited_=false; lastLimit_=HighlightLimit::None;
    buffer_ = &buffer;
    // Do not clear cache_: keep the previous stable highlighting on screen
    // until a matching range is ready to replace those rows atomically.
    debounce_.start();
}

void SyntaxHighlighter::captureAndQueueSnapshot() {
    if (!buffer_ || !usesTreeSitter()) return;
#ifdef TURTLE_CODE_WITH_TREE_SITTER
    pendingRevision_ = revision_;
    hasPendingParse_ = true;
    if (!jobActive_) startPendingParse();
#endif
}

void SyntaxHighlighter::setVisibleRange(std::size_t firstLine,std::size_t lastLine) {
    if(firstLine>lastLine) return;
    const auto visibleRows=lastLine-firstLine+1;
    const auto margin=std::min<std::size_t>(80,limits_.cacheRows>visibleRows ? (limits_.cacheRows-visibleRows)/4 : 0);
    const auto first=firstLine>margin ? firstLine-margin : 0;
    const auto last=std::min(lastLine+margin,first+limits_.cacheRows-1);
    if(first==visibleFirst_ && last==visibleLast_) return;
    visibleFirst_=first; visibleLast_=last;
    cancellation_->viewport.fetch_add(1,std::memory_order_relaxed);
    if(parseLimited_ || !buffer_ || !usesTreeSitter()) return;
    backgroundComplete_=false;
    viewportPending_=true;
    QTimer::singleShot(0,this,&SyntaxHighlighter::startPendingParse);
}

bool SyntaxHighlighter::highlightingComplete() const {
    return !usesTreeSitter() || (backgroundComplete_ && !hasPendingParse_ && !debounce_.isActive() && !jobActive_);
}

void SyntaxHighlighter::startPendingParse() {
#ifdef TURTLE_CODE_WITH_TREE_SITTER
    if (jobActive_ || debounce_.isActive() || !buffer_ || !usesTreeSitter() || parseLimited_) return;
    const bool parse=hasPendingParse_;
    if(!parse && !viewportPending_ && nextPendingLine_<0) return;
    const quint64 revision=parse ? pendingRevision_ : revision_;
    const quint64 generation=generation_;
    const QString languageId=languageId_;
    const bool priority=viewportPending_ || parse;
    const std::size_t firstLine=priority ? visibleFirst_ : static_cast<std::size_t>(nextPendingLine_);
    // Limit each background job so a new scroll can take priority at the next batch.
    const auto count=buffer_->lineCount();
    const auto workingRows=std::min(limits_.cacheRows,std::max(visibleLast_-visibleFirst_+1,std::min<std::size_t>(2048,limits_.cacheRows/2)));
    const auto center=visibleFirst_+(visibleLast_-visibleFirst_)/2;
    const auto workingFirst=count<=limits_.cacheRows ? 0 : std::min(center>workingRows/2 ? center-workingRows/2 : 0,count>workingRows ? count-workingRows : 0);
    const auto workingLast=count<=limits_.cacheRows ? count-1 : std::min(count-1,workingFirst+workingRows-1);
    const std::size_t lastLine=priority ? visibleLast_ : std::min(firstLine+255,workingLast);
    const auto limits=limits_; const auto pinFirst=visibleFirst_,pinLast=visibleLast_;
    const auto document=document_;
    const PieceTreeBuffer* sourceBuffer=document ? &document->buffer() : buffer_;
    std::vector<PendingEdit> edits;
    if(parse) { edits=std::move(queuedEdits_); queuedEdits_.clear(); queuedEditBytes_=0; }
    const bool forceFullParse=parse && needsFullParse_;
    if(parse) needsFullParse_=false;
    auto state=workerState_;
    const auto cancellation=cancellation_;
    const auto documentEpoch=cancellation->document.load(std::memory_order_relaxed);
    const auto viewportEpoch=cancellation->viewport.load(std::memory_order_relaxed);
    hasPendingParse_=false; viewportPending_=false;
    jobActive_=true;
    watcher_.setFuture(QtConcurrent::run(&syntaxPool(),[revision,generation,languageId,document,sourceBuffer,
                                         edits=std::move(edits),forceFullParse,parse,firstLine,lastLine,
                                         state=std::move(state),cancellation,documentEpoch,viewportEpoch,limits,priority,workingFirst,workingLast,pinFirst,pinLast]() mutable {
        ParseResult result;
        result.revision=revision; result.generation=generation; result.languageId=languageId;
        result.firstLine=firstLine;
        const auto cancelParse=[&] { return cancellation->document.load(std::memory_order_relaxed)!=documentEpoch; };
        const auto cancelQuery=[&] { return cancelParse() || cancellation->viewport.load(std::memory_order_relaxed)!=viewportEpoch; };
        bool rebuild=forceFullParse;
        if(forceFullParse && cancelParse()) { result.fullParse=true; result.cancelled=true; return result; }
        if(parse && forceFullParse) {
            state->source=sourceBuffer ? sourceBuffer->text() : std::string{};
        } else if(parse) {
            for(const auto& edit:edits) {
                if(edit.offset>state->source.size() || edit.removed.size()>state->source.size()-edit.offset
                    || state->source.compare(edit.offset,edit.removed.size(),edit.removed)!=0) {
                    state->source=sourceBuffer ? sourceBuffer->text() : std::string{};
                    edits.clear(); rebuild=true; break;
                }
                state->source.replace(edit.offset,edit.removed.size(),edit.inserted);
            }
        }
        result.fullParse=rebuild;
        if(cancelParse() && !state->parser) { result.cancelled=true; return result; }
        if (!state->parser) state->parser = std::make_unique<TreeSitterCpp>(languageId);
        if (!state->parser->available()) return result;
        state->parser->setLimits(limits);
        if(priority) state->parser->setWorkingRange(workingFirst,workingLast,pinFirst,pinLast);
        // A discarded batch may have filled the worker's cache without reaching
        // the UI. Invalidate it in the old coordinates before replaying edits.
        if(state->discardedRange) {
            state->parser->invalidateRange(state->discardedFirst,state->discardedLast);
            state->discardedRange=false;
        }

        std::vector<TreeSitterCpp::Edit> treeEdits;
        treeEdits.reserve(edits.size());
        for (auto& edit : edits) {
            treeEdits.push_back({edit.offset, edit.startRow, edit.startColumn,
                                 std::move(edit.removed), std::move(edit.inserted)});
        }
        result.fullParse = rebuild;
        const auto parsed = parse ? state->parser->updateRange(state->source,treeEdits,rebuild,firstLine,lastLine,cancelParse,cancelQuery)
                                  : state->parser->highlightRange(firstLine,lastLine,cancelQuery);
        result.cancelled=state->parser->wasCancelled();
        result.limit=state->parser->limitReached();
        result.cacheBytes=state->parser->cachedBytes(); result.cacheRows=state->parser->cachedLineCount();
        if(parse && !state->parser->parseWasCancelled() && state->parser->lastParseWasFull()) state->pendingUiReset=true;
        result.resetCache=state->pendingUiReset;
        result.totalLines=state->parser->lineCount();
        result.nextPendingLine=state->parser->nextPendingLine();
        result.lines.resize(parsed.size());
        for (qsizetype line = 0; line < parsed.size(); ++line) {
            auto& output = result.lines[line];
            output.reserve(parsed[line].size());
            for (const auto& span : parsed[line]) {
                Kind kind = Kind::Keyword;
                switch (span.kind) {
                case TreeSitterCpp::Kind::Comment: kind = Kind::Comment; break;
                case TreeSitterCpp::Kind::String: kind = Kind::String; break;
                case TreeSitterCpp::Kind::Number: kind = Kind::Number; break;
                case TreeSitterCpp::Kind::Type: kind = Kind::Type; break;
                case TreeSitterCpp::Kind::Function: kind = Kind::Function; break;
                case TreeSitterCpp::Kind::Variable: kind = Kind::Variable; break;
                case TreeSitterCpp::Kind::Preprocessor: kind = Kind::Preprocessor; break;
                case TreeSitterCpp::Kind::Constant: kind = Kind::Constant; break;
                case TreeSitterCpp::Kind::Parameter: kind = Kind::Parameter; break;
                case TreeSitterCpp::Kind::Property: kind = Kind::Property; break;
                case TreeSitterCpp::Kind::BuiltinType: kind = Kind::BuiltinType; break;
                case TreeSitterCpp::Kind::Boolean: kind = Kind::Boolean; break;
                case TreeSitterCpp::Kind::Operator: kind = Kind::Operator; break;
                case TreeSitterCpp::Kind::Punctuation: kind = Kind::Punctuation; break;
                case TreeSitterCpp::Kind::Keyword: kind = Kind::Keyword; break;
                }
                output.push_back({span.start, span.length, kind, span.capture});
            }
        }
        return result;
    }));
#endif
}

void SyntaxHighlighter::handleParseFinished() {
#ifdef TURTLE_CODE_WITH_TREE_SITTER
    const ParseResult result = watcher_.result();
    jobActive_=false;
    if (!result.cancelled && result.generation == generation_ && result.revision == revision_ &&
        result.languageId == languageId_ && usesTreeSitter()) {
        if(result.resetCache) { cache_.clear(); workerState_->pendingUiReset=false; }
        for (qsizetype line=0;line<result.lines.size();++line)
            cache_.put(result.firstLine+static_cast<std::size_t>(line),result.lines[line],visibleFirst_,visibleLast_);
        workerCacheBytes_=result.cacheBytes; workerCacheRows_=result.cacheRows;
        if(result.limit==HighlightLimit::ParseTime) {
            cache_.clear(); parseLimited_=true; viewportPending_=false;
        }
        if(result.limit!=HighlightLimit::None && result.limit!=lastLimit_) {
            lastLimit_=result.limit; emit resourceLimitReached(result.limit);
        }
        nextPendingLine_=parseLimited_ ? -1 : result.nextPendingLine;
        backgroundComplete_=nextPendingLine_<0;
        emit highlightingReady();
    } else if (result.fullParse && result.generation == generation_ && result.revision!=revision_) {
        // The base snapshot may have included edits made while it was copied.
        // Rebase from the current document before applying further deltas.
        needsFullParse_ = true;
        queuedEdits_.clear(); queuedEditBytes_=0;
    }
    if(result.generation==generation_ && result.cancelled) emit highlightingCancelled();
    if(result.generation==generation_ && result.revision!=revision_ && !result.lines.isEmpty()) {
        // The worker is idle here; the next job owns this retry metadata.
        workerState_->discardedRange=true;
        workerState_->discardedFirst=result.firstLine;
        workerState_->discardedLast=result.firstLine+static_cast<std::size_t>(result.lines.size())-1;
    }
    // Even if this result became stale, its worker session now owns the tree
    // for that snapshot. All queued deltas are applied to it by the next job.
    if (!debounce_.isActive() && (hasPendingParse_ || viewportPending_ || nextPendingLine_>=0))
        QTimer::singleShot(0,this,&SyntaxHighlighter::startPendingParse);
#endif
}

void SyntaxHighlighter::setLimits(HighlightLimits limits) {
    limits=limits.normalized(); if(limits==limits_) return;
    const auto* buffer=buffer_;
    clear(); limits_=limits; cache_.setLimits(limits_);
    if(buffer) setDocumentText(*buffer);
}

bool SyntaxHighlighter::inputFitsBudget(const PieceTreeBuffer& buffer) {
    // Estimate the current full-text copies plus row offsets before creating any
    // worker snapshot. This is a working-memory budget, not an 8 MiB file gate.
    const auto bytes=buffer.size(),rows=buffer.lineCount(),budget=limits_.sourceWorkingBytes;
    if(bytes<=budget/3 && rows<=(budget-bytes*3)/sizeof(std::size_t)) return true;
    clear(); buffer_=&buffer; parseLimited_=true; lastLimit_=HighlightLimit::SourceMemory;
    emit resourceLimitReached(lastLimit_); emit highlightingReady(); return false;
}
