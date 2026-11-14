#pragma once
#include <QString>
#include <QStringList>
#include <QList>
#include <memory>
#include <iterator>
struct ProjectSearchMatch {
    QString path, relativePath;
    int line{}, column{}, length{};
    QString preview;
    qint64 byteOffset{}, byteLength{};
    QString matchedText;
    QStringList captures;
};
// Temporary disk store. Copies are immutable size snapshots of an append-only store.
// Payload and row offsets stay on disk, independent of the number of matches.
class SearchMatches {
public:
    SearchMatches() = default;
    qsizetype size() const { return size_; }
    bool isEmpty() const { return size_==0; }
    ProjectSearchMatch operator[](qsizetype index) const;
    ProjectSearchMatch back() const { return (*this)[size_-1]; }
    void push_back(const ProjectSearchMatch& value);
    void pop_back() { truncate(size_-1); }
    void truncate(qsizetype count);
    void sealFile(qsizetype first);
    bool containsPath(const QString& path) const;
    qsizetype runLength(qsizetype first) const;
    class Cursor {
    public:
        bool next(qsizetype& index, ProjectSearchMatch& value);
    private:
        friend class SearchMatches;
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };
    Cursor ordered() const;
    struct Iterator {
        using iterator_category=std::input_iterator_tag;
        using value_type=ProjectSearchMatch;
        using difference_type=qsizetype;
        using pointer=void;
        using reference=ProjectSearchMatch;
        const SearchMatches* owner{};qsizetype index{};
        ProjectSearchMatch operator*() const {return (*owner)[index];}
        Iterator& operator++(){++index;return *this;}
        Iterator operator++(int){auto result=*this;++*this;return result;}
        bool operator==(const Iterator& other) const {return owner==other.owner && index==other.index;}
        bool operator!=(const Iterator& other) const {return !(*this==other);}
    };
    Iterator begin() const{return {this,0};} Iterator end() const{return {this,size_};}
    Iterator cbegin() const{return begin();} Iterator cend() const{return end();}
private:
    struct Storage;
    std::shared_ptr<Storage> storage_;
    qsizetype size_{};
};

class SearchHashes {
public:
    void insert(const QString& path,const QByteArray& hash);
    QByteArray value(const QString& path) const;
    bool contains(const QString& path) const {return !value(path).isEmpty();}
private:
    struct Storage;
    std::shared_ptr<Storage> storage_;
};
