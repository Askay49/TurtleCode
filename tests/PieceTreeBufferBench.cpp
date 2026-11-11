#include "core/text/PieceTreeBuffer.hpp"

#include <chrono>
#include <iostream>
#include <string>

int main() {
    constexpr std::size_t mebibyte = 1024u * 1024u;
    std::string source;
    source.reserve(8 * mebibyte);
    for (int i = 0; i < 100000; ++i)
        source += "int calculate_value(int input) { return input * 2; } // benchmark line\n";

    const auto loadStart = std::chrono::steady_clock::now();
    PieceTreeBuffer buffer(source);
    const auto loadEnd = std::chrono::steady_clock::now();
    const auto editStart = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < 10000; ++i) {
        const std::size_t position = (i * 7919u) % (buffer.size() + 1);
        buffer.insert(position, "/*x*/");
        buffer.erase(position, 5);
    }
    const auto editEnd = std::chrono::steady_clock::now();
    const auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::cout << "Bytes: " << buffer.size() << '\n'
              << "Lines: " << buffer.lineCount() << '\n'
              << "Load: " << ms(loadStart, loadEnd) << " ms\n"
              << "20k edits (10k insert + 10k erase): " << ms(editStart, editEnd) << " ms\n";
}
