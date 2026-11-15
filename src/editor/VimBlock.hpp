#pragma once
#include "VimMotions.hpp"
#include <string>

namespace VimBlock {
struct Slice {std::size_t begin{},end{};int column{},gap{};std::string before,after,text;};
Slice slice(const PieceTreeBuffer&,std::size_t row,int left,int right,int tabWidth,const std::function<void()>& checkpoint={},const std::function<void(std::string_view)>& sink={});
int column(const PieceTreeBuffer&,std::size_t row,std::size_t position,int tabWidth);
std::size_t advance(const PieceTreeBuffer&,std::size_t position,int count,const std::function<void()>& checkpoint={});
int width(std::string_view text,int column,int tabWidth);
void forEachCell(const PieceTreeBuffer&,std::size_t row,int tabWidth,
    const std::function<bool(std::size_t,std::size_t,int,int,std::string_view)>& visitor,const std::function<void()>& checkpoint={},int initialColumn=0);
}
