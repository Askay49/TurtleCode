#pragma once
#include "core/text/PieceTreeBuffer.hpp"
#include "core/text/ChunkedFile.hpp"
#include <QString>
#include <QVector>
#include <memory>
#include <optional>
#include <array>

namespace VimSearch {
using Checkpoint=std::function<void()>;
struct Options {bool ignoreCase{false},smartCase{false},magic{true};int caseOverride{0};};
struct Span {std::size_t begin{},end{};bool set{false};};
struct Match {std::size_t begin{},end{},rawBegin{},rawEnd{};std::array<Span,10> groups;bool virtualNewline{false};};
class Subject final {
public:
    explicit Subject(const PieceTreeBuffer&,bool finalNewline,const Checkpoint& checkpoint={});
    ~Subject();
    Subject(const Subject&)=delete;
    std::size_t size() const;
    std::size_t begin() const;
    std::string_view slice(std::size_t begin,std::size_t length) const;
    std::size_t next(std::size_t at) const;
    std::size_t previous(std::size_t at) const;
private:
    struct Data;std::unique_ptr<Data> data_;
    friend class Pattern;
};
class Pattern final {
public:
    Pattern(const QString& vimPattern,Options options={});
    ~Pattern();
    Pattern(Pattern&&) noexcept;
    Pattern& operator=(Pattern&&) noexcept;
    Pattern(const Pattern&)=delete;
    QString error() const;
    std::optional<Match> find(const Subject&,std::size_t from,const Checkpoint& checkpoint={}) const;
private:
    struct Data;std::unique_ptr<Data> data_;
};
struct Navigation {std::optional<Match> match;bool wrapped{false};};
Navigation navigate(const Subject&,const Pattern&,std::size_t cursor,bool forward,int count=1,const Checkpoint& checkpoint={},bool endOfMatch=false,int characterOffset=0);
QString literal(const QString& text,bool wholeWord=false);
struct Substitute {
    QString pattern,replacement,flags,error,rest;
    bool repeat{false};int count{0};
};
Substitute parseSubstitute(const QString& arguments);
QString expandReplacement(const QString& replacement,const QString& previous,QString* error);
void replacement(const QString& program,const Subject&,const Match&,std::string_view eol,
                 ChunkedFile& output,const Checkpoint& checkpoint={});
struct Edit {std::uint64_t begin,end,offset,length;};
struct Plan {
    std::shared_ptr<ChunkedFile> records{std::make_shared<ChunkedFile>()},text{std::make_shared<ChunkedFile>()};
    std::size_t count{},lines{};
    Edit at(std::size_t index) const;
};
std::shared_ptr<Plan> plan(const PieceTreeBuffer&,const Subject&,const Pattern&,const QString& replacement,
                           std::size_t first,std::size_t last,bool global,bool countOnly,std::string_view eol,
                           const Checkpoint& checkpoint={},const std::function<bool(std::size_t)>& selected={});
}
