#pragma once

#include <QString>
#include "HighlightBudget.hpp"
#include <QVector>
#include <cstddef>
#include <memory>
#include <functional>
#include <string>
#include <vector>

class TreeSitterCpp final {
public:
    using CancelCheck = std::function<bool()>;
    enum class Kind { Keyword, Type, Function, Variable, String, Comment, Number, Preprocessor, Constant, Property, BuiltinType, Boolean, Operator, Punctuation, Parameter };
    struct Span { int start{}; int length{}; Kind kind{Kind::Keyword}; int priority{0}; QString capture; };
    struct Edit {
        std::size_t offset{};
        std::size_t startRow{};
        std::size_t startColumn{}; // UTF-8 byte column
        std::string removed;
        std::string inserted;
    };

    explicit TreeSitterCpp(QString languageId = QStringLiteral("cpp"));
    ~TreeSitterCpp();
    TreeSitterCpp(const TreeSitterCpp&) = delete;
    TreeSitterCpp& operator=(const TreeSitterCpp&) = delete;

    [[nodiscard]] bool available() const noexcept;
    void reset() noexcept;
    void reload();

    // Called serially by the syntax worker. Keeps the parser/tree alive between
    // calls and applies all edits since the previous parsed snapshot.
    [[nodiscard]] QVector<QVector<Span>> update(
        const std::string& source, const std::vector<Edit>& edits, bool forceFullParse);

    // Parse the complete syntax tree for context, but query only these rows.
    [[nodiscard]] QVector<QVector<Span>> updateRange(
        const std::string& source, const std::vector<Edit>& edits, bool forceFullParse,
        std::size_t firstLine, std::size_t lastLine,
        const CancelCheck& cancelParse = {}, const CancelCheck& cancelQuery = {});
    [[nodiscard]] QVector<QVector<Span>> highlightRange(std::size_t firstLine, std::size_t lastLine, const CancelCheck& cancel = {});
    [[nodiscard]] std::size_t lineCount() const noexcept;
    [[nodiscard]] int nextPendingLine() const noexcept;
    [[nodiscard]] bool wasCancelled() const noexcept;
    [[nodiscard]] bool parseWasCancelled() const noexcept;
    [[nodiscard]] bool lastParseWasFull() const noexcept;
    void setLimits(HighlightLimits limits);
    void setWorkingRange(std::size_t firstLine,std::size_t lastLine,std::size_t pinFirst,std::size_t pinLast);
    [[nodiscard]] std::size_t cachedLineCount() const noexcept;
    [[nodiscard]] std::size_t cachedBytes() const noexcept;
    [[nodiscard]] HighlightLimit limitReached() const noexcept;
    void invalidateRange(std::size_t firstLine, std::size_t lastLine);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
