#include "VimState.hpp"
#include "StoredClipboard.hpp"
#include <QGuiApplication>
#include <QClipboard>
#include <algorithm>

namespace VimState {
QVector<Input> keys(const QString& text) {
    QVector<Input> result;
    for(const char32_t ch:text.toUcs4()) {
        Input input;input.text=QString::fromUcs4(&ch,1);
        if(ch==27){input.key=Qt::Key_Escape;input.text={};}
        else if(ch=='\n' || ch=='\r'){input.key=Qt::Key_Return;input.text="\r";}
        else if(ch=='\t')input.key=Qt::Key_Tab;
        else if(ch==8 || ch==127)input.key=Qt::Key_Backspace;
        else if(ch<32){input.key=Qt::Key_A+int(ch)-1;input.modifiers=Qt::ControlModifier;}
        else {input.key=ch<128?QChar(ushort(ch)).toUpper().unicode():0;if(ch>='A' && ch<='Z')input.modifiers=Qt::ShiftModifier;}
        result.append(input);
    }
    return result;
}
Session& Session::instance(){static Session state;return state;}
std::shared_ptr<ChunkedFile> convertNewlines(const std::shared_ptr<ChunkedFile>& source,std::string_view eol,const std::function<void()>& checkpoint) {
    auto target=std::make_shared<ChunkedFile>();std::string pending;bool cr=false;
    for(std::size_t p=0;p<source->size();p+=64*1024){if(checkpoint)checkpoint();const auto bytes=source->read(p,std::min<std::size_t>(64*1024,source->size()-p));
        for(char ch:bytes){if(ch=='\n'){pending+=eol;cr=false;}else {if(cr)pending+='\r';cr=ch=='\r';if(!cr)pending+=ch;}if(pending.size()>=64*1024){target->append(pending);pending.clear();}}
    }
    if(cr)pending+='\r';
    if(!pending.empty())target->append(pending);source->dropCache();target->dropCache();return target;
}
Register text(const QString& value,Register::Kind kind,int width) {
    Register result;result.kind=kind;result.width=width;result.text=std::make_shared<ChunkedFile>();
    const auto bytes=value.toUtf8();result.text->append({bytes.data(),std::size_t(bytes.size())});result.text->dropCache();return result;
}
Register append(const Register& old,const Register& value,const std::function<void()>& checkpoint) {
    Register joined=value;joined.text=std::make_shared<ChunkedFile>();
    const auto copy=[&](const std::shared_ptr<ChunkedFile>& source){if(source)for(std::size_t at=0;at<source->size();at+=64*1024){if(checkpoint)checkpoint();joined.text->append(source->read(at,std::min<std::size_t>(64*1024,source->size()-at)));}};
    if(old.kind==Register::Block && value.kind==Register::Block) {
        copy(old.text);joined.text->append("\n");copy(value.text);joined.width=std::max(old.width,value.width);joined.column=old.column;
    } else {
        joined.kind=old.kind==Register::Line || value.kind==Register::Line?Register::Line:Register::Character;
        copy(old.text);if(joined.kind==Register::Line && joined.text->size() && joined.text->read(joined.text->size()-1,1)!="\n")joined.text->append("\n");
        copy(value.text);if(joined.kind==Register::Line && joined.text->size() && joined.text->read(joined.text->size()-1,1)!="\n")joined.text->append("\n");
    }
    if((!old.macro.isEmpty() || !value.macro.isEmpty()) && joined.text->size()<=1024*1024){
        const auto inputs=[](const Register& v){if(!v.macro.isEmpty())return v.macro;const auto bytes=v.text?v.text->read(0,v.text->size()):std::string{};return keys(QString::fromUtf8(bytes.data(),qsizetype(bytes.size())));};joined.macro=inputs(old)+inputs(value);
    }else joined.macro.clear();
    if(old.text)old.text->dropCache();if(value.text)value.text->dropCache();joined.text->dropCache();return joined;
}
Register Session::read(QChar name) const {
    if(name.isNull())name='"';
    if(name=='+' || name=='*') {
        const auto mode=name=='*'?QClipboard::Selection:QClipboard::Clipboard;
        const auto* mime=QGuiApplication::clipboard()->mimeData(mode);if(!mime)return {};
        Register result;const auto* stored=dynamic_cast<const StoredClipboard*>(mime);
        if(stored)result.text=stored->file;else result=text(mime->text());
        result.kind=Register::Kind(mime->property("turtleVimKind").toInt());result.width=mime->property("turtleVimWidth").toInt();result.column=mime->property("turtleVimColumn").toInt();return result;
    }
    if(name=='_')return {};
    return registers_.value(name.toLower());
}
void Session::set(QChar name,const Register& value) {
    if(name=='_')return;
    if(name=='+' || name=='*') {
        if(!value.text)return;
        auto* mime=new StoredClipboard(value.text);mime->setProperty("turtleVimKind",int(value.kind));mime->setProperty("turtleVimWidth",value.width);mime->setProperty("turtleVimColumn",value.column);
        QGuiApplication::clipboard()->setMimeData(mime,name=='*'?QClipboard::Selection:QClipboard::Clipboard);return;
    }
    if(name.isUpper() && registers_.contains(name.toLower())) {
        registers_[name.toLower()]=append(registers_.value(name.toLower()),value);return;
    }
    registers_[name.toLower()]=value;
}
void Session::write(QChar selected,const Register& value,bool yank,bool smallDelete,bool forceNumbered,const Register* appended) {
    if(selected=='_')return;
    if(selected.isNull())selected='"';
    if(selected!='"'){if(appended && selected.isUpper())set(selected.toLower(),*appended);else set(selected,value);}
    if(yank){if(selected=='"')set('0',value);}
    else if(smallDelete){if(selected=='"')set('-',value);}
    if(!yank && (!smallDelete || forceNumbered)){for(int i=9;i>1;--i)registers_[QChar('0'+i)]=registers_.value(QChar('0'+i-1));set('1',value);}
    set('"',selected.isUpper()?read(selected.toLower()):value);
}
void Session::clear(){registers_.clear();recording={};recordingOwner=nullptr;lastMacro={};recordingInputs.clear();recordingBytes=0;lastChange={};commandHistory.clear();lastCommand.clear();previousExCommand.clear();lastExRegister={};searchPattern.clear();searchOffset.clear();lastSubPattern.clear();lastReplacement.clear();lastSubFlags.clear();searchForward=highlightSearch=incrementalSearch=wrapSearch=true;globalSubstitute=false;searchHistory.clear();searchOptions={};dictionary.clear();thesaurus.clear();tags="./tags,tags";numberFormats="bin,hex";textWidth=0;joinSpaces=false;globalMarks.clear();marks_.clear();markFiles_.clear();jumps_.clear();anchors_.clear();}
Anchor Session::anchor(QObject* owner,const QString& path,std::size_t offset,QPoint position){auto p=std::make_shared<Location>(Location{owner,path,offset,position});anchors_.append(p);return p;}
QHash<QChar,Anchor>& Session::marks(const QString& key) {
    if(!marks_.contains(key)){if(markFiles_.size()>=256){marks_.remove(markFiles_.takeFirst());}markFiles_.append(key);}
    return marks_[key];
}
Jumps& Session::jumps(QObject* window) {
    if(!jumps_.contains(window)){jumps_.insert(window,{});connect(window,&QObject::destroyed,this,[this,window]{jumps_.remove(window);});}
    return jumps_[window];
}
QVector<Anchor> Session::anchors(QObject* owner) {
    QVector<Anchor> result;
    for(auto it=anchors_.begin();it!=anchors_.end();){if(auto p=it->lock()){if(p->owner==owner)result.append(p);++it;}else it=anchors_.erase(it);}
    return result;
}
void Session::attach(QObject* owner,const QString& path){for(auto& weak:anchors_)if(auto p=weak.lock();p && !path.isEmpty() && p->path==path && !p->owner){p->owner=owner;p->lostPositions.clear();}}
void Session::rename(QObject* owner,const QString& oldKey,const QString& newKey,const QString& path){
    for(const auto& anchor:anchors(owner))anchor->path=path;
    if(oldKey==newKey)return;
    const auto old=marks_.value(oldKey);QHash<QChar,Anchor> moved;
    for(auto it=old.cbegin();it!=old.cend();++it)if(it.value()->owner==owner){moved.insert(it.key(),it.value());marks_[oldKey].remove(it.key());}
    auto& destination=marks(newKey);for(auto it=moved.cbegin();it!=moved.cend();++it)destination.insert(it.key(),it.value());
}
void Session::discardBufferMarks(QObject* owner, const QString& key, const QString& path) {
    const auto belongsToBuffer = [&](const Anchor& anchor) {
        return anchor && (anchor->owner ? anchor->owner == owner : !path.isEmpty() && anchor->path == path);
    };
    globalMarks.removeIf([&](auto it) { return belongsToBuffer(it.value()); });
    if (auto it = marks_.find(key); it != marks_.end()) {
        it.value().removeIf([&](auto mark) { return belongsToBuffer(mark.value()); });
        if (it.value().isEmpty()) {
            marks_.erase(it);
            markFiles_.removeAll(key);
        }
    }
    for (auto& jumps : jumps_) {
        if (belongsToBuffer(jumps.previous)) jumps.previous.reset();
        for (qsizetype i = jumps.entries.size(); i-- > 0;) {
            if (!belongsToBuffer(jumps.entries[i])) continue;
            jumps.entries.removeAt(i);
            if (i < jumps.index) --jumps.index;
        }
    }
}
std::size_t Session::project(std::size_t p,std::size_t at,std::size_t removed,std::size_t inserted){if(p<at)return p;if(p<at+removed)return at;return p-std::min(removed,p-at)+(p>at || removed?inserted:0);}
}
