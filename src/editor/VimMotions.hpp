#pragma once
#include "VimController.hpp"
#include "core/text/PieceTreeBuffer.hpp"
#include <functional>
#include <optional>

namespace VimMotions {
struct Range {
    std::size_t target{},begin{},end{};
    bool valid{false},linewise{false},object{false};
};
std::size_t lineStart(const PieceTreeBuffer&,std::size_t line);
std::size_t lineEnd(const PieceTreeBuffer&,std::size_t line);
std::size_t firstText(const PieceTreeBuffer&,std::size_t line);
std::size_t lastLine(const PieceTreeBuffer&,bool finalNewline);
Range resolveSelection(const PieceTreeBuffer&,std::size_t cursor,const VimController::Action&,
              std::optional<std::pair<std::size_t,std::size_t>> selection,
              const std::function<void()>& checkpoint={},bool finalNewline=false);
Range resolve(const PieceTreeBuffer&,std::size_t cursor,const VimController::Action&,
              const std::function<void()>& checkpoint={},bool finalNewline=false);
}
