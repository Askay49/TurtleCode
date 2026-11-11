#include "editor/UnicodeNavigation.hpp"

#include <QCoreApplication>
#include <QString>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const std::string combining = "e\xCC\x81x"; // e + COMBINING ACUTE + x
    expect(EditorUnicode::nextGrapheme(combining, 0) == 3,
           "combining mark stays with its base character");
    expect(EditorUnicode::previousGrapheme(combining, 3) == 0,
           "backspace boundary includes combining mark");

    const std::string family = QString::fromUtf8("👨‍👩‍👧‍👦").toUtf8().toStdString();
    expect(EditorUnicode::nextGrapheme(family, 0) == family.size(),
           "family ZWJ emoji is one grapheme");
    expect(EditorUnicode::previousGrapheme(family, family.size()) == 0,
           "backspace removes complete family emoji");

    const std::string flag = QString::fromUtf8("🇺🇳").toUtf8().toStdString();
    expect(EditorUnicode::nextGrapheme(flag, 0) == flag.size(),
           "regional indicator pair is one grapheme");

    const std::string skinTone = QString::fromUtf8("👍🏽x").toUtf8().toStdString();
    expect(EditorUnicode::nextGrapheme(skinTone, 0) == skinTone.size() - 1,
           "emoji modifier stays with emoji base");

    const std::string words = QString::fromUtf8("hello, мир! next").toUtf8().toStdString();
    const auto firstWordEnd = static_cast<std::size_t>(QString::fromUtf8("hello, ").toUtf8().size());
    expect(EditorUnicode::nextWord(words, 0) == firstWordEnd,
           "Ctrl+Right advances to next word");
    const auto endOfSecondWord = static_cast<std::size_t>(QString::fromUtf8("hello, мир").toUtf8().size());
    expect(EditorUnicode::previousWord(words, endOfSecondWord) == firstWordEnd,
           "Ctrl+Left moves to previous word start");

    const std::string identifiers = QString::fromUtf8("alpha_beta мир!").toUtf8().toStdString();
    const auto identifier = EditorUnicode::selectionRangeAt(identifiers, 6);
    expect(identifiers.substr(identifier.first, identifier.second - identifier.first) == "alpha_beta",
           "double-click selects an identifier with underscore");
    const auto cyrillic = EditorUnicode::selectionRangeAt(identifiers, identifiers.find("м"));
    expect(identifiers.substr(cyrillic.first, cyrillic.second - cyrillic.first) == "мир",
           "double-click selects a Unicode word");
    const auto accented = EditorUnicode::selectionRangeAt(combining, 1);
    expect(combining.substr(accented.first, accented.second - accented.first) == "e\xCC\x81x",
           "combining mark remains inside selected word");

    if (failures == 0) std::cout << "All Unicode navigation tests passed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
