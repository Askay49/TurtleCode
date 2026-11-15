#pragma once
#include "VimController.hpp"
#include "core/text/PieceTreeBuffer.hpp"
#include <functional>

namespace VimMotions {
struct Range {
    std::size_t target{},begin{},end{};
    bool valid{false},linewise{false},object{false};
};
std::size_t lineStart(const PieceTreeBuffer&,std::size_t line);
std::size_t lineEnd(const PieceTreeBuffer&,std::size_t line);
std::size_t firstText(const PieceTreeBuffer&,std::size_t line);
std::size_t lastLine(const PieceTreeBuffer&,bool finalNewline);
Range resolve(const PieceTreeBuffer&,std::size_t cursor,const VimController::Action&,
              const std::function<void()>& checkpoint={},bool finalNewline=false);
}
