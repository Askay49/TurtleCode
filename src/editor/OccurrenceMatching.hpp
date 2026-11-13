#pragma once
#include <QString>

namespace OccurrenceMatching {
// Both UTF-16 halves of a supplementary letter belong to the identifier.
inline bool isWord(const QString& text, qsizetype index) {
    if (index < 0 || index >= text.size()) return false;
    char32_t code = text[index].unicode();
    if (text[index].isLowSurrogate() && index > 0 && text[index-1].isHighSurrogate())
        code = QChar::surrogateToUcs4(text[index-1],text[index]);
    else if (text[index].isHighSurrogate() && index+1 < text.size() && text[index+1].isLowSurrogate())
        code = QChar::surrogateToUcs4(text[index],text[index+1]);
    const auto category = QChar::category(code);
    return QChar::isLetterOrNumber(code) || code == U'_' ||
        category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining ||
        category == QChar::Mark_Enclosing;
}
inline bool wholeWord(const QString& text, qsizetype begin, qsizetype end) {
    if (begin < 0 || end > text.size() || begin >= end || isWord(text,begin-1) || isWord(text,end)) return false;
    for (auto i=begin;i<end;++i) if (!isWord(text,i)) return false;
    return true;
}
}
