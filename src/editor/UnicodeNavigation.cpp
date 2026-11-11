#include "UnicodeNavigation.hpp"

#include <QTextBoundaryFinder>
#include <QString>

#include <algorithm>

namespace {
QString decode(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::size_t toByteOffset(const QString& text, int utf16Offset) {
    return static_cast<std::size_t>(text.left(std::clamp(utf16Offset, 0, static_cast<int>(text.size()))).toUtf8().size());
}
int toUtf16Offset(std::string_view text, std::size_t byteOffset) {
    byteOffset = std::min(byteOffset, text.size());
    return decode(text.substr(0, byteOffset)).size();
}
bool isWordCharacter(QChar ch) {
    const auto category = ch.category();
    return ch.isLetterOrNumber() || ch == QLatin1Char('_') ||
        category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining ||
        category == QChar::Mark_Enclosing;
}
int selectionClass(QChar ch) {
    if (isWordCharacter(ch)) return 0;
    if (ch.isSpace()) return 1;
    return 2;
}
}

namespace EditorUnicode {
std::size_t previousGrapheme(std::string_view utf8, std::size_t byteOffset) {
    const QString text = decode(utf8);
    const int position = toUtf16Offset(utf8, byteOffset);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.setPosition(position);
    const int boundary = finder.toPreviousBoundary();
    return boundary < 0 ? 0 : toByteOffset(text, boundary);
}

std::size_t nextGrapheme(std::string_view utf8, std::size_t byteOffset) {
    const QString text = decode(utf8);
    const int position = toUtf16Offset(utf8, byteOffset);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.setPosition(position);
    const int boundary = finder.toNextBoundary();
    return boundary < 0 ? utf8.size() : toByteOffset(text, boundary);
}

std::size_t previousWord(std::string_view utf8, std::size_t byteOffset) {
    const QString text = decode(utf8);
    int position = std::clamp(toUtf16Offset(utf8, byteOffset), 0, static_cast<int>(text.size()));
    while (position > 0 && text.at(position - 1).isSpace()) --position;
    while (position > 0 && isWordCharacter(text.at(position - 1))) --position;
    while (position > 0 && !isWordCharacter(text.at(position - 1)) && !text.at(position - 1).isSpace()) --position;
    return toByteOffset(text, position);
}

std::size_t nextWord(std::string_view utf8, std::size_t byteOffset) {
    const QString text = decode(utf8);
    int position = std::clamp(toUtf16Offset(utf8, byteOffset), 0, static_cast<int>(text.size()));
    while (position < text.size() && isWordCharacter(text.at(position))) ++position;
    while (position < text.size() && !isWordCharacter(text.at(position))) ++position;
    return toByteOffset(text, position);
}

std::pair<std::size_t, std::size_t> selectionRangeAt(
    std::string_view utf8, std::size_t byteOffset) {
    const QString text = decode(utf8);
    if (text.isEmpty()) return {0, 0};
    int position = std::clamp(toUtf16Offset(utf8, byteOffset), 0, static_cast<int>(text.size()));
    if (position == text.size() ||
        (position > 0 && text.at(position).isSpace() && !text.at(position - 1).isSpace()))
        --position;
    const int category = selectionClass(text.at(position));
    int begin = position;
    int end = position + 1;
    while (begin > 0 && selectionClass(text.at(begin - 1)) == category) --begin;
    while (end < text.size() && selectionClass(text.at(end)) == category) ++end;
    return {toByteOffset(text, begin), toByteOffset(text, end)};
}
}
