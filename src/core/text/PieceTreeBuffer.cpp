#include "PieceTreeBuffer.hpp"

#include <algorithm>
#include "ChunkedFile.hpp"
#include <sys/stat.h>
#include <limits>
namespace {
std::size_t utf16Units(std::string_view bytes) {std::size_t result=0;for(unsigned char c:bytes)if((c&0xc0)!=0x80)result+=c>=0xf0?2:1;return result;}
std::size_t codePoints(std::string_view bytes) {return std::count_if(bytes.begin(),bytes.end(),[](unsigned char c){return (c&0xc0)!=0x80;});}
}

PieceTreeBuffer::Node::Node(Source src, std::size_t begin, std::size_t len, std::uint32_t p, std::size_t nl,std::size_t cp,std::size_t utf16)
    : source(src), start(begin), length(len), priority(p),
      subtreeBytes(len), ownNewlines(nl), subtreeNewlines(nl),subtreeDiskBytes(src==Source::AddedDisk?len:0),ownCodePoints(cp==static_cast<std::size_t>(-1)?len:cp),subtreeCodePoints(ownCodePoints),ownUtf16(utf16==static_cast<std::size_t>(-1)?len:utf16),subtreeUtf16(ownUtf16) {}

PieceTreeBuffer::PieceTreeBuffer(PieceTreeBuffer&& other) noexcept { *this=std::move(other); }
PieceTreeBuffer& PieceTreeBuffer::operator=(PieceTreeBuffer&& other) noexcept {
    if(this==&other)return *this;
    // Keep each buffer's mutex stable: syntax/search workers may still be
    // reading the document while a prepared buffer is committed on the UI.
    std::scoped_lock lock(*mutex_,*other.mutex_);
    originalId_=other.originalId_;addId_=other.addId_;diskId_=other.diskId_;
    other.originalId_=nextStorageId_++;other.addId_=nextStorageId_++;other.diskId_=nextStorageId_++;
    backing_=std::move(other.backing_);addedBacking_=std::move(other.addedBacking_);
    original_=std::move(other.original_);add_=std::move(other.add_);root_=std::move(other.root_);randomState_=other.randomState_;
    return *this;
}

std::size_t PieceTreeBuffer::bytes(const NodePtr& node) noexcept {
    return node ? node->subtreeBytes : 0;
}
std::size_t PieceTreeBuffer::newlines(const NodePtr& node) noexcept {
    return node ? node->subtreeNewlines : 0;
}
void PieceTreeBuffer::refresh(Node& node) {
    node.subtreeBytes = bytes(node.left) + node.length + bytes(node.right);
    node.subtreeNewlines = newlines(node.left) + node.ownNewlines + newlines(node.right);
    node.subtreeCodePoints=node.ownCodePoints+(node.left?node.left->subtreeCodePoints:0)+(node.right?node.right->subtreeCodePoints:0);
    node.subtreeUtf16=node.ownUtf16+(node.left?node.left->subtreeUtf16:0)+(node.right?node.right->subtreeUtf16:0);
    node.subtreeDiskBytes=(node.source==Source::AddedDisk?node.length:0)+(node.left?node.left->subtreeDiskBytes:0)+(node.right?node.right->subtreeDiskBytes:0);
}

std::uint32_t PieceTreeBuffer::nextPriority() noexcept {
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 17;
    randomState_ ^= randomState_ << 5;
    return randomState_;
}

PieceTreeBuffer::NodePtr PieceTreeBuffer::merge(NodePtr left, NodePtr right) {
    if (!left) return right;
    if (!right) return left;
    if (left->priority > right->priority) {
        left->right = merge(std::move(left->right), std::move(right));
        refresh(*left);
        return left;
    }
    right->left = merge(std::move(left), std::move(right->left));
    refresh(*right);
    return right;
}

std::pair<PieceTreeBuffer::NodePtr, PieceTreeBuffer::NodePtr>
PieceTreeBuffer::split(NodePtr root, std::size_t offset) {
    if (!root) return {};
    const std::size_t leftBytes = bytes(root->left);
    if (offset < leftBytes) {
        auto parts = split(std::move(root->left), offset);
        root->left = std::move(parts.second);
        refresh(*root);
        return {std::move(parts.first), std::move(root)};
    }
    if (offset > leftBytes + root->length) {
        auto parts = split(std::move(root->right), offset - leftBytes - root->length);
        root->right = std::move(parts.first);
        refresh(*root);
        return {std::move(root), std::move(parts.second)};
    }
    if (offset == leftBytes) {
        auto left = std::move(root->left);
        root->left.reset();
        refresh(*root);
        return {std::move(left), std::move(root)};
    }
    if (offset == leftBytes + root->length) {
        auto right = std::move(root->right);
        root->right.reset();
        refresh(*root);
        return {std::move(root), std::move(right)};
    }

    const std::size_t firstLength = offset - leftBytes;
    const Source source = root->source;
    const std::size_t start = root->start;
    auto leftSubtree = std::move(root->left);
    auto rightSubtree = std::move(root->right);
    const auto data=sourceView(source,start,root->length);
    const auto firstNewlines=static_cast<std::size_t>(std::count(data.bytes.begin(),data.bytes.begin()+firstLength,'\n'));
    const auto secondNewlines=static_cast<std::size_t>(std::count(data.bytes.begin()+firstLength,data.bytes.end(),'\n'));
    NodePtr first(new Node(source, start, firstLength, root->priority, firstNewlines,codePoints(data.bytes.substr(0,firstLength)),utf16Units(data.bytes.substr(0,firstLength))));
    NodePtr second(new Node(source, start + firstLength, root->length - firstLength,
                            root->priority ^ 0x85ebca6bu, secondNewlines,codePoints(data.bytes.substr(firstLength)),utf16Units(data.bytes.substr(firstLength))));
    return {merge(std::move(leftSubtree), std::move(first)),
            merge(std::move(second), std::move(rightSubtree))};
}

void PieceTreeBuffer::load(std::string_view value) {
    std::lock_guard lock(*mutex_);
    clear();
    original_.assign(value);
    buildOriginalPieces();
}
void PieceTreeBuffer::buildOriginalPieces() {
    constexpr std::size_t pieceLimit = 4096;
    for (std::size_t start = 0; start < original_.size(); start += pieceLimit) {
        const std::size_t length = std::min(pieceLimit, original_.size() - start);
        const auto first = original_.begin() + static_cast<std::ptrdiff_t>(start);
        const auto last = first + static_cast<std::ptrdiff_t>(length);
        NodePtr piece = std::make_unique<Node>(Source::Original, start, length, nextPriority(),
            static_cast<std::size_t>(std::count(first, last, '\n')),codePoints(std::string_view(&*first,length)),utf16Units(std::string_view(&*first,length)));
        root_ = merge(std::move(root_), std::move(piece));
    }
}

void PieceTreeBuffer::clear() {
    std::lock_guard lock(*mutex_);
    root_.reset(); backing_.reset();addedBacking_.reset();
    originalId_=nextStorageId_++;addId_=nextStorageId_++;diskId_=nextStorageId_++;
    std::string{}.swap(original_);
    std::string{}.swap(add_);
}

void PieceTreeBuffer::insert(std::size_t offset, std::string_view value) {
    std::lock_guard lock(*mutex_);
    if (value.empty()) return;
    offset = std::min(offset, size());
    if(lineEditObserver) { const auto row=lineIndexAt(offset); lineEditObserver(row,offset-lineStart(row),0,std::count(value.begin(),value.end(),'\n')); }
    Source source=Source::Add;std::size_t start;
    if(value.size()>=1024u*1024u) {
        if(!addedBacking_){addedBacking_=std::make_shared<ChunkedFile>();diskId_=nextStorageId_++;}
        start=addedBacking_->append(value);source=Source::AddedDisk;
    } else {
        if(add_.size()+value.size()>4u*1024u*1024u)spillAddedArena();
        start=add_.size();add_.append(value);
    }
    auto parts = split(std::move(root_), offset);
    NodePtr inserted;
    constexpr std::size_t pieceLimit = 4096;
    for (std::size_t relative = 0; relative < value.size(); relative += pieceLimit) {
        const std::size_t length = std::min(pieceLimit, value.size() - relative);
        const auto first = value.begin() + static_cast<std::ptrdiff_t>(relative);
        const auto last = first + static_cast<std::ptrdiff_t>(length);
        NodePtr piece = std::make_unique<Node>(source, start + relative, length, nextPriority(),
            static_cast<std::size_t>(std::count(first, last, '\n')),codePoints(std::string_view(&*first,length)),utf16Units(std::string_view(&*first,length)));
        inserted = merge(std::move(inserted), std::move(piece));
    }
    root_ = merge(merge(std::move(parts.first), std::move(inserted)), std::move(parts.second));
}

void PieceTreeBuffer::erase(std::size_t offset, std::size_t length) {
    std::lock_guard lock(*mutex_);
    if (length == 0 || offset >= size()) return;
    length = std::min(length, size() - offset);
    if(lineEditObserver) {const auto row=lineIndexAt(offset); lineEditObserver(row,offset-lineStart(row),lineIndexAt(offset+length)-row,0);}
    auto first = split(std::move(root_), offset);
    auto second = split(std::move(first.second), length);
    root_ = merge(std::move(first.first), std::move(second.second));
    compactIfWasteful();
}

std::size_t PieceTreeBuffer::size() const noexcept {
    std::lock_guard lock(*mutex_);
    return bytes(root_);
}
std::size_t PieceTreeBuffer::lineCount() const noexcept {
    std::lock_guard lock(*mutex_);
    return newlines(root_) + 1;
}
std::size_t PieceTreeBuffer::countNewlinesBefore(const Node* node, std::size_t offset) const noexcept {
    if (!node || offset == 0) return 0;
    const std::size_t leftBytes = node->left ? node->left->subtreeBytes : 0;
    const std::size_t leftLines = node->left ? node->left->subtreeNewlines : 0;
    if (offset <= leftBytes)
        return countNewlinesBefore(node->left.get(), offset);
    std::size_t result = leftLines;
    const std::size_t inPiece = std::min(node->length, offset - leftBytes);
    if(inPiece==node->length) result+=node->ownNewlines;
    else {
        const auto source=sourceView(node->source,node->start,inPiece);
        result+=static_cast<std::size_t>(std::count(source.bytes.begin(),source.bytes.end(),'\n'));
    }
    if (offset <= leftBytes + node->length) return result;
    return result + countNewlinesBefore(node->right.get(), offset - leftBytes - node->length);
}
std::size_t PieceTreeBuffer::lineIndexAt(std::size_t offset) const noexcept {
    std::lock_guard lock(*mutex_);
    return countNewlinesBefore(root_.get(), std::min(offset, size()));
}

void PieceTreeBuffer::appendRange(const Node* node, std::size_t begin, std::size_t end,
                                  std::size_t base,std::string& out) const {
    if (!node || begin >= end) return;
    const std::size_t leftSize = node->left ? node->left->subtreeBytes : 0;
    const std::size_t pieceBase = base + leftSize;
    appendRange(node->left.get(), begin, end, base, out);
    const std::size_t pBegin = std::max(begin, pieceBase);
    const std::size_t pEnd = std::min(end, pieceBase + node->length);
    if (pBegin < pEnd) {
        const auto source=sourceView(node->source,node->start+pBegin-pieceBase,pEnd-pBegin);
        out.append(source.bytes);
    }
    appendRange(node->right.get(), begin, end, pieceBase + node->length, out);
}

std::string PieceTreeBuffer::slice(std::size_t offset, std::size_t length) const {
    std::lock_guard lock(*mutex_);
    offset = std::min(offset, size());
    length = std::min(length, size() - offset);
    std::string result;
    result.reserve(length);
    appendRange(root_.get(), offset, offset + length, 0, result);
    return result;
}
std::string PieceTreeBuffer::text() const {
    std::lock_guard lock(*mutex_);
    return slice(0,bytes(root_));
}
PieceTreeBuffer PieceTreeBuffer::snapshot(bool preserveStorage) const {
    std::lock_guard lock(*mutex_);
    if(preserveStorage || backing_ || addedBacking_) {
        PieceTreeBuffer copy; copy.originalId_=originalId_;copy.addId_=addId_;copy.diskId_=diskId_; copy.backing_=backing_; copy.addedBacking_=addedBacking_; copy.original_=original_;copy.add_=add_; copy.randomState_=randomState_;
        const auto clone=[&](const auto& self,const Node* source)->NodePtr {
            if(!source) return {};
            auto node=std::make_unique<Node>(source->source,source->start,source->length,source->priority,source->ownNewlines,source->ownCodePoints,source->ownUtf16);
            node->subtreeBytes=source->subtreeBytes;node->subtreeNewlines=source->subtreeNewlines;node->subtreeDiskBytes=source->subtreeDiskBytes;node->subtreeCodePoints=source->subtreeCodePoints;node->subtreeUtf16=source->subtreeUtf16;
            node->left=self(self,source->left.get());node->right=self(self,source->right.get());return node;
        };
        copy.root_=clone(clone,root_.get()); return copy;
    }
    // Copy only live text, not the append arena containing deleted/undone data.
    auto live=text();
    PieceTreeBuffer copy;
    copy.original_=std::move(live);
    copy.buildOriginalPieces();
    return copy;
}
std::size_t PieceTreeBuffer::storageBytes() const noexcept {
    std::lock_guard lock(*mutex_);
    return original_.capacity()+add_.capacity();
}
void PieceTreeBuffer::compactIfWasteful() {
    if(!root_ || !root_->subtreeDiskBytes)addedBacking_.reset();
    const auto live=bytes(root_);
    constexpr std::size_t slack=16u*1024u*1024u;
    const auto retained=original_.capacity()+add_.capacity();
    // Amortize rebuilding: compact only when retained storage is at least
    // three times the live text plus 16 MiB, never after every small deletion.
    if(retained<=slack || live>(retained-slack)/3) return;
    std::string compact; compact.reserve(live);
    appendRange(root_.get(),0,live,0,compact);
    root_.reset(); backing_.reset();addedBacking_.reset(); originalId_=nextStorageId_++;addId_=nextStorageId_++;diskId_=nextStorageId_++; original_=std::move(compact); std::string{}.swap(add_);
    buildOriginalPieces();
}
char PieceTreeBuffer::byteAt(std::size_t offset) const noexcept {
    std::lock_guard lock(*mutex_);
    if (offset >= size()) return '\0';
    const Node* node = root_.get();
    while (node) {
        const std::size_t leftBytes = node->left ? node->left->subtreeBytes : 0;
        if (offset < leftBytes) { node = node->left.get(); continue; }
        if (offset < leftBytes + node->length) {
            const auto source=sourceView(node->source,node->start+offset-leftBytes,1);
            return source.bytes[0];
        }
        offset -= leftBytes + node->length;
        node = node->right.get();
    }
    return '\0';
}

std::size_t PieceTreeBuffer::findNewline(const Node* node, std::size_t ordinal,
                                        std::size_t base) const noexcept {
    if (!node) return static_cast<std::size_t>(-1);
    const std::size_t leftLines = node->left ? node->left->subtreeNewlines : 0;
    const std::size_t leftBytes = node->left ? node->left->subtreeBytes : 0;
    if (ordinal < leftLines)
        return findNewline(node->left.get(), ordinal, base);
    ordinal -= leftLines;
    if(ordinal>=node->ownNewlines)
        return findNewline(node->right.get(),ordinal-node->ownNewlines,base+leftBytes+node->length);
    const auto source=sourceView(node->source,node->start,node->length);
    const auto begin=source.bytes.begin(),end=source.bytes.end();
    auto it = begin;
    for (std::size_t i = 0; i <= ordinal; ++i) {
        it = std::find(it, end, '\n');
        if (it == end) break;
        if (i == ordinal)
            return base + leftBytes + static_cast<std::size_t>(it - begin);
        ++it;
    }
    const std::size_t pieceLines = static_cast<std::size_t>(std::count(begin, end, '\n'));
    if (ordinal < pieceLines) return static_cast<std::size_t>(-1);
    return findNewline(node->right.get(), ordinal - pieceLines,
                       base + leftBytes + node->length);
}

std::size_t PieceTreeBuffer::lineStart(std::size_t lineIndex) const noexcept {
    std::lock_guard lock(*mutex_);
    if (lineIndex == 0) return 0;
    if (lineIndex >= lineCount()) return size();
    const auto newline = findNewline(root_.get(), lineIndex - 1, 0);
    return newline == static_cast<std::size_t>(-1) ? size() : newline + 1;
}
std::size_t PieceTreeBuffer::lineEnd(std::size_t lineIndex) const noexcept {
    std::lock_guard lock(*mutex_);
    if (lineIndex >= lineCount()) return size();
    const auto newline = findNewline(root_.get(), lineIndex, 0);
    return newline == static_cast<std::size_t>(-1) ? size() : newline;
}
std::string PieceTreeBuffer::line(std::size_t lineIndex) const {
    std::lock_guard lock(*mutex_);
    const auto begin = lineStart(lineIndex);
    const auto end = lineEnd(lineIndex);
    return slice(begin, end - begin);
}

void PieceTreeBuffer::forEachLine(const std::function<bool(std::size_t,std::string_view)>& visit) const {
    forEachLineBounded(std::numeric_limits<std::size_t>::max(),[&](std::size_t at,std::string_view bytes,bool){return visit(at,bytes);});
}
void PieceTreeBuffer::forEachLineBounded(std::size_t maxBytes,const std::function<bool(std::size_t,std::string_view,bool)>& visit) const {
    std::lock_guard lock(*mutex_);std::string line;std::size_t start=0,length=0;bool proceed=true;
    forEachChunk([&](std::string_view bytes){
        std::size_t at=0;
        while(at<bytes.size()) {
            const auto newline=bytes.find('\n',at),end=newline==std::string_view::npos?bytes.size():newline;
            line.append(bytes.substr(at,std::min(end-at,maxBytes-line.size())));length+=end-at;
            if(newline==std::string_view::npos)break;
            if(!visit(start,line,length>maxBytes)){proceed=false;return false;}
            start+=length+1;length=0;line.clear();at=end+1;
        }
        return true;
    });
    if(proceed)visit(start,line,length>maxBytes);
}

std::size_t utf8PreviousBoundary(std::string_view value, std::size_t offset) noexcept {
    if (offset == 0) return 0;
    offset = std::min(offset, value.size());
    --offset;
    while (offset > 0 && (static_cast<unsigned char>(value[offset]) & 0xC0u) == 0x80u)
        --offset;
    return offset;
}
std::size_t utf8NextBoundary(std::string_view value, std::size_t offset) noexcept {
    if (offset >= value.size()) return value.size();
    ++offset;
    while (offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xC0u) == 0x80u)
        ++offset;
    return offset;
}

PieceTreeBuffer::SourceView PieceTreeBuffer::sourceView(Source source,std::size_t start,std::size_t length) const {
    const auto file=source==Source::AddedDisk?addedBacking_:source==Source::Original?backing_:nullptr;
    if(file) {
        // Added pieces may start inside a page and cross its boundary.
        if(start/ChunkedFile::pageBytes!=(start+length-(length?1:0))/ChunkedFile::pageBytes) {
            auto owner=std::make_shared<const std::string>(file->read(start,length));
            return {owner,*owner};
        }
        auto page=file->page(start/ChunkedFile::pageBytes);
        const auto local=start%ChunkedFile::pageBytes;
        if(local+length>page->size()) throw std::runtime_error("Invalid backing-file text range");
        const std::string_view bytes(page->data()+local,length); return {std::move(page),bytes};
    }
    const auto& data=source==Source::Original?original_:add_;
    return {{},std::string_view(data).substr(start,length)};
}
std::size_t PieceTreeBuffer::cachedFileBytes() const {
    std::lock_guard lock(*mutex_);return backing_?backing_->cachedBytes():0;
}
bool PieceTreeBuffer::loadFile(const std::string& path,std::string* error,const std::function<bool()>& cancelled,
    const std::function<void(std::size_t,std::size_t,std::string_view)>& progress) {
    int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
    if(fd<0) {if(error)*error=std::strerror(errno);return false;}
    try {
        struct stat before{},after{};
        if(::fstat(fd,&before)!=0 || !S_ISREG(before.st_mode)) throw std::runtime_error("Expected a regular file");
        PieceTreeBuffer loaded;
        if(before.st_size<4*1024*1024) {
            std::string bytes; bytes.reserve(before.st_size);
            char chunk[64*1024];
            while(bytes.size()<static_cast<std::size_t>(before.st_size)) {
                if(cancelled && cancelled()) throw std::runtime_error("Loading cancelled");
                const auto got=::read(fd,chunk,std::min<std::size_t>(sizeof(chunk),before.st_size-bytes.size()));
                if(got<0 && errno==EINTR) continue;
                if(got<0) throw std::runtime_error(std::strerror(errno));
                if(!got) throw std::runtime_error("File shortened while loading");
                bytes.append(chunk,got);
                if(progress)progress(bytes.size(),before.st_size,{});
            }
            loaded.load(bytes);
        } else {
            loaded.backing_=std::make_shared<ChunkedFile>();
            std::size_t position=0; char chunk[64*1024];
            while(position<static_cast<std::size_t>(before.st_size)) {
                if(cancelled && cancelled()) throw std::runtime_error("Loading cancelled");
                // Fill complete pages so every indexed piece fits one cache page.
                std::size_t got=0;
                const auto wanted=std::min<std::size_t>(sizeof(chunk),before.st_size-position);
                while(got<wanted) {
                    const auto count=::read(fd,chunk+got,wanted-got);
                    if(count<0 && errno==EINTR) continue;
                    if(count<0) throw std::runtime_error(std::strerror(errno));
                    if(!count)throw std::runtime_error("File shortened while loading");
                    got+=count;
                }
                if(!got)break;
                loaded.backing_->append({chunk,got});
                for(std::size_t offset=0;offset<got;offset+=4096) {
                    const auto length=std::min<std::size_t>(4096,got-offset);
                    const auto nl=std::count(chunk+offset,chunk+offset+length,'\n');
                    auto node=std::make_unique<Node>(Source::Original,position+offset,length,loaded.nextPriority(),nl,codePoints({chunk+offset,length}),utf16Units({chunk+offset,length}));
                    loaded.root_=merge(std::move(loaded.root_),std::move(node));
                }
                if(progress) {
                    const auto preview=position==0?std::string_view(chunk,got):std::string_view{};
                    progress(position+got,before.st_size,preview);
                }
                position+=got;
            }
        }
        if(::fstat(fd,&after)!=0 || before.st_size!=after.st_size || before.st_mtim.tv_sec!=after.st_mtim.tv_sec || before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec || before.st_ctim.tv_sec!=after.st_ctim.tv_sec || before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec)
            throw std::runtime_error("File changed while it was being loaded");
        ::close(fd);fd=-1;
        const auto guard=mutex_; std::lock_guard lock(*guard);
        *this=std::move(loaded);return true;
    } catch(const std::exception& problem) {
        if(fd>=0)::close(fd);
        if(error)*error=problem.what();
        return false;
    }
}
bool PieceTreeBuffer::forEachChunk(const std::function<bool(std::string_view)>& visit) const {
    std::lock_guard lock(*mutex_);bool proceed=true;
    const auto walk=[&](const auto& self,const Node* node)->void {
        if(!node || !proceed)return;
        self(self,node->left.get()); if(!proceed)return;
        const auto data=sourceView(node->source,node->start,node->length);proceed=visit(data.bytes);
        self(self,node->right.get());
    };
    walk(walk,root_.get());return proceed;
}

void PieceTreeBuffer::spillAddedArena() {
    if(add_.empty())return;
    if(!addedBacking_){addedBacking_=std::make_shared<ChunkedFile>();diskId_=nextStorageId_++;}
    const auto base=addedBacking_->append(add_);
    const auto rewrite=[&](const auto& self,Node* node)->void {
        if(!node)return;
        self(self,node->left.get());self(self,node->right.get());
        if(node->source==Source::Add){node->source=Source::AddedDisk;node->start+=base;}
        refresh(*node);
    };
    rewrite(rewrite,root_.get());std::string{}.swap(add_);addId_=nextStorageId_++;
}

std::size_t PieceTreeBuffer::codePointsBefore(std::size_t offset) const {
    std::lock_guard lock(*mutex_);offset=std::min(offset,size());std::size_t count=0;auto* node=root_.get();
    while(node) {
        const auto leftBytes=bytes(node->left),leftPoints=node->left?node->left->subtreeCodePoints:0;
        if(offset<=leftBytes){node=node->left.get();continue;}
        count+=leftPoints;
        const auto inside=std::min(node->length,offset-leftBytes);
        count+=inside==node->length?node->ownCodePoints:codePoints(sourceView(node->source,node->start,inside).bytes);
        if(offset<=leftBytes+node->length)break;
        offset-=leftBytes+node->length;node=node->right.get();
    }
    return count;
}
std::size_t PieceTreeBuffer::byteAtCodePoint(std::size_t index) const {
    std::lock_guard lock(*mutex_);std::size_t base=0;auto* node=root_.get();
    while(node) {
        const auto left=node->left?node->left->subtreeCodePoints:0;
        if(index<left){node=node->left.get();continue;}
        index-=left;base+=bytes(node->left);
        if(index<node->ownCodePoints){
            const auto view=sourceView(node->source,node->start,node->length);
            for(std::size_t at=0;at<view.bytes.size();++at)if((static_cast<unsigned char>(view.bytes[at])&0xc0)!=0x80){if(index==0)return base+at;--index;}
        }
        index-=node->ownCodePoints;base+=node->length;node=node->right.get();
    }
    return size();
}

std::size_t PieceTreeBuffer::utf16Before(std::size_t offset) const {
    std::lock_guard lock(*mutex_);
    offset=std::min(offset,size());std::size_t count=0;auto* node=root_.get();
    while(node) {
        const auto leftBytes=bytes(node->left),leftUnits=node->left?node->left->subtreeUtf16:0;
        if(offset<=leftBytes){node=node->left.get();continue;}
        count+=leftUnits;
        const auto inside=std::min(node->length,offset-leftBytes);
        count+=inside==node->length?node->ownUtf16:utf16Units(sourceView(node->source,node->start,inside).bytes);
        if(offset<=leftBytes+node->length)break;
        offset-=leftBytes+node->length;node=node->right.get();
    }
    return count;
}
std::size_t PieceTreeBuffer::byteAtUtf16(std::size_t index) const {
    std::lock_guard lock(*mutex_);std::size_t base=0;auto* node=root_.get();
    while(node) {
        const auto left=node->left?node->left->subtreeUtf16:0;
        if(index<left){node=node->left.get();continue;}
        index-=left;base+=bytes(node->left);
        if(index<node->ownUtf16){
            const auto view=sourceView(node->source,node->start,node->length);
            for(std::size_t at=0;at<view.bytes.size();++at){
                const auto c=static_cast<unsigned char>(view.bytes[at]);if((c&0xc0)==0x80)continue;
                const std::size_t units=c>=0xf0?2:1;if(index<units)return base+at;index-=units;
            }
        }
        index-=node->ownUtf16;base+=node->length;node=node->right.get();
    }
    return size();
}

std::vector<PieceTreeBuffer::RecoveryPiece> PieceTreeBuffer::recoveryPieces() const {
    std::lock_guard lock(*mutex_);
    std::vector<RecoveryPiece> result;
    std::size_t position=0;
    const auto walk=[&](const auto& self,const Node* node)->void {
        if(!node)return;
        self(self,node->left.get());
        const auto id=node->source==Source::Original?originalId_:(node->source==Source::Add?addId_:diskId_);
        result.push_back({id,node->start,node->length,position});position+=node->length;
        self(self,node->right.get());
    };
    walk(walk,root_.get());return result;
}
