#include "editor/SyntaxHighlighter.hpp"
#include "core/text/PieceTreeBuffer.hpp"
#include "language/GrammarManager.hpp"
#include "language/TreeSitterCpp.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimer>
#include <QThread>
#include <cstdlib>
#include <iostream>
#include <string>

static void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

static void waitForHighlight(SyntaxHighlighter& highlighter) {
    QEventLoop loop;
    QObject::connect(&highlighter, &SyntaxHighlighter::highlightingReady,
                     &loop, &QEventLoop::quit);
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();
}

static bool hasKind(const QVector<SyntaxHighlighter::Span>& spans, SyntaxHighlighter::Kind kind) {
    for (const auto& span : spans) if (span.kind == kind) return true;
    return false;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("Turtle Code");
    QCoreApplication::setOrganizationName("Turtle Code");
    for (const auto& language : {QStringLiteral("c"), QStringLiteral("cpp")}) {
        if (!GrammarManager::isInstalled(language)) continue;
        const std::string sample =
            "void system_clock_config(void) {\n"
            "  RCC_OscInitTypeDef RCC_OscInitStruct = {0};\n"
            "  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;\n"
            "  RCC_OscInitStruct.PLL.PLLM = 25;\n"
            "  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();\n"
            "}\n"
            "#if FLAG\n"
            "int local = 1;\n"
            "#endif\n"
            "#include \"dataformatter.h\"\n"
            "#include <QLabel>\n"
            "void pin_set(gpio_pin_t gpio, uint32_t state);\n"
            "void pointers(const char *name, int values[COUNT], void (*callback)(int code));\n";
        TreeSitterCpp parser(language);
        require(parser.available(), "classification grammar is available");
        const auto lines = parser.update(sample, {}, true);
        auto tokenKind = [&](int line, const QString& token, TreeSitterCpp::Kind kind) {
            const auto text = QString::fromStdString(sample).split('\n').at(line);
            const int position = text.indexOf(token);
            require(position >= 0, "classification test token exists");
            for (const auto& span : lines.at(line))
                if (span.start <= position && span.start + span.length >= position + token.size() && span.kind == kind) return true;
            return false;
        };
        require(tokenKind(0, "void", TreeSitterCpp::Kind::BuiltinType), "primitive types have their own category");
        require(tokenKind(0, "system_clock_config", TreeSitterCpp::Kind::Function), "function declarations override generic variables");
        require(tokenKind(1, "RCC_OscInitTypeDef", TreeSitterCpp::Kind::Type), "user types are distinct from primitive types");
        require(tokenKind(2, "RCC_OscInitStruct", TreeSitterCpp::Kind::Variable), "mixed-case variables do not match the constant predicate");
        require(tokenKind(2, "OscillatorType", TreeSitterCpp::Kind::Property), "struct members have their own category");
        require(tokenKind(2, "RCC_OSCILLATORTYPE_HSE", TreeSitterCpp::Kind::Constant), "uppercase macros override generic variables");
        require(tokenKind(3, "PLLM", TreeSitterCpp::Kind::Property), "nested members remain properties");
        require(tokenKind(3, "25", TreeSitterCpp::Kind::Number), "numeric literals keep their own category");
        require(tokenKind(4, "HAL_RCC_OscConfig", language=="cpp" ? TreeSitterCpp::Kind::Type : TreeSitterCpp::Kind::Function),
            "upstream C++ queries classify capitalized calls as constructors until LSP resolves the symbol");
        require(tokenKind(4, "HAL_OK", TreeSitterCpp::Kind::Constant), "constant predicates are evaluated");
        require(tokenKind(7, "local", TreeSitterCpp::Kind::Variable), "conditional directives do not color their branch bodies");
        require(tokenKind(9, "\"dataformatter.h\"", TreeSitterCpp::Kind::String), "quoted include paths use the string color");
        require(tokenKind(10, "<QLabel>", TreeSitterCpp::Kind::String), "angle-bracket include paths use the string color");
        require(tokenKind(11, "gpio_pin_t", TreeSitterCpp::Kind::Type), "parameter types retain their type category");
        require(tokenKind(11, "gpio,", TreeSitterCpp::Kind::Parameter) == false, "parameter color excludes punctuation");
        require(tokenKind(11, "state", TreeSitterCpp::Kind::Parameter), "scalar parameter names are recognized");
        // Find gpio by its declaration offset, avoiding the substring in gpio_pin_t.
        const auto declaration = QStringLiteral("void pin_set(gpio_pin_t gpio, uint32_t state);");
        const int gpioStart = declaration.indexOf(" gpio,") + 1;
        bool gpioParameter = false;
        for (const auto& span : lines.at(11)) if (span.kind == TreeSitterCpp::Kind::Parameter && span.start <= gpioStart && span.start + span.length >= gpioStart + 4) gpioParameter = true;
        require(gpioParameter, "user-defined type parameter names are recognized");
        require(tokenKind(12, "name", TreeSitterCpp::Kind::Parameter), "pointer parameters are recognized");
        require(tokenKind(12, "values", TreeSitterCpp::Kind::Parameter), "array parameters are recognized");
        require(tokenKind(12, "COUNT", TreeSitterCpp::Kind::Constant), "array size constants are not parameter names");
        require(tokenKind(12, "callback", TreeSitterCpp::Kind::Parameter), "function-pointer parameters are recognized");
        require(tokenKind(12, "code", TreeSitterCpp::Kind::Parameter), "nested function parameters are recognized");
        for (const auto& spans : lines) for (int i=1;i<spans.size();++i)
            require(spans[i-1].start+spans[i-1].length<=spans[i].start, "resolved spans never overlap");
    }
    if (!GrammarManager::isInstalled(QStringLiteral("cpp"))) return 0;
    const std::string source =
        "#include <vector>\n"
        "struct Widget { int value = 42; bool enabled = true; bool disabled = false; void* ptr = nullptr; };\n"
        "int main() {\n"
        "  // comment\n"
        "  Widget item; item.run();\n"
        "  const char* text = \"hello\";\n"
        "  auto café = 7; return café;\n"
        "}\n";
    PieceTreeBuffer buffer(source);
    SyntaxHighlighter highlighter;
    highlighter.setLanguageForPath(QStringLiteral("sample.cpp"));
    require(highlighter.usesTreeSitter(), "C++ files use the configured Tree-sitter parser");
    highlighter.setDocumentText(buffer);
    waitForHighlight(highlighter);

    auto getLine = [&](std::size_t line) {
        const auto bytes = buffer.line(line);
        const auto start = buffer.lineStart(line);
        const QString text = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
        return highlighter.highlightLine(line, start, text, [](std::size_t) { return QString{}; });
    };

    require(hasKind(getLine(0), SyntaxHighlighter::Kind::Preprocessor), "preprocessor include is captured");
    require(hasKind(getLine(1), SyntaxHighlighter::Kind::Keyword), "C++ keyword is captured");
    require(hasKind(getLine(1), SyntaxHighlighter::Kind::Type), "type identifier is captured");
    require(hasKind(getLine(1), SyntaxHighlighter::Kind::Constant), "boolean and nullptr constants are captured");
    require(hasKind(getLine(3), SyntaxHighlighter::Kind::Comment), "line comment is captured");
    require(hasKind(getLine(4), SyntaxHighlighter::Kind::Function), "member function call is captured");
    require(hasKind(getLine(5), SyntaxHighlighter::Kind::String), "string literal is captured");
    const auto unicodeLine = buffer.line(6);
    const QString unicodeText = QString::fromUtf8(unicodeLine.data(), static_cast<qsizetype>(unicodeLine.size()));
    const auto unicodeSpans = highlighter.highlightLine(6, buffer.lineStart(6), unicodeText,
        [](std::size_t) { return QString{}; });
    require(hasKind(unicodeSpans, SyntaxHighlighter::Kind::Number), "number literal is captured after Unicode text");
    bool returnOffsetMatches = false;
    for (const auto& span : unicodeSpans) {
        if (span.kind == SyntaxHighlighter::Kind::Keyword &&
            unicodeText.mid(span.start, span.length) == QStringLiteral("return"))
            returnOffsetMatches = span.start == unicodeText.indexOf(QStringLiteral("return"));
    }
    require(returnOffsetMatches, "Tree-sitter UTF-8 byte offsets map to Qt UTF-16 columns");

    const std::string inserted = "// inserted\n";
    const std::size_t editOffset = buffer.lineStart(3);
    buffer.insert(editOffset, inserted);
    highlighter.applyEdit(editOffset, 3, 0, {}, inserted, buffer);
    require(hasKind(getLine(4),SyntaxHighlighter::Kind::Comment),"existing comment colors survive insertion before the worker replies");
    require(hasKind(getLine(5),SyntaxHighlighter::Kind::Function),"unchanged lines retain styles before the worker replies");
    waitForHighlight(highlighter);
    require(hasKind(getLine(3), SyntaxHighlighter::Kind::Comment), "background parse updates syntax after edit");
    require(hasKind(getLine(4), SyntaxHighlighter::Kind::Comment), "line indexes remain correct after newline insertion");
    require(hasKind(getLine(5), SyntaxHighlighter::Kind::Function), "later lines are re-queried after newline insertion");

    // Queue two edits against successive buffer revisions before the debounce
    // fires. The worker must apply both TSInputEdits to its retained old tree.
    const std::size_t returnLine = 7;
    const std::string returnText = buffer.line(returnLine);
    const auto returnColumn = returnText.find("return");
    require(returnColumn != std::string::npos, "incremental test locates return keyword");
    const std::size_t returnOffset = buffer.lineStart(returnLine) + returnColumn;
    const std::string oldKeyword = "return";
    const std::string newKeyword = "co_return";
    buffer.erase(returnOffset, oldKeyword.size());
    buffer.insert(returnOffset, newKeyword);
    highlighter.applyEdit(returnOffset, returnLine, returnColumn, oldKeyword, newKeyword, buffer);

    const std::size_t headerOffset = buffer.lineStart(1);
    const std::string header = "// incremental header\n";
    buffer.insert(headerOffset, header);
    highlighter.applyEdit(headerOffset, 1, 0, {}, header, buffer);
    waitForHighlight(highlighter);
    const auto shiftedKeywordLine = getLine(8);
    bool coReturnHighlighted = false;
    const auto shiftedKeywordBytes = buffer.line(8);
    const QString shiftedKeywordText = QString::fromUtf8(
        shiftedKeywordBytes.data(), static_cast<qsizetype>(shiftedKeywordBytes.size()));
    for (const auto& span : shiftedKeywordLine) {
        if (span.kind == SyntaxHighlighter::Kind::Keyword &&
            shiftedKeywordText.mid(span.start, span.length) == QStringLiteral("co_return")) {
            coReturnHighlighted = true;
            break;
        }
    }
    require(coReturnHighlighted, "batched incremental edits preserve correct syntax after line shifts");

    const std::string blockComment = "/* first\n * middle\n * last */\n";
    const std::size_t blockStart = buffer.size();
    const std::size_t blockStartRow = buffer.lineIndexAt(blockStart);
    buffer.insert(blockStart, blockComment);
    highlighter.applyEdit(blockStart, blockStartRow, 0, {}, blockComment, buffer);
    waitForHighlight(highlighter);
    const std::size_t middleRow = blockStartRow + 1;
    const std::string middleLine = buffer.line(middleRow);
    const auto middleColumn = middleLine.find("middle");
    require(middleColumn != std::string::npos, "incremental test locates text inside block comment");
    const std::size_t middleOffset = buffer.lineStart(middleRow) + middleColumn;
    buffer.erase(middleOffset, 6);
    buffer.insert(middleOffset, "changed");
    highlighter.applyEdit(middleOffset, middleRow, middleColumn, "middle", "changed", buffer);
    waitForHighlight(highlighter);
    const auto middleBytes = buffer.line(middleRow);
    const QString middleText = QString::fromUtf8(middleBytes.data(), static_cast<qsizetype>(middleBytes.size()));
    require(hasKind(highlighter.highlightLine(middleRow, buffer.lineStart(middleRow), middleText,
                [](std::size_t) { return QString{}; }), SyntaxHighlighter::Kind::Comment),
            "editing inside a multi-line capture refreshes the whole captured node");

    highlighter.setLanguageForPath(QStringLiteral("notes.txt"));
    require(!highlighter.usesTreeSitter(), "unsupported files do not invoke a fallback lexer");
    require(getLine(0).isEmpty(), "unsupported files stay plain text");

    {
        std::string large;
        for(int row=0;row<10'000;++row) {
            if(row==1000) large+="/* comment starts outside the viewport\n";
            else if(row>1000 && row<5000) large+="still inside the comment\n";
            else if(row==5000) large+="*/\n";
            else if(row==6000) large+="const char* raw = R\"tag(\n";
            else if(row>6000 && row<8000) large+="still inside the raw string\n";
            else if(row==8000) large+=")tag\";\n";
            else large+="int value = 42;\n";
        }
        PieceTreeBuffer document(large);
        SyntaxHighlighter viewport;
        HighlightLimits comparisonLimits; comparisonLimits.cacheRows=12000; comparisonLimits.cacheBytes=32u*1024u*1024u;
        comparisonLimits.queryBytes=comparisonLimits.cacheBytes; comparisonLimits.queryTimeMs=1000;
        viewport.setLimits(comparisonLimits);
        viewport.setLanguageForPath("viewport.cpp");
        viewport.setVisibleRange(3000,3010);
        viewport.setDocumentText(document);
        waitForHighlight(viewport);
        auto row=[&](std::size_t line) {
            return viewport.highlightLine(line,0,{},[](std::size_t){return QString{};});
        };
        require(hasKind(row(3000),SyntaxHighlighter::Kind::Comment),"first batch has context from a comment beginning off screen");
        require(!row(2920).isEmpty() && !row(3090).isEmpty(),"visible batch includes 80 rows of margin");
        require(row(2919).isEmpty() && row(3091).isEmpty() && row(0).isEmpty(),"initial query does not prepare the rest of the file");
        require(!viewport.highlightingComplete(),"remaining rows are scheduled progressively");
        auto until=[&](const std::function<bool()>& predicate) {
            QElapsedTimer timer; timer.start();
            while(!predicate() && timer.elapsed()<15000) { QCoreApplication::processEvents(); QThread::msleep(1); }
            require(predicate(),"progressive highlighting completes before timeout");
        };
        // A newly visible distant range must jump ahead of the background scan.
        viewport.setVisibleRange(7000,7010);
        until([&]{return !row(7000).isEmpty();});
        require(hasKind(row(7000),SyntaxHighlighter::Kind::String),"scrolling into a multiline raw string preserves its outside context");
        require(row(9000).isEmpty(),"scroll reprioritization precedes unrelated background rows");
        // Edit while background batches remain queued. Their results must not
        // leave holes or overwrite the new line positions.
        document.insert(0,"// inserted\n");
        viewport.applyEdit(0,0,0,{},"// inserted\n",document);
        viewport.setVisibleRange(9001,9011);
        until([&]{return viewport.highlightingComplete();});
        require(hasKind(row(0),SyntaxHighlighter::Kind::Comment),"background completion includes edits outside the viewport");
        require(hasKind(row(3001),SyntaxHighlighter::Kind::Comment),"cached multiline comments shift with inserted rows");
        require(hasKind(row(7001),SyntaxHighlighter::Kind::String),"cached raw strings shift with inserted rows");
        require(hasKind(row(9001),SyntaxHighlighter::Kind::BuiltinType),"new viewport is highlighted after editing");
        for(int line=0;line<10'001;++line)
            require(!row(line).isEmpty(),"discarded batches cannot leave missing highlighted rows");

        // Compare every row against a fresh complete query, including exact
        // UTF-16 spans and captures, to detect partial-range boundary errors.
        TreeSitterCpp complete("cpp"); complete.setLimits(comparisonLimits);
        const auto expected=complete.update(document.text(),{},true);
        for(int line=0;line<expected.size();++line) {
            const auto actual=row(line);
            require(actual.size()==expected[line].size(),"progressive results match a complete query");
            for(int token=0;token<actual.size();++token)
                require(actual[token].start==expected[line][token].start && actual[token].length==expected[line][token].length
                    && actual[token].capture==expected[line][token].capture,"progressive captures and positions match a complete query");
        }
    }

    {
        std::string large;
        for(int row=0;row<100'000;++row) large+="int value = 42;\n";
        TreeSitterCpp parser("cpp"); int polls=0;
        QElapsedTimer clock; clock.start();
        const auto stopped=parser.updateRange(large,{},true,0,20,[&]{return ++polls>=3;});
        require(stopped.isEmpty() && parser.wasCancelled() && parser.parseWasCancelled(),"in-flight parsing can be interrupted between time slices");
        const auto cancelledMs=clock.elapsed();
        const auto recovered=parser.updateRange(large,{},false,0,20);
        require(recovered.size()==21 && !parser.wasCancelled(),"a cancelled initial parse can restart cleanly");
        large.replace(0,3,"long");
        const auto aborted=parser.updateRange(large,{{0,0,0,"int","long"}},false,0,20,[]{return true;});
        require(aborted.isEmpty() && parser.parseWasCancelled(),"an incremental parse observes cancellation");
        const auto second=large.find('\n')+1; large.replace(second,3,"char");
        const auto resumed=parser.updateRange(large,{{second,1,0,"int","char"}},false,0,20);
        require(!parser.lastParseWasFull() && resumed.size()==21,"the next revision reuses an interrupted incremental tree");
        TreeSitterCpp reference("cpp"); const auto expected=reference.updateRange(large,{},true,0,20);
        auto same=[](const auto& a,const auto& b) {
            if(a.size()!=b.size()) return false;
            for(int row=0;row<a.size();++row) {
                if(a[row].size()!=b[row].size())return false;
                for(int i=0;i<a[row].size();++i)
                    if(a[row][i].start!=b[row][i].start || a[row][i].length!=b[row][i].length || a[row][i].capture!=b[row][i].capture)return false;
            }
            return true;
        };
        require(same(resumed,expected),"resuming after cancelled edits matches a fresh parse");
        int matches=0;
        const auto query=parser.highlightRange(200,455,[&]{return ++matches>12;});
        require(query.isEmpty() && parser.wasCancelled() && !parser.parseWasCancelled(),"query cancellation stops old viewport preparation");
        require(same(parser.highlightRange(200,455),reference.highlightRange(200,455)),"a cancelled query leaves no partial valid rows");
        std::cout<<"Cancellation check: stopped initial parse in "<<cancelledMs<<" ms\n";

        PieceTreeBuffer document(large); SyntaxHighlighter asynchronous;
        asynchronous.setLanguageForPath("cancel.cpp"); int cancellations=0;
        QObject::connect(&asynchronous,&SyntaxHighlighter::highlightingCancelled,[&]{++cancellations;});
        asynchronous.setDocumentText(document);
        int edited=0;
        QTimer::singleShot(95,&asynchronous,[&] {
            document.insert(0,"/*\n"); asynchronous.applyEdit(0,0,0,{},"/*\n",document);
            asynchronous.setVisibleRange(90'001,90'020); ++edited;
        });
        QTimer::singleShot(110,&asynchronous,[&] {
            const auto offset=document.size(),row=document.lineIndexAt(offset),column=offset-document.lineStart(row);
            document.insert(offset,"*/\n"); asynchronous.applyEdit(offset,row,column,{},"*/\n",document); ++edited;
        });
        QTimer::singleShot(125,&asynchronous,[&] {
            document.insert(0,"// newest\n"); asynchronous.applyEdit(0,0,0,{},"// newest\n",document);
            asynchronous.setVisibleRange(90'002,90'020); ++edited;
        });
        QElapsedTimer timeout; timeout.start();
        while(timeout.elapsed()<15000) {
            QCoreApplication::processEvents(); QThread::msleep(1);
            if(edited==3 && hasKind(asynchronous.highlightLine(90'002,0,{},[](std::size_t){return QString{};}),SyntaxHighlighter::Kind::Comment)) break;
        }
        require(edited==3 && cancellations>0,"editing actually cancels the previous asynchronous job");
        require(hasKind(asynchronous.highlightLine(90'002,0,{},[](std::size_t){return QString{};}),SyntaxHighlighter::Kind::Comment),
            "the newest viewport reflects all edits in a rapid burst after cancellation");
        asynchronous.setLanguageForPath("cancel.txt");
        QElapsedTimer settle; settle.start();
        while(settle.elapsed()<30) { QCoreApplication::processEvents(); QThread::msleep(1); }
        require(asynchronous.highlightLine(90'001,0,{},[](std::size_t){return QString{};}).isEmpty(),"late cancelled jobs cannot publish after a language change");
    }

    {
        HighlightLimits limits; limits.cacheRows=16; limits.cacheBytes=16*1024;
        HighlightCache<SyntaxHighlighter::Span> cache; cache.setLimits(limits);
        QVector<SyntaxHighlighter::Span> spans{{0,3,SyntaxHighlighter::Kind::Keyword,"keyword"}};
        for(int row=0;row<32;++row) cache.put(row,spans,0,2);
        require(cache.size()<=16 && cache.bytes()<=limits.cacheBytes,"sparse LRU obeys both cache budgets");
        require(cache.contains(0) && cache.contains(1) && cache.contains(2) && !cache.contains(3),"LRU protects the viewport before evicting older rows");
        require(cache.get(20)!=nullptr,"recent history is cached"); cache.put(32,spans,0,2);
        require(cache.contains(20) && !cache.contains(19),"cache reads renew recency");
        cache.splice(20,1,3);
        require(!cache.contains(20) && cache.contains(23),"sparse caches shift correctly after newline edits");
        cache.invalidate(0,2); require(!cache.contains(0),"sparse invalidation removes cached ranges");
        QVector<SyntaxHighlighter::Span> dense(10000,spans[0]);
        require(!cache.put(100,dense,100,100),"a single oversized cache row rejects its spans");
        const auto* fallback=cache.get(100);
        require(fallback && fallback->isEmpty() && fallback->capacity()==0,"an oversized cache row releases storage and retains a plain-text fallback");
        require(cache.bytes()<=limits.cacheBytes,"oversized rows cannot exceed the byte budget");
    }
    {
        TreeSitterCpp parser("cpp"); HighlightLimits limits; limits.parseTimeMs=1;
        parser.setLimits(limits);
        const auto timedOut=parser.updateRange("int value = 42;\n",{},true,0,1,[]{QThread::msleep(2);return false;});
        require(timedOut.isEmpty() && parser.limitReached()==HighlightLimit::ParseTime && parser.cachedBytes()==0,
            "parse deadlines release the syntax tree/cache and report the limit");
        limits.parseTimeMs=5000; parser.setLimits(limits);
        require(!parser.updateRange("int value = 42;\n",{},false,0,1).isEmpty(),"a parser recovers when budgets permit");
        parser.invalidateRange(0,1); limits.queryTimeMs=1; parser.setLimits(limits);
        const auto query=parser.highlightRange(0,1,[]{QThread::msleep(2);return false;});
        require(query.size()==2 && query[0].isEmpty() && parser.limitReached()==HighlightLimit::QueryTime,
            "query deadlines return cached plain rows rather than rescheduling forever");
        parser.invalidateRange(0,1); limits.queryTimeMs=100; limits.maxSpansPerLine=2; parser.setLimits(limits);
        const auto dense=parser.highlightRange(0,1);
        require(dense[0].isEmpty() && parser.limitReached()==HighlightLimit::SpanMemory,"dense rows obey token limits");
        require(parser.cachedBytes()<=limits.cacheBytes,"worker cache stays within its byte budget after fallback");
        limits.maxSpansPerLine=4096; limits.maxLineBytes=128; parser.setLimits(limits);
        const auto longLine=parser.updateRange("const char* text = \""+std::string(1000,'x')+"\";\n",{},true,0,1);
        require(longLine[0].isEmpty() && parser.limitReached()==HighlightLimit::SpanMemory,"very long rows avoid expensive UTF-16 highlighting allocations");
    }
    {
        std::string large;
        for(int row=0;row<10'000;++row) {
            if(row==3000) large+="/* starts outside cached viewport\n";
            else if(row>3000 && row<5000) large+="inside comment\n";
            else if(row==5000) large+="*/\n";
            else large+="int value = 42;\n";
        }
        PieceTreeBuffer document(large); SyntaxHighlighter bounded;
        HighlightLimits limits; limits.cacheRows=128; limits.cacheBytes=256*1024;
        bounded.setLimits(limits); bounded.setLanguageForPath("bounded.cpp"); bounded.setDocumentText(document);
        auto until=[&](const std::function<bool()>& predicate) {
            QElapsedTimer timer; timer.start();
            while(!predicate() && timer.elapsed()<10000) { QCoreApplication::processEvents(); QThread::msleep(1); }
            require(predicate(),"bounded highlighting completes without a retry loop");
        };
        const auto row=[&](int index) { return bounded.highlightLine(index,0,{},[](std::size_t){return QString{};}); };
        for(int line:{100,2000,4000,6000,8000,100,4000,100}) {
            bounded.setVisibleRange(line,line+8);
            until([&]{return bounded.highlightingComplete();});
            require(hasKind(row(line),line==4000 ? SyntaxHighlighter::Kind::Comment : SyntaxHighlighter::Kind::BuiltinType),"evicted rows rehydrate with correct offscreen syntax context");
            require(bounded.cachedLineCount()<=limits.cacheRows && bounded.workerCachedLineCount()<=limits.cacheRows,
                "UI and worker cache rows remain bounded across the file");
            require(bounded.cachedBytes()<=limits.cacheBytes && bounded.workerCachedBytes()<=limits.cacheBytes,
                "UI and worker charged memory remain bounded across the file");
        }
        require(row(2000).isEmpty(),"distant old rows are evicted rather than accumulated");
        document.insert(0,"// inserted\n"); bounded.applyEdit(0,0,0,{},"// inserted\n",document);
        bounded.setVisibleRange(101,110); until([&]{return bounded.highlightingComplete();});
        require(hasKind(row(101),SyntaxHighlighter::Kind::BuiltinType),"sparse row indexes survive edits before the cached area");

        // Deliberately tiny native parse budget on a sizeable input.
        HighlightLimits tiny=limits; tiny.parseTimeMs=1; bounded.setLimits(tiny);
        until([&]{return bounded.highlightingComplete();});
        require(bounded.resourceLimit()==HighlightLimit::ParseTime && bounded.cachedLineCount()==0,"parse fallback stops background retries for the revision");
        bounded.setVisibleRange(9000,9008);
        require(bounded.highlightingComplete(),"scrolling cannot restart a parse that exhausted its revision budget");
        bounded.setLimits(limits); bounded.setVisibleRange(9000,9008);
        until([&]{return bounded.highlightingComplete();});
        require(hasKind(row(9000),SyntaxHighlighter::Kind::BuiltinType),"raising the budget restores highlighting");
    }

    {
        std::string initial; for(int i=0;i<100;++i) initial+="int value = 42;\n";
        PieceTreeBuffer document(initial); SyntaxHighlighter queued;
        HighlightLimits limits; limits.cacheRows=32; limits.cacheBytes=64*1024; limits.queryBytes=64*1024;
        queued.setLimits(limits); queued.setLanguageForPath("queue.cpp"); queued.setVisibleRange(0,2); queued.setDocumentText(document);
        auto drain=[&] {
            QElapsedTimer timer; timer.start();
            while(!queued.highlightingComplete() && timer.elapsed()<10000) { QCoreApplication::processEvents(); QThread::msleep(1); }
            require(queued.highlightingComplete(),"bounded edit queue completes");
        };
        drain();
        const auto paste="//"+std::string(70*1024,'x')+"\n";
        document.insert(0,paste); queued.applyEdit(0,0,0,{},paste,document);
        document.insert(0,"// latest\n"); queued.applyEdit(0,0,0,{},"// latest\n",document);
        drain();
        for(int row:{0,1}) require(hasKind(queued.highlightLine(row,0,{},[](std::size_t){return QString{};}),SyntaxHighlighter::Kind::Comment),
            "overflowing the delta queue rebases on the latest text without losing edits");
        require(queued.cachedBytes()<=limits.cacheBytes && queued.workerCachedBytes()<=limits.cacheBytes,"rebasing preserves cache memory budgets");
    }

    {
        std::string large="/*\n"; const std::string padding="padding for a large multiline comment\n";
        while(large.size()<9u*1024u*1024u) large+=padding;
        large+="*/\nint value = 42;\n";
        PieceTreeBuffer document(large); SyntaxHighlighter overEight;
        overEight.setLanguageForPath("over-eight.cpp"); overEight.setVisibleRange(0,12);
        QElapsedTimer largeClock; largeClock.start(); overEight.setDocumentText(document);
        waitForHighlight(overEight);
        const auto first=overEight.highlightLine(1,0,{},[](std::size_t){return QString{};});
        require(overEight.enabled() && hasKind(first,SyntaxHighlighter::Kind::Comment),"files above eight MiB remain eligible and receive viewport syntax highlighting");
        std::cout<<"Large-file viewport: "<<document.size()<<" bytes, first batch "<<largeClock.elapsed()<<" ms\n";
        require(overEight.cachedLineCount()<=4096 && overEight.cachedBytes()<=8u*1024u*1024u,"large-file highlighting retains bounded caches");
        HighlightLimits limited; limited.sourceWorkingBytes=4096; overEight.setLimits(limited);
        QElapsedTimer settling; settling.start();
        while(!overEight.highlightingComplete() && settling.elapsed()<5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
        require(overEight.resourceLimit()==HighlightLimit::SourceMemory && overEight.highlightingComplete(),"working-copy memory budget pauses before allocating large snapshots");
    }

    {
        PieceTreeBuffer document("int value = 42;\n"); SyntaxHighlighter stable;
        stable.setLanguageForPath("stable.cpp"); stable.setDocumentText(document); waitForHighlight(stable);
        const auto row=[&]{return stable.highlightLine(0,0,{},[](std::size_t){return QString{};});};
        document.insert(6,"x"); stable.applyEdit(6,0,6,{},"x",document);
        require(hasKind(row(),SyntaxHighlighter::Kind::BuiltinType) && hasKind(row(),SyntaxHighlighter::Kind::Number),"typing preserves existing same-row styles before debounce");
        document.erase(6,1); stable.applyEdit(6,0,6,"x",{},document);
        require(hasKind(row(),SyntaxHighlighter::Kind::BuiltinType) && hasKind(row(),SyntaxHighlighter::Kind::Number),"deleting preserves existing same-row styles before debounce");
        waitForHighlight(stable);
        // Correctness after projection still comes from the actual parser.
        document.insert(0,"//"); stable.applyEdit(0,0,0,{},"//",document); waitForHighlight(stable);
        require(hasKind(row(),SyntaxHighlighter::Kind::Comment) && !hasKind(row(),SyntaxHighlighter::Kind::BuiltinType),"fresh parsing replaces provisional styles when syntax changes");
    }

    if (qEnvironmentVariableIsSet("TURTLE_CODE_SYNTAX_BENCH")) {
        std::string large;
        large.reserve(2'000'000);
        for (int i = 0; i < 100'000; ++i) large += "int value = 42;\n";
        PieceTreeBuffer largeBuffer(large);
        SyntaxHighlighter benchmark;
        benchmark.setLanguageForPath(QStringLiteral("large.cpp"));
        QElapsedTimer clock;
        clock.start();
        benchmark.setVisibleRange(50'000,50'030);
        benchmark.setDocumentText(largeBuffer);
        waitForHighlight(benchmark);
        const auto initialMs = clock.elapsed();
        const auto offset = largeBuffer.lineStart(50'000);
        largeBuffer.insert(offset, "// edit\n");
        clock.restart();
        benchmark.applyEdit(offset, 50'000, 0, {}, "// edit\n", largeBuffer);
        const auto schedulingMs = clock.elapsed();
        waitForHighlight(benchmark);
        std::cout << "Syntax benchmark: " << largeBuffer.size() << " bytes, initial "
                  << initialMs << " ms to visible batch, edit scheduling " << schedulingMs
                  << " ms, updated " << clock.elapsed() << " ms; cached " << benchmark.cachedLineCount()
                  << " rows, " << benchmark.cachedBytes() << " UI bytes, " << benchmark.workerCachedBytes() << " worker bytes\n";
    }

    std::cout << "All Tree-sitter syntax tests passed\n";
}
