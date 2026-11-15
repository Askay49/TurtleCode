#pragma once
#include "VimMotions.hpp"
#include "VimSearch.hpp"
#include "core/text/EditHistory.hpp"

// Widget-independent Normal edits. Text and edit records stay in shared disk
// payloads; applying a plan uses the same bounded batches as substitutions.
namespace VimNormal {
using Check=std::function<void()>;
struct Options {
    int shiftWidth{4},tabWidth{4},textWidth{79};bool tabs{false},joinSpaces{false};
    QString numberFormats{"bin,hex"};
};
struct Plan {std::shared_ptr<VimSearch::Plan> edits{std::make_shared<VimSearch::Plan>()};std::size_t cursor{};};
struct Join {
    struct Part {std::size_t begin,text,end,destination;};
    std::shared_ptr<ChunkedFile> text{std::make_shared<ChunkedFile>()};std::size_t cursor{},begin{},end{};std::vector<Part> parts;
    std::size_t project(std::size_t position) const;
};
Join join(const PieceTreeBuffer&,std::size_t first,std::size_t last,bool raw,bool spaces,const Check& = {});
Plan changeCase(const PieceTreeBuffer&,const std::vector<VimMotions::Range>&,VimController::Command,std::size_t cursor,const Check& = {});
Plan number(const PieceTreeBuffer&,const std::vector<VimMotions::Range>&,int count,bool decrement,bool progressive,bool visual,const Options&,const Check& = {});
Plan indent(const PieceTreeBuffer&,std::size_t first,std::size_t last,const Options&,const Check& = {});
Plan format(const PieceTreeBuffer&,std::size_t first,std::size_t last,const Options&,std::string_view eol,const Check& = {});
std::size_t project(const Plan&,std::size_t position);
HistoryResult apply(const Plan&,PieceTreeBuffer&,EditHistory&,std::size_t original,const Check& = {});
}
