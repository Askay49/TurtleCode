#pragma once
#include "HighlightBudget.hpp"
#include "HighlightEdit.hpp"
#include <QVector>
#include <map>
#include <cstdint>
#include <utility>

// Sparse LRU cache. Account conservatively for containers, their allocation
// capacity and string payloads (including implicitly shared strings per copy).
template<class Span> class HighlightCache {
    struct Entry { QVector<Span> spans; std::size_t bytes; std::uint64_t touched; };
    std::map<std::size_t,Entry> rows_;
    std::map<std::uint64_t,std::size_t> recency_;
    HighlightLimits limits_;
    std::size_t bytes_{0};
    std::uint64_t clock_{0};
    static std::size_t charge(const QVector<Span>& spans) {
        std::size_t bytes=sizeof(Entry)+sizeof(std::size_t)+128+spans.capacity()*sizeof(Span);
        for(const auto& span:spans) bytes+=32+span.capture.capacity()*sizeof(char16_t);
        return bytes;
    }
    void erase(typename std::map<std::size_t,Entry>::iterator it) { bytes_-=it->second.bytes; recency_.erase(it->second.touched); rows_.erase(it); }
public:
    void setLimits(HighlightLimits limits) { limits_=limits.normalized(); trim(1,0); }
    void clear() { rows_.clear(); recency_.clear(); bytes_=0; }
    std::size_t size() const { return rows_.size(); }
    std::size_t bytes() const { return bytes_; }
    bool contains(std::size_t row) const { return rows_.find(row)!=rows_.end(); }
    const QVector<Span>* get(std::size_t row) {
        auto it=rows_.find(row); if(it==rows_.end()) return nullptr;
        recency_.erase(it->second.touched); it->second.touched=++clock_; recency_.emplace(clock_,row); return &it->second.spans;
    }
    bool put(std::size_t row,QVector<Span> spans,std::size_t pinFirst,std::size_t pinLast) {
        bool retained=true;
        if(charge(spans)>limits_.cacheBytes) {
            // Empty initializer-list assignment may retain capacity in Qt.
            // Release the oversized allocation before charging the fallback.
            QVector<Span>{}.swap(spans); retained=false;
        }
        if(auto it=rows_.find(row);it!=rows_.end()) erase(it);
        const auto bytes=charge(spans);
        rows_.emplace(row,Entry{std::move(spans),bytes,++clock_}); recency_.emplace(clock_,row); bytes_+=bytes;
        trim(pinFirst,pinLast);
        return retained;
    }
    void trim(std::size_t pinFirst,std::size_t pinLast) {
        while(rows_.size()>limits_.cacheRows || bytes_>limits_.cacheBytes) {
            auto victim=recency_.begin();
            while(victim!=recency_.end() && victim->second>=pinFirst && victim->second<=pinLast) ++victim;
            // Budgets remain hard even when the entire retained set is pinned.
            if(victim==recency_.end()) victim=recency_.begin();
            erase(rows_.find(victim->second));
        }
    }
    void invalidate(std::size_t first,std::size_t last) {
        auto it=rows_.lower_bound(first);
        while(it!=rows_.end() && it->first<=last) { auto old=it++; erase(old); }
    }
    void projectEdit(const HighlightEdit& edit,std::size_t pinFirst,std::size_t pinLast) {
        std::map<std::size_t,QVector<Span>> projected;
        for(const auto& [row,entry]:rows_) {
            if(entry.spans.isEmpty()) {
                if(row<edit.row) projected[row];
                else if(row>edit.oldEndRow) projected[row-edit.oldEndRow+edit.newEndRow];
            }
            for(const auto& span:entry.spans) edit.project(row,span,[&](std::size_t target,Span copy) {
                projected[target].append(std::move(copy));
            });
        }
        // Keep the existing recency for surviving entries rather than treating
        // offscreen rows as recently read during each keystroke.
        std::map<std::size_t,std::uint64_t> recency;
        for(const auto& [row,entry]:rows_) {
            const auto target=row<edit.row ? row : row>edit.oldEndRow ? row-edit.oldEndRow+edit.newEndRow : edit.row;
            recency[target]=std::max(recency[target],entry.touched);
        }
        clear();
        for(auto& [row,spans]:projected) {
            const auto bytes=charge(spans);
            if(bytes>limits_.cacheBytes) continue;
            // Split boundary rows may share a former stamp, so allocate unique
            // stamps in their previous LRU order below.
            rows_.emplace(row,Entry{std::move(spans),bytes,recency[row]}); bytes_+=bytes;
        }
        std::map<std::pair<std::uint64_t,std::size_t>,std::size_t> ordered;
        for(const auto& [row,entry]:rows_) ordered.emplace(std::make_pair(entry.touched,row),row);
        for(const auto& [key,row]:ordered) { rows_.at(row).touched=++clock_; recency_.emplace(clock_,row); }
        trim(pinFirst,pinLast);
    }
    void splice(std::size_t first,std::size_t removedRows,std::size_t insertedRows) {
        std::map<std::size_t,Entry> shifted;
        for(auto& [row,entry]:rows_) {
            if(row>=first && row<first+removedRows) { bytes_-=entry.bytes; recency_.erase(entry.touched); continue; }
            const auto next=row>=first+removedRows ? row-removedRows+insertedRows : row;
            recency_[entry.touched]=next; shifted.emplace(next,std::move(entry));
        }
        rows_=std::move(shifted);
    }
};
