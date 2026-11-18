#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <optional>
#include <memory>

// Syntax and validation are independent of widgets, documents and command handlers.
// Parse one command at a time: a preceding command can change '.' and '$'.
namespace VimEx {
enum class Arguments { None, File, RegisterCount, Register, Raw, Substitute, Global, Address };
enum class DefaultRange { Current, Whole };
struct Definition {
    QString name;
    int minimum{1};
    bool range{false}, bang{false}, zero{false};
    Arguments arguments{Arguments::None};
    DefaultRange defaultRange{DefaultRange::Current};
    bool bars{true}, comments{true};
};
struct Context {
    qint64 current{1}, last{1};
    std::function<std::optional<qint64>(QChar)> mark;
    // The search engine supplies this in the search/substitute stage.
    std::function<std::optional<qint64>(const QString&, bool, qint64)> search;
};
struct Invocation {
    Definition definition;
    qint64 first{1}, last{1};
    bool explicitRange{false}, bang{false};
    QString arguments;
    QChar registerName;
    int count{0};
    qint64 destination{};
    bool silent{false},ignoreErrors{false},keepJumps{false},keepPatterns{false};
    bool vertical{false};
};
struct Parsed {
    Invocation command;
    QString rest, error;
    bool empty{false};
};
class Registry {
public:
    Registry();
    void add(Definition definition);
    const Definition* find(const QString& abbreviation) const;
    Parsed parse(const QString& line, const Context& context) const;
    QStringList complete(const QString& prefix) const;
private:
    QVector<Definition> definitions_;
};
QString fileArgument(const QString& argument, const QString& currentFile, QString* error,const QString& alternateFile={});
struct Global {QString pattern,commands,error;};
Global parseGlobal(const QString& arguments);
// Pending :global/:normal line marks. Lazy row shifts avoid revisiting every
// selected line after each insertion/deletion. Copy-on-write snapshots share unchanged subtrees with workers.
class LineMarks {
public:
    LineMarks();~LineMarks();LineMarks(const LineMarks&);LineMarks& operator=(const LineMarks&);
    void add(qint64 row);
    std::optional<qint64> takeFirst();
    bool empty() const;
    void edit(qint64 row,qint64 column,qint64 removed,qint64 inserted);
    void move(qint64 first,qint64 last,qint64 destination);
private:
    struct Node;std::shared_ptr<Node> root_;unsigned random_{0x12345678};
    static void shift(std::shared_ptr<Node>&,qint64);
    static void push(Node&);
    static std::pair<std::shared_ptr<Node>,std::shared_ptr<Node>> split(std::shared_ptr<Node>,qint64);
    static std::shared_ptr<Node> merge(std::shared_ptr<Node>,std::shared_ptr<Node>);
};
}
