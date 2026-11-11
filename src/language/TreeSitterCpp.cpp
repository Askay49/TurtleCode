#include "TreeSitterCpp.hpp"
#include "GrammarManager.hpp"
#include "HighlightQueries.hpp"
#include "HighlightCache.hpp"
#include <QElapsedTimer>
#include <set>
#include "LanguageRegistry.hpp"

#include <QDebug>
#include <QFile>
#include <QRegularExpression>
#include <QHash>
#include <QStringList>
#include <queue>
#include <QLibrary>
#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace {
struct DirtyRange { std::size_t first{}; std::size_t last{}; };

TSPoint advancePoint(TSPoint point, std::string_view text) {
    for (const unsigned char c : text) {
        if (c == '\n') { ++point.row; point.column = 0; }
        else ++point.column;
    }
    return point;
}

bool isParameterName(TSNode node);

TreeSitterCpp::Kind captureKind(std::string_view name) {
    using Kind = TreeSitterCpp::Kind;
    if (name.starts_with("keyword.directive") || name.starts_with("keyword.import")) return Kind::Preprocessor;
    if (name.starts_with("type.builtin")) return Kind::BuiltinType;
    if (name.starts_with("character")) return Kind::String;
    if (name.starts_with("comment")) return Kind::Comment;
    if (name.starts_with("string")) return Kind::String;
    if (name.starts_with("number")) return Kind::Number;
    if (name.starts_with("type") || name.starts_with("constructor") || name.starts_with("namespace")) return Kind::Type;
    if (name.starts_with("function") || name.starts_with("method")) return Kind::Function;
    if (name.starts_with("property") || name.starts_with("variable.member") || name.starts_with("field")) return Kind::Property;
    if (name.starts_with("parameter") || name.starts_with("variable.parameter")) return Kind::Parameter;
    if (name.starts_with("variable")) return Kind::Variable;
    if (name.starts_with("constant") || name.starts_with("boolean")) return Kind::Constant;
    if (name.starts_with("operator")) return Kind::Operator;
    if (name.starts_with("punctuation") || name.starts_with("delimiter")) return Kind::Punctuation;
    if (name.starts_with("preprocessor") || name.starts_with("attribute")) return Kind::Preprocessor;
    if (name.starts_with("tag")) return Kind::Type;
    return Kind::Keyword;
}

QString queryString(const TSQuery* query, uint32_t id) {
    uint32_t size = 0; const char* value = ts_query_string_value_for_id(query, id, &size);
    return QString::fromUtf8(value, size);
}
bool acceptsMatch(const TSQuery* query, const TSQueryMatch& match, const std::string& source,
                  QHash<QString, QRegularExpression>& expressions) {
    uint32_t count = 0;
    const auto* steps = ts_query_predicates_for_pattern(query, match.pattern_index, &count);
    auto texts = [&](const TSQueryPredicateStep& step) {
        QStringList values;
        if (step.type == TSQueryPredicateStepTypeString) values.append(queryString(query, step.value_id));
        else for (uint16_t i=0; i<match.capture_count; ++i) if (match.captures[i].index == step.value_id) {
            const auto node = match.captures[i].node;
            const auto begin = ts_node_start_byte(node), end = ts_node_end_byte(node);
            if (end <= source.size()) values.append(QString::fromUtf8(source.data()+begin, end-begin));
        }
        return values;
    };
    for (uint32_t i=0; i<count;) {
        const uint32_t first = i;
        while (i<count && steps[i].type != TSQueryPredicateStepTypeDone) ++i;
        const uint32_t end = i; ++i;
        if (end <= first || steps[first].type != TSQueryPredicateStepTypeString) continue;
        const auto name = queryString(query, steps[first].value_id);
        // Properties/directives describe captures; they are not text filters.
        if (name.endsWith('!') || name == "is?" || name == "is-not?" || name == "not-is?") continue;
        if (end-first < 3) continue;
        if(name.contains("has-parent?") || name.contains("has-ancestor?") || name=="has-declarator-ancestor?") {
            QStringList expected; for(uint32_t j=first+2;j<end;++j) expected.append(texts(steps[j]));
            bool found=false;
            for(uint16_t k=0;k<match.capture_count;++k) if(match.captures[k].index==steps[first+1].value_id) {
                if(name=="has-declarator-ancestor?") { found=isParameterName(match.captures[k].node); break; }
                for(TSNode parent=ts_node_parent(match.captures[k].node);!ts_node_is_null(parent);parent=ts_node_parent(parent)) {
                    if(expected.contains(QString::fromUtf8(ts_node_type(parent)))) { found=true; break; }
                    if(name.contains("has-parent?")) break;
                }
                if(found) break;
            }
            if(name.startsWith("not-")) found=!found;
            if(!found) return false;
            continue;
        }
        const auto left = texts(steps[first+1]);
        if (left.isEmpty()) return false;
        const bool regex = name == "lua-match?" || name == "match?" || name == "not-match?" || name == "any-match?" || name == "any-not-match?";
        const bool equality = name == "eq?" || name == "not-eq?" || name == "any-eq?" || name == "any-not-eq?";
        const bool anyOf = name == "any-of?" || name == "not-any-of?";
        if (!regex && !equality && !anyOf) continue;
        const bool negate = name.contains("not-");
        QStringList right;
        for (uint32_t j=first+2; j<end; ++j) right.append(texts(steps[j]));
        if (right.isEmpty()) return false;
        bool accepted = !name.startsWith("any-") || anyOf;
        for (int item=0; item<left.size(); ++item) {
            bool value;
            if (regex) {
                auto pattern = right.first();
                if(name=="lua-match?") {
                    // Translate the Lua character classes used by the pinned queries.
                    pattern.replace("[%u]","[A-Z]"); pattern.replace("[%l]","[a-z]");
                    pattern.replace("%u","[A-Z]"); pattern.replace("%l","[a-z]");
                    pattern.replace("%d","[0-9]"); pattern.replace("%a","[A-Za-z]"); pattern.replace("%w","[A-Za-z0-9]");
                    pattern.replace("%s","\\s");
                }
                if (!expressions.contains(pattern)) expressions.insert(pattern, QRegularExpression(pattern));
                const auto& expression = expressions[pattern];
                if (!expression.isValid()) return false;
                value = expression.match(left[item]).hasMatch();
            } else if (anyOf) value = right.contains(left[item]);
            else value = left[item] == right[std::min(item, static_cast<int>(right.size())-1)];
            if (negate) value = !value;
            if (name.startsWith("any-") && !anyOf) accepted = accepted || value;
            else accepted = accepted && value;
        }
        if (!accepted) return false;
    }
    return true;
}
bool isParameterName(TSNode node) {
    if (QByteArray(ts_node_type(node)) != "identifier") return false;
    TSNode current = node;
    for (TSNode parent = ts_node_parent(current); !ts_node_is_null(parent); parent = ts_node_parent(current)) {
        const QByteArray type(ts_node_type(parent));
        const TSNode declarator = ts_node_child_by_field_name(parent, "declarator", 10);
        const bool declaredChild = !ts_node_is_null(declarator) && ts_node_eq(declarator, current);
        if (type == "parameter_declaration" || type == "optional_parameter_declaration") return declaredChild;
        // Follow the declared name through pointers, arrays and function pointers.
        // Array bounds and default-value identifiers are not declared names.
        if (!declaredChild && !(type == "parenthesized_declarator" && ts_node_named_child_count(parent) == 1)) return false;
        current = parent;
    }
    return false;
}

int capturePriority(TreeSitterCpp::Kind kind) {
    using Kind=TreeSitterCpp::Kind;
    switch(kind) {
    case Kind::Comment: case Kind::String: return 100;
    case Kind::Function: return 80;
    case Kind::Parameter: return 75;
    case Kind::Property: case Kind::Type: case Kind::BuiltinType: return 70;
    case Kind::Constant: case Kind::Boolean: case Kind::Number: return 60;
    case Kind::Keyword: return 50;
    case Kind::Operator: case Kind::Punctuation: return 40;
    case Kind::Preprocessor: return 20;
    case Kind::Variable: return 10;
    }
    return 0;
}
void resolveOverlaps(QVector<TreeSitterCpp::Span>& spans) {
    using Span=TreeSitterCpp::Span;
    std::stable_sort(spans.begin(), spans.end(), [](const Span& a,const Span& b) { return a.start<b.start; });
    QVector<int> boundaries;
    for (const auto& span:spans) { boundaries.append(span.start); boundaries.append(span.start+span.length); }
    std::sort(boundaries.begin(),boundaries.end()); boundaries.erase(std::unique(boundaries.begin(),boundaries.end()),boundaries.end());
    auto lowerPriority=[&](int a,int b) {
        if (spans[a].priority!=spans[b].priority) return spans[a].priority<spans[b].priority;
        if (spans[a].length!=spans[b].length) return spans[a].length>spans[b].length;
        return a<b;
    };
    std::priority_queue<int,std::vector<int>,decltype(lowerPriority)> active(lowerPriority);
    QVector<Span> output; int next=0;
    for (int i=0;i+1<boundaries.size();++i) {
        const int position=boundaries[i];
        while (next<spans.size() && spans[next].start<=position) active.push(next++);
        while (!active.empty() && spans[active.top()].start+spans[active.top()].length<=position) active.pop();
        if (active.empty()) continue;
        const auto& winner=spans[active.top()]; const int length=boundaries[i+1]-position;
        if (!output.isEmpty() && output.last().kind==winner.kind && output.last().capture==winner.capture && output.last().start+output.last().length==position) output.last().length+=length;
        else output.append({position,length,winner.kind,winner.priority,winner.capture});
    }
    spans=std::move(output);
}

void mergeDirtyRanges(std::vector<DirtyRange>& ranges, std::size_t lineCount) {
    if (lineCount == 0) { ranges.clear(); return; }
    for (auto& range : ranges) {
        range.first = std::min(range.first, lineCount - 1);
        range.last = std::min(std::max(range.first, range.last), lineCount - 1);
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    std::vector<DirtyRange> merged;
    for (const auto& range : ranges) {
        if (merged.empty() || range.first > merged.back().last + 1) merged.push_back(range);
        else merged.back().last = std::max(merged.back().last, range.last);
    }
    ranges = std::move(merged);
}
}

struct TreeSitterCpp::Impl {
    QString languageId;
    QLibrary grammarLibrary;
    TSParser* parser{nullptr};
    TSTree* tree{nullptr};
    TSQuery* query{nullptr};
    QVector<int> patternPriorities;
    QHash<QString, QRegularExpression> predicateExpressions;
    std::size_t sourceSize{0};
    std::string sourceText;
    HighlightCache<Span> lineCache;
    HighlightLimits limits;
    HighlightLimit limit{HighlightLimit::None};
    std::size_t workingFirst{0},workingLast{0},scanNext{0},pinFirst{0},pinLast{255};
    bool cancelled{false}, parseCancelled{false}, fullParse{false}, treeCurrent{false};
    std::vector<std::size_t> lineStarts;


    explicit Impl(QString id) : languageId(std::move(id)), grammarLibrary(GrammarManager::libraryPath(languageId)) {
        if (!grammarLibrary.load()) {
            qInfo() << languageId << "Tree-sitter grammar is not installed:" << grammarLibrary.errorString();
            return;
        }
        using LanguageFunction = const TSLanguage* (*)();
        const auto* definition = LanguageRegistry::language(languageId);
        const QByteArray symbol = definition ? definition->parserSymbol.toUtf8() : QByteArray{};
        auto languageFunction = reinterpret_cast<LanguageFunction>(grammarLibrary.resolve(symbol.constData()));
        if (!languageFunction) {
            qWarning() << languageId << "Tree-sitter grammar module has no parser symbol" << symbol;
            grammarLibrary.unload();
            return;
        }
        const TSLanguage* language = languageFunction();
        parser = ts_parser_new();
        if (!parser || !language || !ts_parser_set_language(parser, language)) return;

        QByteArray queryBytes;
        if(languageId=="c" || languageId=="cpp") queryBytes=HighlightQueries::compatibleSource(language,languageId);
        else { QFile file(GrammarManager::queryPath(languageId)); if(file.open(QIODevice::ReadOnly)) queryBytes=file.readAll(); }
        uint32_t errorOffset = 0;
        TSQueryError errorType{};
        query = queryBytes.isEmpty() ? nullptr : ts_query_new(
            language, queryBytes.constData(), static_cast<uint32_t>(queryBytes.size()), &errorOffset, &errorType);
        if(query) {
            patternPriorities.fill(100,ts_query_pattern_count(query));
            for(uint32_t pattern=0;pattern<ts_query_pattern_count(query);++pattern) {
                uint32_t count=0; const auto* steps=ts_query_predicates_for_pattern(query,pattern,&count);
                for(uint32_t i=0;i+2<count;++i) if(steps[i].type==TSQueryPredicateStepTypeString && queryString(query,steps[i].value_id)=="set!" &&
                    steps[i+1].type==TSQueryPredicateStepTypeString && queryString(query,steps[i+1].value_id)=="priority") {
                    bool ok=false; const int priority=queryString(query,steps[i+2].value_id).toInt(&ok); if(ok) patternPriorities[pattern]=priority;
                }
            }
        }
        if (!query) {
            qWarning() << languageId << "Tree-sitter highlight query failed at byte" << errorOffset
                       << "with error code" << static_cast<int>(errorType);
        }
    }

    ~Impl() {
        if (query) ts_query_delete(query);
        if (tree) ts_tree_delete(tree);
        if (parser) ts_parser_delete(parser);
    }

    void resetTree() {
        if (tree) { ts_tree_delete(tree); tree = nullptr; }
        treeCurrent=false;
        sourceSize = 0;
        std::string{}.swap(sourceText);
        lineCache.clear();
        std::vector<std::size_t>{}.swap(lineStarts); scanNext=workingFirst;
    }

    void applyCacheSplice(const Edit& edit, TSPoint oldEnd, TSPoint newEnd) {
        const auto oldCount=static_cast<std::size_t>(oldEnd.row)-edit.startRow+1;
        const auto newCount=static_cast<std::size_t>(newEnd.row)-edit.startRow+1;
        lineCache.splice(edit.startRow,oldCount,newCount);
    }

    QVector<QVector<Span>> update(const std::string& source, const std::vector<Edit>& edits, bool forceFullParse, std::size_t firstLine, std::size_t lastLine,
        const CancelCheck& cancelParse = {}, const CancelCheck& cancelQuery = {}) {
        cancelled=false; parseCancelled=false; limit=HighlightLimit::None;
        if (!parser || !query || source.size() > std::numeric_limits<uint32_t>::max()) return {};

        treeCurrent=false;
        std::vector<DirtyRange> dirty;
        bool full = forceFullParse || !tree;
        if (!full) {
            for (const auto& edit : edits) {
                if (edit.offset > sourceSize || edit.removed.size() > sourceSize - edit.offset ||
                    edit.offset > sourceText.size() || edit.removed.size() > sourceText.size() - edit.offset ||
                    sourceText.compare(edit.offset, edit.removed.size(), edit.removed) != 0) {
                    full = true;
                    break;
                }
                const TSPoint startPoint{static_cast<uint32_t>(edit.startRow), static_cast<uint32_t>(edit.startColumn)};
                const TSPoint oldEndPoint = advancePoint(startPoint, edit.removed);
                const TSPoint newEndPoint = advancePoint(startPoint, edit.inserted);
                if (oldEndPoint.row < startPoint.row || newEndPoint.row < startPoint.row) {
                    full = true;
                    break;
                }
                TSInputEdit tsEdit{};
                tsEdit.start_byte = static_cast<uint32_t>(edit.offset);
                tsEdit.old_end_byte = static_cast<uint32_t>(edit.offset + edit.removed.size());
                tsEdit.new_end_byte = static_cast<uint32_t>(edit.offset + edit.inserted.size());
                tsEdit.start_point = startPoint;
                tsEdit.old_end_point = oldEndPoint;
                tsEdit.new_end_point = newEndPoint;
                ts_tree_edit(tree, &tsEdit);
                sourceText.replace(edit.offset, edit.removed.size(), edit.inserted);
                sourceSize = sourceSize - edit.removed.size() + edit.inserted.size();
                applyCacheSplice(edit, oldEndPoint, newEndPoint);
                // Earlier invalidated ranges use the coordinates before this
                // edit. Keep their bounds aligned during multi-cursor batches.
                const auto shift=[&](std::size_t row,bool end) {
                    if(row<edit.startRow) return row;
                    if(row<=oldEndPoint.row) return end ? static_cast<std::size_t>(newEndPoint.row) : edit.startRow;
                    return row-static_cast<std::size_t>(oldEndPoint.row)+static_cast<std::size_t>(newEndPoint.row);
                };
                for(auto& range:dirty) { range.first=shift(range.first,false); range.last=shift(range.last,true); }

                dirty.push_back({edit.startRow, static_cast<std::size_t>(newEndPoint.row)});

                // A changed token can be nested inside a larger captured node
                // (notably a multi-line comment/string). Expand the query range
                // to the smallest named node that contains the edit.
                if (tree && sourceSize > 0) {
                    const uint32_t changedStart = static_cast<uint32_t>(std::min(edit.offset, sourceSize - 1));
                    const uint32_t changedEnd = static_cast<uint32_t>(std::min(
                        std::max(edit.offset + edit.inserted.size(), edit.offset + 1), sourceSize));
                    const TSNode affected = ts_node_named_descendant_for_byte_range(
                        ts_tree_root_node(tree), changedStart, changedEnd);
                    // Insertions between top-level declarations have no old
                    // child covering the new bytes. The containing root is not
                    // a capture: reparsing it would recolor the whole file.
                    // Structural changes are covered by changed ranges below.
                    if (!ts_node_is_null(affected) && !ts_node_is_null(ts_node_parent(affected))) {
                        const TSPoint nodeStart = ts_node_start_point(affected);
                        const TSPoint nodeEnd = ts_node_end_point(affected);
                        dirty.push_back({nodeStart.row, nodeEnd.row});
                    }
                }
            }
            if (sourceText != source || sourceSize != source.size()) full = true;
        }

        TSTree* oldTree = nullptr;
        if (full) {
            resetTree();
            lineCache.clear();
        } else {
            oldTree = tree; // already edited to match the new source
        }

        fullParse=full;
        // Preserve invalidated rows even when a newer revision interrupts this
        // parse. The already edited old tree can still be reused by the next one.
        if(!full && !lineStarts.empty()) {
            for(const auto& range:dirty) lineCache.invalidate(range.first,range.last);
        }
        TSTree* next=nullptr;
        QElapsedTimer parseClock; parseClock.start();
#if !TURTLE_CODE_TS_PARSE_WITH_OPTIONS
        ts_parser_set_timeout_micros(parser,5000);
#endif
        do {
            if(cancelParse && cancelParse()) {
                cancelled=true; parseCancelled=true;
                ts_parser_reset(parser);
                return {};
            }
#if TURTLE_CODE_TS_PARSE_WITH_OPTIONS
            // New runtimes replace parser timeouts with a resumable progress
            // callback. Keep the same five-millisecond cooperative slices.
            QElapsedTimer sliceClock; sliceClock.start();
            std::string_view inputSource(source);
            TSInput input{};
            input.payload=&inputSource;
            input.encoding=TSInputEncodingUTF8;
            input.read=[](void* payload,uint32_t offset,TSPoint,uint32_t* length)->const char* {
                const auto& text=*static_cast<const std::string_view*>(payload);
                *length=offset<text.size()?static_cast<uint32_t>(text.size()-offset):0;
                return *length?text.data()+offset:nullptr;
            };
            TSParseOptions options{};
            options.payload=&sliceClock;
            options.progress_callback=[](TSParseState* state) {
                return static_cast<QElapsedTimer*>(state->payload)->elapsed()>=5;
            };
            next=ts_parser_parse_with_options(parser,oldTree,input,options);
#else
            next=ts_parser_parse_string(parser,oldTree,source.data(),static_cast<uint32_t>(source.size()));
#endif
            if(parseClock.elapsed()>=limits.parseTimeMs) {
                if(next) ts_tree_delete(next);
                ts_parser_reset(parser); resetTree();
                // Also release native parser scratch allocations after a limit.
                const auto* language=ts_parser_language(parser);
                ts_parser_delete(parser); parser=ts_parser_new();
                if(parser) ts_parser_set_language(parser,language);
                limit=HighlightLimit::ParseTime; return {};
            }
        } while(!next);
#if !TURTLE_CODE_TS_PARSE_WITH_OPTIONS
        ts_parser_set_timeout_micros(parser,0);
#endif
        if (!next) {
            ts_parser_reset(parser); resetTree(); return {};
        }

        if (oldTree) {
            uint32_t changedCount = 0;
            TSRange* changed = ts_tree_get_changed_ranges(oldTree, next, &changedCount);
            for (uint32_t i = 0; i < changedCount; ++i) {
                dirty.push_back({changed[i].start_point.row, changed[i].end_point.row});
            }
            std::free(changed);
            ts_tree_delete(oldTree);
        } else {
            full = true;
        }
        tree = next;
        sourceText = source;
        sourceSize = source.size();
        treeCurrent=true;

        lineStarts.assign(1,0);
        for (std::size_t i = 0; i < source.size(); ++i)
            if (source[i] == '\n') lineStarts.push_back(i + 1);
        const std::size_t lineCount = lineStarts.size();

        if (full) {
            dirty.clear();
            dirty.push_back({0, lineCount - 1});
        }
        mergeDirtyRanges(dirty, lineCount);
        for (const auto& range : dirty) lineCache.invalidate(range.first,range.last);
        scanNext=workingFirst;
        return highlightRange(firstLine,lastLine,cancelQuery);
    }

    QVector<QVector<Span>> highlightRange(std::size_t firstLine,std::size_t lastLine,const CancelCheck& cancel = {}) {
        cancelled=false; parseCancelled=false; limit=HighlightLimit::None;
        QElapsedTimer queryClock; queryClock.start();
        auto interrupted=[&] { cancelled=cancel && cancel(); return cancelled; };
        if(interrupted()) return {};
        const auto& source=sourceText;
        if (!treeCurrent || !tree || firstLine>=lineStarts.size() || firstLine>lastLine) return {};
        const auto batchRows=std::min(limits.cacheRows,std::max<std::size_t>(1,limits.queryBytes/192));
        lastLine=std::min({lastLine,lineStarts.size()-1,firstLine+batchRows-1});
        std::map<std::size_t,QVector<Span>> work;
        std::set<std::size_t> denseRows;
        std::size_t queryBytes=0;
        std::vector<DirtyRange> dirty;
        for(std::size_t row=firstLine;row<=lastLine;) {
            if(lineCache.contains(row)) { ++row; continue; }
            const auto begin=row;
            while(row<=lastLine && !lineCache.contains(row)) ++row;
            dirty.push_back({begin,row-1});
        }
        bool limited=false;
        for (const auto& range : dirty) {
            const std::size_t byteStart = lineStarts[range.first];
            const std::size_t byteEnd = (range.last + 1 < lineStarts.size())
                ? lineStarts[range.last + 1] : source.size();
            if (byteEnd <= byteStart) continue;
            TSQueryCursor* cursor = ts_query_cursor_new();
            if (!cursor) continue;
            ts_query_cursor_set_byte_range(cursor, static_cast<uint32_t>(byteStart), static_cast<uint32_t>(byteEnd));
            ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));
            TSQueryMatch match{};
            while (ts_query_cursor_next_match(cursor, &match)) {
                if(interrupted()) { ts_query_cursor_delete(cursor); return {}; }
                if(queryClock.elapsed()>=limits.queryTimeMs) { limit=HighlightLimit::QueryTime; limited=true; break; }
                if (!acceptsMatch(query, match, source, predicateExpressions)) continue;
                for (uint16_t captureIndex=0; captureIndex<match.capture_count; ++captureIndex) {
                const TSNode node = match.captures[captureIndex].node;
                const std::size_t nodeStart = ts_node_start_byte(node);
                std::size_t nodeEnd = ts_node_end_byte(node);
                if (nodeEnd <= nodeStart || nodeEnd <= byteStart || nodeStart >= byteEnd) continue;
                uint32_t nameLength = 0;
                const char* name = ts_query_capture_name_for_id(query, match.captures[captureIndex].index, &nameLength);
                const auto capture=QString::fromUtf8(name,nameLength);
                if(capture.startsWith('_') || capture=="spell") continue;
                Kind kind = captureKind(std::string_view(name ? name : "", nameLength));
                const QByteArray nodeType(ts_node_type(node));
                if (kind == Kind::Type && (nodeType == "primitive_type" || nodeType == "sized_type_specifier")) kind = Kind::BuiltinType;
                if (kind == Kind::Constant && (nodeType == "true" || nodeType == "false" || nodeType == "null")) kind = Kind::Boolean;
                const int priority = (languageId=="c" || languageId=="cpp") ? patternPriorities.value(match.pattern_index,100) : capturePriority(kind);
                // Conditional directive nodes contain their entire branch bodies.
                // Color the directive line, not all code nested below #if/#else.
                if (kind == Kind::Preprocessor && (nodeType == "preproc_if" || nodeType == "preproc_ifdef" || nodeType == "preproc_else" || nodeType == "preproc_elif")) {
                    const auto newline = source.find('\n', nodeStart);
                    if (newline != std::string::npos) nodeEnd = std::min(nodeEnd, newline);
                }
                const std::size_t firstLine = static_cast<std::size_t>(
                    std::upper_bound(lineStarts.begin(), lineStarts.end(), std::max(nodeStart, byteStart)) - lineStarts.begin() - 1);
                const std::size_t lastByte = std::min(nodeEnd, byteEnd) - 1;
                const std::size_t lastLine = static_cast<std::size_t>(
                    std::upper_bound(lineStarts.begin(), lineStarts.end(), lastByte) - lineStarts.begin() - 1);
                for (std::size_t line = std::max(firstLine, range.first);
                     line <= std::min(lastLine, range.last); ++line) {
                    if((line&127)==0 && interrupted()) { ts_query_cursor_delete(cursor); return {}; }
                    const std::size_t lineStart = lineStarts[line];
                    const std::size_t lineEnd = line + 1 < lineStarts.size() ? lineStarts[line + 1] - 1 : source.size();
                    if(denseRows.contains(line)) continue;
                    if(lineEnd-lineStart>limits.maxLineBytes) {
                        denseRows.insert(line); work[line]={}; limit=HighlightLimit::SpanMemory; continue;
                    }
                    const std::size_t segmentStart = std::max(nodeStart, lineStart);
                    const std::size_t segmentEnd = std::min(nodeEnd, lineEnd);
                    if (segmentEnd <= segmentStart) continue;
                    const int localStart = QString::fromUtf8(
                        source.data() + lineStart, static_cast<qsizetype>(segmentStart - lineStart)).size();
                    const int localEnd = QString::fromUtf8(
                        source.data() + lineStart, static_cast<qsizetype>(segmentEnd - lineStart)).size();
                    if(localEnd>localStart && !denseRows.contains(line)) {
                        auto& spans=work[line];
                        if(static_cast<std::size_t>(spans.size())>=limits.maxSpansPerLine) {
                            denseRows.insert(line); spans={}; limit=HighlightLimit::SpanMemory;
                        } else {
                            // Double the logical payload estimate to cover vector growth.
                            queryBytes+=2*(sizeof(Span)+64+capture.size()*sizeof(QChar));
                            if(queryBytes>limits.queryBytes) { limit=HighlightLimit::SpanMemory; limited=true; break; }
                            spans.append({localStart,localEnd-localStart,kind,priority,capture});
                        }
                    }
                }
                if(limited) break;
                }
                if(limited) break;
            }
            ts_query_cursor_delete(cursor);
            if(limited) break;
        }

        if(interrupted()) return {};
        QVector<QVector<Span>> output;
        output.reserve(static_cast<qsizetype>(lastLine-firstLine+1));
        for(std::size_t line=firstLine;line<=lastLine;++line) {
            if(const auto* cached=lineCache.get(line)) { output.append(*cached); continue; }
            auto spans=limited ? QVector<Span>{} : std::move(work[line]);
            resolveOverlaps(spans);
            if(!lineCache.put(line,spans,pinFirst,pinLast)) { spans={}; limit=HighlightLimit::SpanMemory; }
            output.append(std::move(spans));
        }
        if(firstLine<=scanNext) scanNext=std::max(scanNext,lastLine+1);
        return output;
    }
};

TreeSitterCpp::TreeSitterCpp(QString languageId) : impl_(std::make_unique<Impl>(std::move(languageId))) {}
TreeSitterCpp::~TreeSitterCpp() = default;
bool TreeSitterCpp::available() const noexcept { return impl_ && impl_->parser && impl_->query; }

void TreeSitterCpp::reload() {
    impl_ = std::make_unique<Impl>(impl_ ? impl_->languageId : QStringLiteral("cpp"));
}

void TreeSitterCpp::reset() noexcept {
    if (impl_) impl_->resetTree();
}

QVector<QVector<TreeSitterCpp::Span>> TreeSitterCpp::update(
    const std::string& source, const std::vector<Edit>& edits, bool forceFullParse) {
    if (!available()) return {};
    return impl_->update(source, edits, forceFullParse, 0, std::numeric_limits<std::size_t>::max());
}

QVector<QVector<TreeSitterCpp::Span>> TreeSitterCpp::updateRange(
    const std::string& source,const std::vector<Edit>& edits,bool forceFullParse,
    std::size_t firstLine,std::size_t lastLine,const CancelCheck& cancelParse,const CancelCheck& cancelQuery) {
    if(!available()) return {};
    return impl_->update(source,edits,forceFullParse,firstLine,lastLine,cancelParse,cancelQuery);
}
QVector<QVector<TreeSitterCpp::Span>> TreeSitterCpp::highlightRange(std::size_t firstLine,std::size_t lastLine,const CancelCheck& cancel) {
    if(!available()) return {};
    return impl_->highlightRange(firstLine,lastLine,cancel);
}
std::size_t TreeSitterCpp::lineCount() const noexcept { return impl_ ? impl_->lineStarts.size() : 0; }
int TreeSitterCpp::nextPendingLine() const noexcept {
    if(!impl_ || impl_->lineStarts.empty()) return -1;
    const auto last=std::min(impl_->workingLast,impl_->lineStarts.size()-1);
    for(auto row=std::max(impl_->scanNext,impl_->workingFirst);row<=last;++row)
        if(!impl_->lineCache.contains(row)) return static_cast<int>(row);
    return -1;
}
void TreeSitterCpp::invalidateRange(std::size_t firstLine,std::size_t lastLine) {
    if(impl_) { impl_->lineCache.invalidate(firstLine,lastLine); impl_->scanNext=impl_->workingFirst; }
}
void TreeSitterCpp::setLimits(HighlightLimits limits) {
    impl_->limits=limits.normalized(); impl_->lineCache.setLimits(impl_->limits);
}
void TreeSitterCpp::setWorkingRange(std::size_t first,std::size_t last,std::size_t pinFirst,std::size_t pinLast) {
    impl_->workingFirst=first; impl_->workingLast=last; impl_->scanNext=first;
    impl_->pinFirst=pinFirst; impl_->pinLast=pinLast;
}
std::size_t TreeSitterCpp::cachedLineCount() const noexcept { return impl_->lineCache.size(); }
std::size_t TreeSitterCpp::cachedBytes() const noexcept { return impl_->lineCache.bytes(); }
HighlightLimit TreeSitterCpp::limitReached() const noexcept { return impl_->limit; }

bool TreeSitterCpp::wasCancelled() const noexcept { return impl_ && impl_->cancelled; }
bool TreeSitterCpp::parseWasCancelled() const noexcept { return impl_ && impl_->parseCancelled; }
bool TreeSitterCpp::lastParseWasFull() const noexcept { return impl_ && impl_->fullParse; }
