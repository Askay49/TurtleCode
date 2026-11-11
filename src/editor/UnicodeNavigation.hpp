#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

namespace EditorUnicode {
// Offsets are UTF-8 byte offsets into a single logical line. Returned offsets
// are always UTF-8 code-point boundaries and, for grapheme functions, extended
// grapheme-cluster boundaries as defined by Qt's Unicode boundary tables.
[[nodiscard]] std::size_t previousGrapheme(std::string_view utf8, std::size_t byteOffset);
[[nodiscard]] std::size_t nextGrapheme(std::string_view utf8, std::size_t byteOffset);
[[nodiscard]] std::size_t previousWord(std::string_view utf8, std::size_t byteOffset);
[[nodiscard]] std::size_t nextWord(std::string_view utf8, std::size_t byteOffset);
[[nodiscard]] std::pair<std::size_t, std::size_t> selectionRangeAt(
    std::string_view utf8, std::size_t byteOffset);
}
