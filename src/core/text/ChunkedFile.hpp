#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

// Immutable, unlinked backing file. Cached pages are pinned by readers and
// shared by cheap document snapshots, independently of the original pathname.
class ChunkedFile {
    int fd_{-1};
    std::size_t written_{0};
    struct Page { std::shared_ptr<const std::string> data; std::uint64_t used; };
    mutable std::mutex mutex_;
    mutable std::map<std::size_t,Page> pages_;
    mutable std::uint64_t clock_{0};
public:
    static constexpr std::size_t pageBytes=64u*1024u, maxPages=32;
    ChunkedFile() {
        const char* configured=std::getenv("TMPDIR");
        std::string name=std::string(configured && *configured?configured:"/tmp")+"/turtle-code-text-XXXXXX";
        fd_=::mkstemp(name.data());
        if(fd_<0) throw std::runtime_error(std::strerror(errno));
        ::unlink(name.c_str()); ::fcntl(fd_,F_SETFD,FD_CLOEXEC);
    }
    ~ChunkedFile() { if(fd_>=0) ::close(fd_); }
    ChunkedFile(const ChunkedFile&)=delete;
    std::size_t append(std::string_view bytes) {
        std::lock_guard lock(mutex_); const auto start=written_;
        // The last cached page can grow; snapshots only refer to its old prefix.
        pages_.erase(start/pageBytes);
        while(!bytes.empty()) {
            const auto count=::write(fd_,bytes.data(),bytes.size());
            if(count<0 && errno==EINTR) continue;
            if(count<=0) throw std::runtime_error(std::strerror(errno));
            bytes.remove_prefix(static_cast<std::size_t>(count)); written_+=count;
        }
        return start;
    }
    std::size_t size() const { std::lock_guard lock(mutex_);return written_; }
    std::string read(std::size_t offset,std::size_t length) const {
        std::string result;result.reserve(length);
        while(length) {
            auto data=page(offset/pageBytes);const auto begin=offset%pageBytes;
            if(begin>=data->size())throw std::runtime_error("Incomplete temporary text storage");
            const auto count=std::min(length,data->size()-begin);result.append(data->data()+begin,count);offset+=count;length-=count;
        }
        return result;
    }
    std::shared_ptr<const std::string> page(std::size_t index) const {
        std::lock_guard lock(mutex_);
        if(auto it=pages_.find(index);it!=pages_.end()) { it->second.used=++clock_; return it->second.data; }
        auto data=std::make_shared<std::string>(pageBytes,'\0'); std::size_t count=0;
        while(count<pageBytes) {
            auto got=::pread(fd_,data->data()+count,pageBytes-count,static_cast<off_t>(index*pageBytes+count));
            if(got<0 && errno==EINTR) continue;
            if(got<0) throw std::runtime_error(std::strerror(errno));
            if(!got) break;
            count+=got;
        }
        data->resize(count);
        if(pages_.size()>=maxPages) {
            auto victim=std::min_element(pages_.begin(),pages_.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
            pages_.erase(victim);
        }
        pages_.emplace(index,Page{data,++clock_}); return data;
    }
    void dropCache() const {std::lock_guard lock(mutex_);pages_.clear();}
    std::size_t cachedBytes() const { std::lock_guard lock(mutex_); std::size_t sum=0;for(const auto& [key,page]:pages_)sum+=page.data->capacity();return sum; }
};
