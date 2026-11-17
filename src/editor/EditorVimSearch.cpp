#include "EditorWidget.hpp"
#include "app/I18n.hpp"
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QTimer>
#include <QLineEdit>
#include <QLabel>
#include <QScrollBar>
#include <QRegularExpression>
#include <QScopeGuard>
#include <algorithm>

namespace {
struct Query {QString pattern,offset,error;};
int characterOffset(const QString& offset){return !offset.isEmpty() && QStringLiteral("esb").contains(offset[0])?offset.mid(1).toInt():0;}
Query query(const QString& input,QChar delimiter){
    Query result;qsizetype at=0;
    for(;at<input.size();++at){const auto c=input[at];if(c==delimiter){++at;break;}result.pattern+=c;if(c=='\\' && at+1<input.size())result.pattern+=input[++at];}
    result.offset=input.mid(at);
    if(!result.offset.isEmpty() && !QRegularExpression("^([esb]([+-]?[0-9]+)?|[+-]?[0-9]+|[+-])$").match(result.offset).hasMatch())result.error="E488: Invalid search offset";
    if(!result.offset.isEmpty()){
        auto value=result.offset;if(QStringLiteral("esb").contains(value[0]))value.remove(0,1);
        if(!value.isEmpty() && value!="+" && value!="-"){bool ok=false;const auto number=value.toLongLong(&ok);if(!ok || number>10000 || number< -10000)result.error="Search offset exceeds 10000 characters or lines";}
    }
    return result;
}
struct SearchResult {
    std::shared_ptr<VimSearch::Subject> subject;VimSearch::Navigation navigation;
    std::vector<std::pair<std::size_t,std::size_t>> highlights;QString error;
};
void collect(SearchResult& result,const PieceTreeBuffer& buffer,const VimSearch::Pattern& pattern,std::size_t first,std::size_t last,const VimSearch::Checkpoint& check){
    const auto end=last+1<buffer.lineCount()?buffer.lineStart(last+1):result.subject->size()+1;
    auto at=std::max(result.subject->begin(),buffer.lineStart(first?first-1:0));
    while(at<=result.subject->size() && result.highlights.size()<2000){
        const auto match=pattern.find(*result.subject,at,check);if(!match || match->begin>=end)break;
        if(match->end>=buffer.lineStart(first))result.highlights.emplace_back(match->begin,match->end);
        at=match->end>match->begin?match->end:result.subject->next(match->rawBegin);
    }
}
}
void EditorWidget::invalidateVimSearch(){
    if(vimSearchCancelled_)vimSearchCancelled_->store(true);
    ++vimSearchGeneration_;vimSearchSubject_.reset();vimSearchHighlights_.clear();vimLastSearchMatch_.reset();vimSearchRunning_=false;
    if(vim_.enabled())QTimer::singleShot(0,this,&EditorWidget::updateVimSearchHighlights);
}
void EditorWidget::startVimSearch(const VimController::Action& action){
    using C=VimController::Command;auto& state=VimState::Session::instance();
    if(action.operation!=C::Consume && action.command!=C::SearchForward && action.command!=C::SearchBackward)beginVimChange();
    if(action.command==C::SearchForward || action.command==C::SearchBackward){
        vimSearchAction_=action;vimSearchOrigin_=cursorPosition_;vimSearchOriginAnchor_=anchor_;
        vimSearchOriginTop_=verticalScrollBar()->value();vimSearchOriginLeft_=horizontalScrollBar()->value();
        showVimCommandLine(0,action.command==C::SearchForward?'/':'?');return;
    }
    if(action.command==C::SearchNext || action.command==C::SearchPrevious){
        if(state.searchPattern.isEmpty()){emit highlightingStatusChanged("E35: No previous regular expression");if(isVimReplaying())vimReplayCancel_=true;return;}
        navigateVimSearch(state.searchPattern,action.command==C::SearchPrevious?!state.searchForward:state.searchForward,action,state.searchOffset,true);return;
    }
    if(action.command==C::SelectMatch) {
        if(state.searchPattern.isEmpty()){emit highlightingStatusChanged("E35: No previous regular expression");return;}
        navigateVimSearch(state.searchPattern,!action.around,action,{},false);return;
    }
    const auto original=cursorPosition_;auto word=std::make_shared<VimMotions::Range>();auto pattern=std::make_shared<QString>();
    runBulkOperation(I18n::text("Searching…"),[original,word,pattern,action](PieceTreeBuffer& buffer,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        auto at=original;const auto blank=[&](std::size_t p){return p<buffer.size() && (buffer.byteAt(p)==' ' || buffer.byteAt(p)=='\t');};
        const auto keyword=[](std::string_view bytes){const auto chars=QString::fromUtf8(bytes.data(),qsizetype(bytes.size())).toUcs4();return !chars.isEmpty() && (chars[0]=='_' || QChar::isLetterOrNumber(chars[0]));};
        if(!keyword(buffer.slice(at,std::min<std::size_t>(4,buffer.size()-at)))){
            std::optional<std::size_t> fallback,found;
            VimBlock::forEachCell(buffer,buffer.lineIndexAt(original),4,[&](std::size_t begin,std::size_t,int,int,std::string_view bytes){
                if(begin<original)return true;
                if(keyword(bytes)){found=begin;return false;}
                if(!fallback && !bytes.empty() && bytes!=" " && bytes!="\t")fallback=begin;
                return true;
            },[&]{step(10);});
            at=found.value_or(fallback.value_or(VimMotions::lineEnd(buffer,buffer.lineIndexAt(original))));
        }
        while(blank(at)){step(10);++at;}
        if(at>=VimMotions::lineEnd(buffer,buffer.lineIndexAt(original)))return HistoryResult{original,{original}};
        VimController::Action object;object.command=C::Object;object.character='w';
        *word=VimMotions::resolve(buffer,at,object,[&]{step(20);});
        if(word->valid && word->end-word->begin<=65536){const auto bytes=buffer.slice(word->begin,word->end-word->begin);const auto value=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));const bool keyword=!value.isEmpty() && (value[0].isLetterOrNumber() || value[0]=='_');*pattern=VimSearch::literal(value,keyword && !action.around);}
        return HistoryResult{original,{original}};
    },false,{},[this,pattern,word,action](bool ok){
        if(!ok || pattern->isEmpty()){emit highlightingStatusChanged("E348: No string under cursor");if(isVimReplaying())vimReplayCancel_=true;return;}
        auto& state=VimState::Session::instance();*pattern+=state.searchOptions.ignoreCase?QStringLiteral("\\c"):QStringLiteral("\\C");state.searchPattern=*pattern;state.searchOffset.clear();state.searchForward=action.command==C::SearchWord;state.set('/',VimState::text(*pattern));
        state.searchHistory.removeAll(*pattern);state.searchHistory.append(*pattern);while(state.searchHistory.size()>200)state.searchHistory.removeFirst();
        navigateVimSearch(*pattern,state.searchForward,action,{},false,word->begin);
    });
}
void EditorWidget::previewVimSearch(){
    if(!vimExActive_ || vimCommandPrompt_==':')return;
    if(vimSearchCancelled_)vimSearchCancelled_->store(true);
    const auto generation=++vimSearchGeneration_;vimSearchRunning_=false;
    if(!VimState::Session::instance().incrementalSearch || vimExInput_->text().isEmpty()){
        vimExHints_->setText(I18n::text("Enter: search · Esc: cancel · ↑/↓: history"));
        cursorPosition_=vimSearchOrigin_;anchor_=vimSearchOriginAnchor_;cursors_={cursorPosition_};cursorAnchors_={anchor_};verticalScrollBar()->setValue(vimSearchOriginTop_);horizontalScrollBar()->setValue(vimSearchOriginLeft_);vimSearchHighlights_.clear();viewport()->update();return;
    }
    vimSearchRunning_=true;
    QTimer::singleShot(65,this,[this,generation]{
        if(generation!=vimSearchGeneration_ || !vimExActive_)return;
        const auto input=query(vimExInput_->text(),vimCommandPrompt_);
        if(!input.error.isEmpty()){vimExHints_->setText(input.error);vimSearchRunning_=false;return;}
        const auto pattern=input.pattern.isEmpty()?VimState::Session::instance().searchPattern:input.pattern;
        auto cancelled=std::make_shared<std::atomic<bool>>(false);vimSearchCancelled_=cancelled;
        const auto revision=lspRevision_;const auto old=vimSearchSubjectRevision_==revision?vimSearchSubject_:nullptr;
        const auto snapshot=std::make_shared<PieceTreeBuffer>(buffer_.snapshot());const auto options=VimState::Session::instance().searchOptions;
        const auto origin=vimSearchOrigin_;const bool forward=vimCommandPrompt_=='/';const auto count=vimSearchAction_.count;const auto finalNewline=vimFinalNewline_;
        auto* watcher=new QFutureWatcher<std::shared_ptr<SearchResult>>(this);
        connect(watcher,&QFutureWatcher<std::shared_ptr<SearchResult>>::finished,this,[this,watcher,generation,revision,input,forward]{
            const auto result=watcher->result();watcher->deleteLater();if(generation!=vimSearchGeneration_ || revision!=lspRevision_ || !vimExActive_)return;
            vimSearchRunning_=false;
            if(!result->error.isEmpty()){cursorPosition_=vimSearchOrigin_;anchor_=vimSearchOriginAnchor_;cursors_={cursorPosition_};cursorAnchors_={anchor_};verticalScrollBar()->setValue(vimSearchOriginTop_);horizontalScrollBar()->setValue(vimSearchOriginLeft_);vimExHints_->setText(result->error);viewport()->update();return;}
            vimSearchSubject_=result->subject;vimSearchSubjectRevision_=revision;
            vimSearchHighlights_.clear();if(VimState::Session::instance().highlightSearch)for(const auto& match:result->highlights)vimSearchHighlights_.push_back({match.first,match.second});else if(result->navigation.match)vimSearchHighlights_.push_back({result->navigation.match->begin,result->navigation.match->end});
            if(result->navigation.match && (!result->navigation.wrapped || VimState::Session::instance().wrapSearch)){
                VimController::Action action;action.command=forward?VimController::Command::SearchForward:VimController::Command::SearchBackward;
                applyVimSearchMatch(*result->navigation.match,input.offset,action,forward);vimExHints_->setText(I18n::text("Enter: search · Esc: cancel · ↑/↓: history"));
            }else {cursorPosition_=vimSearchOrigin_;anchor_=vimSearchOriginAnchor_;cursors_={cursorPosition_};cursorAnchors_={anchor_};vimExHints_->setText("E486: Pattern not found");viewport()->update();}
        });
        watcher->setFuture(QtConcurrent::run([snapshot,old,options,origin,forward,count,finalNewline,pattern,cancelled,input]{
            auto result=std::make_shared<SearchResult>();const auto check=[&]{if(cancelled->load())throw std::runtime_error("Search cancelled");};
            try{VimSearch::Pattern expression(pattern,options);if(!expression.error().isEmpty())throw std::runtime_error(expression.error().toStdString());
                result->subject=old?old:std::make_shared<VimSearch::Subject>(*snapshot,finalNewline,check);
                result->navigation=VimSearch::navigate(*result->subject,expression,origin,forward,count,check,input.offset.startsWith('e'),characterOffset(input.offset));
                const auto row=snapshot->lineIndexAt(result->navigation.match?result->navigation.match->begin:origin);
                collect(*result,*snapshot,expression,row>3?row-3:0,std::min(snapshot->lineCount()-1,row+40),check);
            }catch(const std::exception& error){result->error=QString::fromUtf8(error.what());}return result;
        }));
    });
}
void EditorWidget::commitVimSearch(const QString& input){
    const auto parsed=query(input,vimCommandPrompt_);if(!parsed.error.isEmpty()){emit highlightingStatusChanged(parsed.error);return;}
    auto& state=VimState::Session::instance();const bool reuse=input.isEmpty();
    const auto pattern=parsed.pattern.isEmpty()?state.searchPattern:parsed.pattern;
    if(pattern.isEmpty()){emit highlightingStatusChanged("E35: No previous regular expression");if(isVimReplaying())vimReplayCancel_=true;return;}
    if(!vimSendingReplay_){state.searchHistory.removeAll(input);if(!input.isEmpty())state.searchHistory.append(input);while(state.searchHistory.size()>200)state.searchHistory.removeFirst();}
    state.searchPattern=pattern;if(!reuse)state.searchOffset=parsed.offset;state.searchForward=vimCommandPrompt_=='/';vimHighlightHidden_=false;state.set('/',VimState::text(pattern));
    navigateVimSearch(pattern,state.searchForward,vimSearchAction_,state.searchOffset);
}
void EditorWidget::navigateVimSearch(const QString& pattern,bool forward,const VimController::Action& action,const QString& offset,bool repeat,std::optional<std::size_t> anchor){
    auto& state=VimState::Session::instance();vimHighlightHidden_=false;
    if(vimSearchCancelled_)vimSearchCancelled_->store(true);
    ++vimSearchGeneration_;
    if(action.operation!=VimController::Command::Consume)beginVimChange();
    const auto original=cursorPosition_;
    const auto selected=vim_.mode()==VimController::Mode::Visual?std::optional{selectionRange()}:std::nullopt;
    const auto searchFrom=anchor.value_or(repeat && vimLastSearchMatch_ && original==vimLastSearchCursor_?vimLastSearchPosition_:original);
    const auto revision=lspRevision_;const auto finalNewline=vimFinalNewline_;const auto options=state.searchOptions;
    const auto old=vimSearchSubjectRevision_==revision?vimSearchSubject_:nullptr;auto result=std::make_shared<SearchResult>();
    runBulkOperation(I18n::text("Searching…"),[result,old,pattern,options,searchFrom,original,forward,action,finalNewline,offset,selected](PieceTreeBuffer& buffer,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep& step){
        VimSearch::Pattern expression(pattern,options);if(!expression.error().isEmpty())throw std::runtime_error(expression.error().toStdString());
        result->subject=old?old:std::make_shared<VimSearch::Subject>(buffer,finalNewline,[&]{step(10);});
        if(action.command==VimController::Command::SelectMatch) {
            std::optional<VimSearch::Match> containing;
            for(auto at=result->subject->begin();at<=searchFrom;) {
                const auto found=expression.find(*result->subject,at,[&]{step(40);});if(!found || found->begin>searchFrom)break;
                if(found->begin<=searchFrom && (searchFrom<found->end || found->begin==found->end))containing=found;
                at=found->end>found->begin?found->end:result->subject->next(found->rawBegin);
            }
            const bool advance=forward && containing && selected && selected->first<=containing->begin && selected->second>=containing->end;
            if(containing && action.count==1 && !advance)result->navigation.match=containing;
            else result->navigation=VimSearch::navigate(*result->subject,expression,searchFrom,forward,action.count-(containing && !advance?1:0),[&]{step(50);});
        }else result->navigation=VimSearch::navigate(*result->subject,expression,searchFrom,forward,action.count,[&]{step(50);},offset.startsWith('e'),characterOffset(offset));
        if(result->navigation.match){const auto row=buffer.lineIndexAt(result->navigation.match->begin);collect(*result,buffer,expression,row>3?row-3:0,std::min(buffer.lineCount()-1,row+40),[&]{step(80);});}
        return HistoryResult{original,{original}};
    },false,{},[this,result,revision,action,offset,forward](bool ok){
        if(!ok){if(isVimReplaying())vimReplayCancel_=true;finishVimChange();return;}
        vimSearchSubject_=result->subject;vimSearchSubjectRevision_=revision;
        if(!result->navigation.match || (result->navigation.wrapped && !VimState::Session::instance().wrapSearch)){emit highlightingStatusChanged("E486: Pattern not found");if(isVimReplaying())vimReplayCancel_=true;finishVimChange();return;}
        vimSearchHighlights_.clear();if(VimState::Session::instance().highlightSearch)for(const auto& match:result->highlights)vimSearchHighlights_.push_back({match.first,match.second});
        applyVimSearchMatch(*result->navigation.match,offset,action,forward);
        if(action.operation==VimController::Command::Consume){vimLastSearchMatch_=result->navigation.match;vimLastSearchCursor_=cursorPosition_;}
        if(result->navigation.wrapped)emit highlightingStatusChanged(I18n::text("Search wrapped"));
    });
}
void EditorWidget::applyVimSearchMatch(const VimSearch::Match& match,const QString& offset,const VimController::Action& action,bool){
    if(action.command==VimController::Command::SelectMatch) {
        if(action.operation!=VimController::Command::Consume){
            if(match.begin==match.end && match.begin>=lineContentEnd(buffer_.lineIndexAt(std::min(match.begin,buffer_.size())))){cursorPosition_=std::min(match.begin,buffer_.size());cursors_={cursorPosition_};clearAllSelections();ensureCursorVisible();viewport()->update();return;}
            editVimRange(match.begin,match.end>match.begin?match.end:graphemeBoundaryAt(match.begin,true),action.operation,false);return;
        }
        if(vim_.mode()!=VimController::Mode::Visual){switchVimMode(VimController::Mode::Visual);anchor_=action.around && match.end>match.begin?graphemeBoundaryAt(match.end,false):match.begin;}
        cursorPosition_=action.around?match.begin:match.end>match.begin?graphemeBoundaryAt(match.end,false):match.begin;
        cursors_={cursorPosition_};cursorAnchors_={anchor_};ensureCursorVisible();viewport()->update();return;
    }
    const auto handling=vimHandlingKey_;vimHandlingKey_=true;
    const auto guard=qScopeGuard([this,handling]{vimHandlingKey_=handling;if(!vimHandlingKey_ && !busy_ && !vim_.acceptsText())finishVimChange();});
    const auto original=cursorPosition_;auto target=match.begin;bool inclusive=false,linewise=false;
    if(!offset.isEmpty()){
        const auto first=offset[0];auto number=offset;
        if(first=='e' || first=='s' || first=='b'){
            number.remove(0,1);target=first=='e' && match.end>match.begin?graphemeBoundaryAt(match.end,false):match.begin;inclusive=first=='e';
            const auto count=number.isEmpty()?0:number.toInt();for(int n=0;n<std::abs(count);++n)target=graphemeBoundaryAt(target,count>0);
        }else{
            linewise=true;const auto delta=number=="+"?1:number=="-"?-1:number.toInt();const auto row=std::clamp<qint64>(qint64(buffer_.lineIndexAt(match.begin))+delta,0,qint64(VimMotions::lastLine(buffer_,vimFinalNewline_)));target=VimMotions::firstText(buffer_,std::size_t(row));
        }
    }
    VimMotions::Range range;range.valid=true;range.target=target;range.begin=std::min(original,target);range.end=std::max(original,target);range.linewise=linewise;
    if(action.operation!=VimController::Command::Consume){
        if(inclusive)range.end=graphemeBoundaryAt(range.end,true);
        const auto first=buffer_.lineIndexAt(range.begin),last=buffer_.lineIndexAt(range.end);
        if(linewise){range.begin=vimLineStart(first);range.end=last+1<buffer_.lineCount()?buffer_.lineStart(last+1):buffer_.size();}
        else if(!inclusive && target>original && last>first && target==vimLineStart(last)){
            if(original<=VimMotions::firstText(buffer_,first)){range.linewise=true;range.begin=vimLineStart(first);range.end=buffer_.lineStart(last);}
            else range.end=lineContentEnd(last-1);
        }
        vimForceNumbered_=true;
    }else if(!vimExActive_)pushVimJump();
    if(!vimExActive_ && action.operation==VimController::Command::Consume)vimLastSearchPosition_=target;
    applyVimAction(action,range);
}
void EditorWidget::updateVimSearchHighlights(){
    if(!vim_.enabled() || vimExActive_ || busy_ || vimSubConfirm_)return;
    const auto& state=VimState::Session::instance();
    if(state.searchPattern.isEmpty() || !state.highlightSearch || vimHighlightHidden_){vimSearchHighlights_.clear();viewport()->update();return;}
    if(vimSearchCancelled_)vimSearchCancelled_->store(true);
    const auto generation=++vimSearchGeneration_;
    QTimer::singleShot(50,this,[this,generation]{
        if(generation!=vimSearchGeneration_ || busy_ || vimExActive_ || vimHighlightHidden_ || !VimState::Session::instance().highlightSearch)return;
        const auto& state=VimState::Session::instance();const auto pattern=state.searchPattern;const auto options=state.searchOptions;
        const auto revision=lspRevision_;const auto old=vimSearchSubjectRevision_==revision?vimSearchSubject_:nullptr;
        const auto first=std::size_t(std::max(0,documentLineForVisualRow(verticalScrollBar()->value()))),last=std::min(buffer_.lineCount()-1,first+std::size_t(viewport()->height()/lineHeight()+2));
        const auto snapshot=std::make_shared<PieceTreeBuffer>(buffer_.snapshot());const bool finalNewline=vimFinalNewline_;
        auto cancelled=std::make_shared<std::atomic<bool>>(false);vimSearchCancelled_=cancelled;
        auto* watcher=new QFutureWatcher<std::shared_ptr<SearchResult>>(this);
        connect(watcher,&QFutureWatcher<std::shared_ptr<SearchResult>>::finished,this,[this,watcher,generation,revision]{const auto result=watcher->result();watcher->deleteLater();if(generation!=vimSearchGeneration_ || revision!=lspRevision_ || !result->error.isEmpty())return;vimSearchSubject_=result->subject;vimSearchSubjectRevision_=revision;vimSearchHighlights_.clear();for(const auto& span:result->highlights)vimSearchHighlights_.push_back({span.first,span.second});viewport()->update();});
        watcher->setFuture(QtConcurrent::run([snapshot,old,pattern,options,first,last,finalNewline,cancelled]{auto result=std::make_shared<SearchResult>();const auto check=[&]{if(cancelled->load())throw std::runtime_error("Search cancelled");};try{VimSearch::Pattern expression(pattern,options);if(!expression.error().isEmpty())return result;result->subject=old?old:std::make_shared<VimSearch::Subject>(*snapshot,finalNewline,check);collect(*result,*snapshot,expression,first,last,check);}catch(const std::exception& error){result->error=QString::fromUtf8(error.what());}return result;}));
    });
}
