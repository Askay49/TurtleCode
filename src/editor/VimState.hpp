#pragma once
#include "core/text/ChunkedFile.hpp"
#include <QHash>
#include <QPointer>
#include <QPoint>
#include <QVector>
#include <QMap>
#include <QObject>
#include <QString>
#include "VimSearch.hpp"
#include <memory>
#include <functional>

namespace VimState {
struct Input {
    enum Kind { Key, Text, Edit, Put } kind{Key};
    int key{},modifiers{};QString text;
    int back{},forward{};
    std::shared_ptr<ChunkedFile> payload;
    std::size_t offset{},length{};
};
struct Register {
    enum Kind { Character, Line, Block } kind{Character};
    std::shared_ptr<ChunkedFile> text;
    int width{};
    int column{};
    QVector<Input> macro;
    bool empty() const {return !text || !text->size();}
};
struct Location {
    QPointer<QObject> owner;QString path;std::size_t offset{};QPoint position{1,1};
    QMap<quint64,std::size_t> lostPositions;
};
using Anchor=std::shared_ptr<Location>;
struct Jumps {QVector<Anchor> entries;int index{};Anchor previous;};
struct Change {QVector<Input> inputs;};
class Session final : public QObject {
public:
    static Session& instance();
    Register read(QChar name) const;
    void write(QChar selected,const Register&,bool yank,bool smallDelete,bool forceNumbered=false,const Register* appended=nullptr);
    void set(QChar name,const Register&);
    void clear();
    QChar recording,lastMacro;
    QPointer<QObject> recordingOwner;
    QVector<Input> recordingInputs;
    std::size_t recordingBytes{};
    Change lastChange;
    QStringList commandHistory;
    QString lastCommand;
    QString previousExCommand;
    QChar lastExRegister;
    QString searchPattern,searchOffset,lastSubPattern,lastReplacement,lastSubFlags;
    bool searchForward{true},highlightSearch{true};
    bool incrementalSearch{true},wrapSearch{true},globalSubstitute{false};
    QStringList searchHistory;
    VimSearch::Options searchOptions;
    QString numberFormats{"bin,hex"};int textWidth{};bool joinSpaces{false};
    Anchor anchor(QObject* owner,const QString& path,std::size_t offset,QPoint position);
    QHash<QChar,Anchor>& marks(const QString& key);
    QHash<QChar,Anchor> globalMarks;
    Jumps& jumps(QObject* window);
    QVector<Anchor> anchors(QObject* owner);
    void attach(QObject* owner,const QString& path);
    void rename(QObject* owner,const QString& oldKey,const QString& newKey,const QString& path);
    static std::size_t project(std::size_t position,std::size_t at,std::size_t removed,std::size_t inserted);
private:
    QHash<QChar,Register> registers_;
    QHash<QString,QHash<QChar,Anchor>> marks_;
    QVector<QString> markFiles_;
    QHash<QObject*,Jumps> jumps_;
    QVector<std::weak_ptr<Location>> anchors_;
};
Register text(const QString&,Register::Kind kind=Register::Character,int width=0);
QVector<Input> keys(const QString&);
Register append(const Register&,const Register&,const std::function<void()>& checkpoint={});
std::shared_ptr<ChunkedFile> convertNewlines(const std::shared_ptr<ChunkedFile>&,std::string_view eol,const std::function<void()>& checkpoint={});
}
