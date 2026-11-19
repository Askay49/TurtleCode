#include "EditorWidget.hpp"
#include "OccurrenceMatching.hpp"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QTextBoundaryFinder>

void EditorWidget::completeVimInsert(int direction,QChar source) {
    if(!vimInsertCandidates_.isEmpty() && vimCompletionTextRevision_==lspRevision_ && cursorPosition_==vimCompletionEnd_) {
        vimInsertCandidate_=(vimInsertCandidate_+direction+vimInsertCandidates_.size())%vimInsertCandidates_.size();
        const auto value=vimInsertCandidates_[vimInsertCandidate_].toUtf8();
        const auto handling=std::exchange(vimHandlingKey_,true),capture=std::exchange(vimCaptureInsertEdit_,true);
        const auto guard=qScopeGuard([&]{vimHandlingKey_=handling;vimCaptureInsertEdit_=capture;});
        replaceRange(vimCompletionStart_,vimCompletionEnd_-vimCompletionStart_,value.toStdString());
        vimCompletionEnd_=cursorPosition_;vimCompletionTextRevision_=lspRevision_;
        // Dot repeats the accepted text; macros keep their original provider keys.
        const auto text=QString::fromUtf8(value);qsizetype common=0;
        while(common<text.size() && common<vimCompletionOriginal_.size() && text[common]==vimCompletionOriginal_[common])++common;
        if(common && common<text.size() && text[common].isLowSurrogate())--common;
        VimState::Input input;input.kind=VimState::Input::Edit;input.text=text.mid(common);
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,vimCompletionOriginal_.mid(common));while(finder.toNextBoundary()>=0)++input.back;
        if(vimCompletionChangeIndex_>=0 && vimCompletionChangeIndex_<=vimChangeKeys_.size()){vimChangeKeys_.resize(vimCompletionChangeIndex_);vimChangeKeys_.append(input);}
        return;
    }
    hideCompletions();
    const auto completionStart=[](const QVector<VimState::Input>& keys){
        auto index=keys.size();if(index)--index;
        if(index && keys[index-1].modifiers==int(Qt::ControlModifier) && keys[index-1].key==Qt::Key_X)--index;
        return int(index);
    };
    vimCompletionChangeIndex_=completionStart(vimChangeKeys_);
    const auto at=cursorPosition_,row=currentLine(),start=vimLineStart(row);
    auto begin=at;
    if(source=='L')begin=std::min(at,VimMotions::firstText(buffer_,row));
    else while(begin>start) {
        const auto previous=graphemeBoundaryAt(begin,false);const auto bytes=buffer_.slice(previous,begin-previous);const auto value=QString::fromUtf8(bytes.c_str());
        if(source=='F' ? value.contains(QRegularExpression("[\\s<>\"|]")) : !OccurrenceMatching::isWord(value,0))break;
        begin=previous;
    }
    const auto prefix=QString::fromUtf8(buffer_.slice(begin,at-begin).c_str());
    auto candidates=std::make_shared<QStringList>();
    const auto path=filePath_;const auto& state=VimState::Session::instance();
    const auto files=vimBufferOption(source=='K'?"dictionary":source=='T'?"thesaurus":"tags").toString();
    const auto options=state.searchOptions;
    const auto exCommands = source == 'V' ? vimExRegistry().complete(prefix) : QStringList{};
    runBulkOperation(QStringLiteral("Vim completion…"),[candidates,prefix,source,at,begin,path,files,options,exCommands](PieceTreeBuffer& buffer,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        // Work and candidate counts are bounded independently of document size.
        constexpr std::size_t budget=16*1024*1024;std::size_t scanned=0;int entries=0;
        const bool ignore=options.ignoreCase && !(options.smartCase && prefix.contains(QRegularExpression("\\p{Lu}")));
        const auto sensitivity=ignore?Qt::CaseInsensitive:Qt::CaseSensitive;
        QSet<QString> seen;QStringList before,after;
        const auto add=[&](const QString& text,bool prior=false,bool synonym=false){
            if(text.size()>65536 || (!synonym && !text.startsWith(prefix,sensitivity)) || seen.contains(text) || seen.size()>=2000)return;
            seen.insert(text);(prior?before:after).append(text);
        };
        static const QRegularExpression words("[\\p{L}\\p{N}_\\p{M}]+");
        const auto scanLine=[&](const QString& line,bool prior=false,std::size_t offset=0,bool current=false){
            if(source=='L'){if(!current || offset!=buffer.lineStart(buffer.lineIndexAt(at)))add(line.trimmed(),prior);return;}
            if(source==']'){if(!line.startsWith('!'))add(line.section('\t',0,0));return;}
            QString text=line;
            if(source=='D') {static const QRegularExpression define("^\\s*#\\s*define\\s+([\\p{L}_][\\p{L}\\p{N}_]*)");const auto match=define.match(line);if(match.hasMatch())add(match.captured(1),prior);return;}
            const bool cursorLine=current && offset==buffer.lineStart(buffer.lineIndexAt(at));
            const int cursorColumn=cursorLine?QString::fromUtf8(buffer.slice(offset,at-offset).c_str()).size():0;
            const int prefixColumn=cursorLine?QString::fromUtf8(buffer.slice(offset,begin-offset).c_str()).size():0;
            auto matches=words.globalMatch(text);QStringList found;bool synonym=false;
            while(matches.hasNext()){const auto match=matches.next();found.append(match.captured());if(source=='T' && match.captured().startsWith(prefix,sensitivity))synonym=true;
                if(source!='T'){if(!cursorLine || match.capturedStart()!=prefixColumn)add(match.captured(),cursorLine?match.capturedStart()<cursorColumn:prior);}}
            if(source=='T' && synonym)for(const auto& word:found)add(word,false,true);
        };
        const auto folder=QFileInfo(path).absoluteDir();
        if(source=='F') {
            const QFileInfo info(prefix);QDirIterator iterator(folder.absoluteFilePath(info.path()),QDir::AllEntries|QDir::NoDotAndDotDot);
            QStringList names;while(iterator.hasNext() && entries++<20000){step(50);iterator.next();const auto file=iterator.fileInfo();if(file.fileName().startsWith(info.fileName(),sensitivity))names.append((info.path()=="."?QString{}:info.path()+"/")+file.fileName()+(file.isDir()?"/":""));if(names.size()>=2000)break;}
            names.sort();for(const auto& name:names)add(name);
        } else if(source=='V') {
            for(const auto& name:exCommands)add(name);
        } else {
            QStringList pending;QSet<QString> visited;
            const auto include=[&](const QString& line,const QDir& directory){
                if(source!='I' && source!='D')return;
                static const QRegularExpression expression("^\\s*#\\s*include\\s*[<\"]([^>\"]+)[>\"]");
                const auto match=expression.match(line);if(match.hasMatch() && pending.size()<256)pending.append(directory.absoluteFilePath(match.captured(1)));
            };
            if(source!='K' && source!='T' && source!=']') {
                for(std::size_t row=0;row<buffer.lineCount() && scanned<budget && seen.size()<2000;++row) {
                    step(40);const auto offset=buffer.lineStart(row),length=VimMotions::lineEnd(buffer,row)-offset;
                    scanned+=std::min<std::size_t>(length+1,65536);if(length>65536)continue;
                    const auto bytes=buffer.slice(offset,length);const auto line=QString::fromUtf8(bytes.data(),bytes.size());
                    scanLine(line,offset<at,offset,true);include(line,folder);
                }
            } else for(const auto& file:files.split(',',Qt::SkipEmptyParts))pending.append(folder.absoluteFilePath(file));
            while(!pending.isEmpty() && visited.size()<256 && scanned<budget && seen.size()<2000) {
                step(50);const auto name=pending.takeFirst();const QFileInfo info(name);const auto canonical=info.canonicalFilePath();if(canonical.isEmpty() || visited.contains(canonical))continue;visited.insert(canonical);
                QFile file(canonical);if(!file.open(QIODevice::ReadOnly))continue;
                while(!file.atEnd() && scanned<budget && seen.size()<2000){step(60);const auto bytes=file.readLine(65536);scanned+=bytes.size();if(bytes.size()==65536 && !bytes.endsWith('\n')){while(!file.atEnd() && scanned<budget){step(60);const auto rest=file.readLine(65536);scanned+=rest.size();if(rest.endsWith('\n'))break;}continue;}
                    const auto line=QString::fromUtf8(bytes);scanLine(line);include(line,info.absoluteDir());}
            }
        }
        *candidates=after+before;return HistoryResult{at,{at}};
    },false,{},[this,candidates,prefix,begin,at,direction,source](bool ok){
        if(!ok || candidates->isEmpty())return;
        vimInsertCandidates_=*candidates;vimInsertCandidates_.append(prefix);vimCompletionOriginal_=prefix;vimInsertCompletionSource_=source;
        vimCompletionStart_=begin;vimCompletionEnd_=at;vimInsertCandidate_=direction>0?-1:vimInsertCandidates_.size()-1;vimCompletionTextRevision_=lspRevision_;
        completeVimInsert(direction);
    });
}
