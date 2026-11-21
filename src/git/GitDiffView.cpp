#include "GitDiffView.hpp"
#include "app/Codicons.hpp"
#include "app/FontSettings.hpp"
#include "app/I18n.hpp"
#include "app/ThemeManager.hpp"
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextFrame>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
namespace {
QString documentText(const QTextEdit* edit) {
    auto value=edit->document()->toRawText(); value.replace(QChar(0x2029),'\n'); return value;
}
class DiffBlockData final : public QTextBlockUserData {
public:
    bool newline{false};
};
class NumberedEdit final : public QTextEdit {
    class Gutter final : public QWidget {
        NumberedEdit* edit_;
    public:
        explicit Gutter(NumberedEdit* edit) : QWidget(edit),edit_(edit) { setAttribute(Qt::WA_TransparentForMouseEvents); }
        void paintEvent(QPaintEvent*) override {
            const auto& theme=ThemeManager::instance();
            QPainter p(this); p.fillRect(rect(),theme.color("editor.gutterBackground"));
            p.setPen(theme.color("editor.lineNumber")); p.setFont(edit_->font());
            auto block=edit_->cursorForPosition(QPoint(0,0)).block();
            for(;block.isValid();block=block.next()) {
                if(!block.isVisible()) continue;
                const auto r=edit_->cursorRect(QTextCursor(block)); if(r.top()>height()) break;
                p.drawText(QRect(0,r.top(),width()-12,r.height()),Qt::AlignRight|Qt::AlignVCenter,block.userState() < 0 ? QString{} : QString::number(block.userState()+1));
            }
        }
    };
    Gutter* gutter_;
public:
    explicit NumberedEdit(QWidget* parent) : QTextEdit(parent),gutter_(new Gutter(this)) {
        connect(verticalScrollBar(),&QScrollBar::valueChanged,gutter_,[this] { gutter_->update(); });
        connect(this,&QTextEdit::textChanged,gutter_,[this] { gutter_->update(); });
        connect(document()->documentLayout(),&QAbstractTextDocumentLayout::update,gutter_,[this] { gutter_->update(); });
    }
    void setLineCount(int count) {
        const int width=fontMetrics().horizontalAdvance(QString::number(qMax(1,count)))+24;
        setViewportMargins(width,0,0,0);
        updateGutter();
    }
protected:
    void resizeEvent(QResizeEvent* event) override { QTextEdit::resizeEvent(event); updateGutter(); }
    QMimeData* createMimeDataFromSelection() const override {
        auto* mime=new QMimeData;
        const auto cursor=textCursor();
        const int begin=cursor.selectionStart(),end=cursor.selectionEnd();
        QString text;
        for(auto block=document()->findBlock(begin);block.isValid() && block.position()<end;block=block.next()) {
            if(block.userState()<0) continue;
            const auto value=block.text();
            const int start=qMax(0,begin-block.position());
            const int count=qMax(0,qMin(int(value.size()),end-block.position())-start);
            text+=value.mid(start,count);
            auto* data=static_cast<DiffBlockData*>(block.userData());
            if(data && data->newline && end>block.position()+value.size()) text+='\n';
        }
        mime->setText(text); return mime;
    }
private:
    void updateGutter() {
        gutter_->setGeometry(contentsRect().left(),viewport()->y(),viewportMargins().left(),viewport()->height());
        gutter_->update();
    }
};
QTextEdit* pane(QWidget* parent, const char* name) {
    auto* edit=new NumberedEdit(parent); edit->setObjectName(name); edit->setReadOnly(true);
    edit->setAcceptRichText(false); edit->setLineWrapMode(QTextEdit::NoWrap);
    edit->setFrameShape(QFrame::NoFrame);
    edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    edit->setFont(FontSettings::font(FontSettings::Area::Editor));
    edit->document()->setDocumentMargin(4); edit->document()->setUndoRedoEnabled(false); return edit;
}
QVBoxLayout* column(QWidget* widget) {
    auto* layout=new QVBoxLayout(widget);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);
    return layout;
}
QWidget* heading(QWidget* parent,QLabel*& title,GitDiffView* closeTarget=nullptr,
                 const char* closeName="gitDiffClose") {
    auto* header=new QFrame(parent);
    header->setObjectName("gitDiffHeader");
    header->setMinimumHeight(27);
    auto* layout=new QHBoxLayout(header);
    layout->setContentsMargins(12,1,4,1);
    layout->setSpacing(4);
    title=new QLabel(header);
    title->setObjectName("gitDiffTitle");
    title->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    layout->addWidget(title,1);
    if(closeTarget) {
        auto* close=new QToolButton(header);
        close->setObjectName(closeName);
        close->setIcon(Codicons::icon("close"));
        close->setIconSize(QSize(16,16));
        close->setFixedSize(26,24);
        close->setAccessibleName(I18n::text("Close Git diff"));
        close->setToolTip(close->accessibleName());
        layout->addWidget(close);
        QObject::connect(close,&QToolButton::clicked,closeTarget,&QWidget::hide);
    }
    return header;
}
QString limited(const QString& text) {
    int offset=0;
    for(int i=0;i<30000;++i) { const auto end=text.indexOf('\n',offset); if(end<0) return text; offset=end+1; }
    return text.left(offset)+"\n"+I18n::text("Diff preview limited to 30000 lines.");
}
}
class GitDiffView::Overview final : public QWidget {
public:
    explicit Overview(GitDiffView* view,QWidget* parent) : QWidget(parent),view_(view) {
        setObjectName("gitDiffOverview");
        setFixedWidth(7);
        setCursor(Qt::PointingHandCursor);
        setToolTip(I18n::text("Click to scroll to this part of the diff"));
    }
    void setAlignment(const GitDiff::Model& model) {
        rowCount_=model.alignment.right.rowCount();
        marks_.clear();
        for (const auto& hunk : model.hunks) {
            const int paired = std::min(hunk.leftCount, hunk.rightCount);
            if (paired > 0) marks_.append({hunk.row, hunk.row + paired, Modified});
            if (paired < hunk.rows) marks_.append({hunk.row + paired, hunk.row + hunk.rows,
                hunk.leftCount < hunk.rightCount ? Added : Removed});
        }
        update();
    }
    void setPageRows(int rows) {
        if(pageRows_==rows) return;
        pageRows_=rows;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        const auto& theme=ThemeManager::instance();
        const auto background=theme.color("editor.background");
        const QColor added=background.lightness()<128 ? QColor("#73c991") : QColor("#1a7f37");
        const QColor colors[]={added,theme.color("diagnostic.error"),theme.color("accent")};
        QPainter painter(this);
        painter.fillRect(rect(),background);
        painter.fillRect(0,0,1,height(),theme.color("editor.separator"));
        const qreal scale=qreal(height())/qMax(pageRows_,rowCount_);
        for(const auto& mark:marks_) {
            const int top=qBound(0,qRound(mark.begin*scale),qMax(0,height()-2));
            const int bottom=qRound(mark.end*scale);
            painter.fillRect(2,top,width()-2,qMax(2,bottom-top),colors[mark.change]);
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()!=Qt::LeftButton || rowCount_==0) return;
        const int row=qBound(0,int(event->position().y()*qMax(pageRows_,rowCount_)/qMax(1,height())),rowCount_-1);
        view_->scrollToRow(row);
    }
private:
    enum Change { Added, Removed, Modified };
    struct Mark { int begin,end; Change change; };
    GitDiffView* view_;
    QVector<Mark> marks_;
    int rowCount_{0},pageRows_{1};
};
GitDiffView::GitDiffView(QWidget* parent) : QWidget(parent),home_(parent) {
    setObjectName("gitDiffView");
    stack_=new QStackedWidget(this);
    column(this)->addWidget(stack_);
    auto* split=new QSplitter(Qt::Horizontal,stack_);
    split->setObjectName("gitDiffSplitter");
    split->setChildrenCollapsible(false);
    split->setHandleWidth(3);
    comparison_=split;
    stack_->addWidget(split);

    auto* left=new QWidget(split);
    auto* leftLayout=column(left);
    leftLayout->addWidget(heading(left,leftTitle_));
    original_=pane(left,"gitDiffOld");
    leftLayout->addWidget(original_,1);

    auto* right=new QWidget(split);
    auto* rightLayout=column(right);
    rightLayout->addWidget(heading(right,rightTitle_,this));
    auto* body=new QWidget(right);
    auto* bodyLayout=new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0,0,0,0);
    bodyLayout->setSpacing(0);
    auto* views=new QWidget(body);
    rightBody_=column(views);
    current_=pane(views,"gitDiffNew");
    rightBody_->addWidget(current_);
    bodyLayout->addWidget(views,1);
    overview_=new Overview(this,body);
    bodyLayout->addWidget(overview_);
    scroll_=new QScrollBar(Qt::Vertical,body);
    scroll_->setObjectName("gitDiffScroll");
    scroll_->setAccessibleName(I18n::text("Scroll both versions"));
    scroll_->setFocusPolicy(Qt::StrongFocus);
    bodyLayout->addWidget(scroll_);
    rightLayout->addWidget(body,1);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0,1);
    split->setStretchFactor(1,1);

    patch_=new QWidget(stack_);
    auto* patchLayout=column(patch_);
    patchLayout->addWidget(heading(patch_,patchTitle_,this,"gitDiffPatchClose"));
    text_=new QPlainTextEdit(patch_); text_->setObjectName("gitDiffText"); text_->setReadOnly(true);
    text_->setFrameShape(QFrame::NoFrame);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap); text_->setFont(FontSettings::font(FontSettings::Area::Editor));
    patchLayout->addWidget(text_,1);
    stack_->addWidget(patch_);
    connect(scroll_,&QScrollBar::valueChanged,this,[this](int value) {
        scrollSource()->setValue(value);
    });
    connect(original_->verticalScrollBar(),&QScrollBar::valueChanged,this,[this](int value) {
        if(syncing_ || updating_ || !sharedScrolling_) return;
        scrollSource()->setValue(live_.editor ? editorLineForRow(qRound(qreal(value)/lineHeight_)) : value);
        syncScroll();
    });
    connect(original_->verticalScrollBar(),&QScrollBar::rangeChanged,this,[this] { syncScroll(); });
    debounce_=new QTimer(this); debounce_->setSingleShot(true); debounce_->setInterval(100); connect(debounce_,&QTimer::timeout,this,&GitDiffView::rebuild);
    auto* poll=new QTimer(this); poll->setInterval(200); connect(poll,&QTimer::timeout,this,&GitDiffView::pollDocument); poll->start();
    auto* refresh=new QTimer(this); refresh->setInterval(2500); connect(refresh,&QTimer::timeout,this,[this] { if(!legacy_ && isVisible() && !busy_ && isActiveWindow()) emit refreshRequested(); }); refresh->start();
    connect(&ThemeManager::instance(),&ThemeManager::themeChanged,overview_,qOverload<>(&QWidget::update));
    QWidget::hide();
}
void GitDiffView::clearBinding() {
    setSharedScrolling(false);
    if(live_.highlight) live_.highlight(nullptr);
    if(live_.align) live_.align({});
    live_={};
}
void GitDiffView::setVisible(bool visible) {
    if(visible==!isHidden()) {
        QWidget::setVisible(visible);
        return;
    }
    if(visible) {
        QWidget::setVisible(true);
        emit visibilityChanged(true);
        return;
    }
    const bool restoreFocus=isAncestorOf(QApplication::focusWidget());
    const auto editor=live_.editor;
    const auto host=editorHost_;
    QWidget::setVisible(false);
    revertPending_=false;
    ++revision_;
    debounce_->stop();
    clearBinding();
    if(host) {
        editorHost_.clear();
        auto* layout=parentWidget()->layout();
        delete layout->replaceWidget(this,host,Qt::FindDirectChildrenOnly);
        host->show();
        setParent(home_);
    }
    emit visibilityChanged(false);
    if(restoreFocus) {
        if(editor && editor->isVisible()) editor->setFocus();
        else if(host && host->focusWidget()) host->focusWidget()->setFocus();
    }
}
void GitDiffView::display(const QString& path,bool staged,const QString& text) {
    hide();
    legacy_=true; path_=path; staged_=staged;
    patchTitle_->setText(I18n::text("Git diff")+" — "+QFileInfo(path).fileName());
    patchTitle_->setToolTip(path);
    showPatch(text);
    show();
    text_->setFocus();
}
void GitDiffView::displayVersions(const GitDiffSnapshot& snapshot,LiveDocument live,QWidget* editorHost) {
    hide();
    snapshot_=snapshot; live_=std::move(live); legacy_=false; previewOnly_=false;
    path_=snapshot.repository+"/"+snapshot.entry.path; staged_=snapshot.staged;
    current_->setVisible(!live_.editor);
    if(editorHost) {
        // Move the existing editor container intact so its views and buffer history stay attached.
        delete editorHost->parentWidget()->layout()->replaceWidget(editorHost,this,Qt::FindDirectChildrenOnly);
        editorHost_=editorHost;
        rightBody_->addWidget(editorHost);
        editorHost->setVisible(bool(live_.editor));
    }
    updateTitles();
    for(auto* title:{leftTitle_,rightTitle_,patchTitle_}) title->setToolTip(path_);
    patchTitle_->setText(I18n::text("Git diff")+" — "+snapshot.entry.path);
    sourceReady_=live_.available && live_.available();
    documentDirty_=!sourceReady_;
    working_=live_.read && sourceReady_ ? live_.read() : snapshot.right;
    original_->clear();
    current_->clear();
    text_->clear();
    overview_->setAlignment({});
    stack_->setCurrentWidget(comparison_);
    show();
    rebuild();
    if(previewOnly_) text_->setFocus();
    else if(live_.editor && editorHost_) live_.editor->setFocus();
}
void GitDiffView::updateTitles() {
    leftTitle_->setText(snapshot_.entry.conflict() ? I18n::text("Ours") : staged_ ? QStringLiteral("HEAD") : I18n::text("Index"));
    const auto right=staged_ && !live_.editor ? I18n::text("Index") : I18n::text("Working tree");
    // The normal editor header shows the filename when the working file is open.
    rightTitle_->setText(live_.editor ? right : right+" — "+snapshot_.entry.path);
}
void GitDiffView::showPatch(const QString& patch) {
    const bool focusPatch=isAncestorOf(QApplication::focusWidget());
    previewOnly_=true;
    setSharedScrolling(false);
    if(live_.highlight) live_.highlight(nullptr);
    if(live_.align) live_.align({});
    text_->setPlainText(limited(patch));
    stack_->setCurrentWidget(patch_);
    if(focusPatch) text_->setFocus();
}
void GitDiffView::refreshVersions(const GitDiffSnapshot& snapshot) {
    if(legacy_ || snapshot.repository+"/"+snapshot.entry.path!=path_ || snapshot.staged!=staged_) return;
    const bool conflictChanged=snapshot_.entry.conflict()!=snapshot.entry.conflict();
    const bool changed=snapshot_.left!=snapshot.left || snapshot_.special!=snapshot.special ||
        conflictChanged || (previewOnly_ && snapshot_.patch!=snapshot.patch);
    snapshot_=snapshot;
    if(conflictChanged) updateTitles();
    // A temporarily busy live editor still owns the working version. A saved
    // disk snapshot must not replace its pending/unsaved contents.
    bool rightChanged=false;
    if(!live_.read || !live_.editor) { rightChanged=working_!=snapshot.right; working_=snapshot.right; }
    if(changed || rightChanged) rebuild();
}
void GitDiffView::setOperationBusy(bool busy) { busy_=busy; pollDocument(); }
void GitDiffView::documentChanged() {
    documentDirty_=true;
    pollDocument();
    // Saving also emits contentChanged. Reapply only a comparison for this
    // exact revision; edits keep projected colors until the new diff is ready.
    if(!legacy_ && isVisible() && !previewOnly_ && modelRevision_==revision_ &&
       live_.highlight && live_.available && live_.available()) {
        if(live_.align) live_.align(model_.alignment.right);
        live_.highlight(&model_);
    }
}
void GitDiffView::pollDocument() {
    if(legacy_ || !isVisible()) return;
    const bool ready=live_.available && live_.available();
    const bool becameReady=ready && !sourceReady_; sourceReady_=ready;
    tryRevertFile();
    if(live_.read && ready && (documentDirty_ || becameReady)) {
        documentDirty_=false;
        const auto bytes=live_.read();
        if(bytes!=working_) { working_=bytes; ++revision_; debounce_->start(); }
        else if(becameReady && !previewOnly_ && modelRevision_==revision_ && live_.highlight) {
            if(live_.align) live_.align(model_.alignment.right);
            live_.highlight(&model_);
        }
    }
}
void GitDiffView::rebuild() {
    if(legacy_) return;
    const auto revision=++revision_; const auto left=snapshot_.left,right=working_;
    if(snapshot_.special || !GitDiff::textual(left) || !GitDiff::textual(right) || left.count('\n')>30000 || right.count('\n')>30000 || right.size()>8*1024*1024) {
        showPatch(snapshot_.patch);
        return;
    }
    previewOnly_=false;
    auto* watcher=new QFutureWatcher<GitDiff::Model>(this);
    connect(watcher,&QFutureWatcher<GitDiff::Model>::finished,this,[this,watcher,revision] {
        watcher->deleteLater(); if(revision!=revision_ || legacy_) return;
        GitDiff::Model model;
        try { model=watcher->result(); }
        catch(...) { showPatch(snapshot_.patch); return; }
        modelRevision_=revision; render(std::move(model));
    });
    watcher->setFuture(QtConcurrent::run([left,right] { return GitDiff::compare(left,right,true); }));
}
void GitDiffView::render(GitDiff::Model model) {
    updating_=true;
    const bool restoreScroll = sharedScrolling_ && !live_.editor;
    const int oldScroll = current_->verticalScrollBar()->value();
    const auto anchor = model_.alignment.right.anchorAt(oldScroll / lineHeight_);
    const int pixelOffset = oldScroll % lineHeight_;
    model_ = std::move(model);
    const auto fill=[](QTextEdit* edit,const QVector<QByteArray>& lines,const LineAlignment& alignment) {
        const auto visitRows = [&](auto visit) {
            int row = 0;
            for (const auto& range : alignment.ranges()) {
                for (; row < range.row; ++row) visit(row, -1);
                for (int i = 0; i < range.count; ++i, ++row) visit(row, range.line + i);
            }
            for (; row < alignment.rowCount(); ++row) visit(row, -1);
        };
        QString text;
        visitRows([&](int row, int line) {
            if (row != 0) text += '\n';
            if (line < 0 || line >= lines.size()) return;
            auto value = GitDiff::displayText(lines[line]);
            if (value.endsWith('\n')) value.chop(1);
            text += value;
        });
        if (documentText(edit) != text) edit->setPlainText(text);
        auto block=edit->document()->begin();
        visitRows([&](int, int line) {
            if (!block.isValid()) return;
            block.setUserState(line);
            auto* data = static_cast<DiffBlockData*>(block.userData());
            if (!data) { data = new DiffBlockData; block.setUserData(data); }
            data->newline = line >= 0 && line < lines.size() && lines[line].endsWith('\n');
            block=block.next();
        });
    };
    const auto& alignment = model_.alignment;
    fill(original_, model_.left, alignment.left);
    if (!live_.editor) fill(current_, model_.right, alignment.right);
    stack_->setCurrentWidget(comparison_);
    setSharedScrolling(true);
    if(live_.align && isVisible()) live_.align(alignment.right);
    updatePaneMetrics();
    if (restoreScroll)
        current_->verticalScrollBar()->setValue(alignment.right.rowForAnchor(anchor) * lineHeight_ + std::min(pixelOffset, lineHeight_ - 1));
    overview_->setAlignment(model_);
    for(auto* edit:{original_,current_}) {
        if(edit==current_ && live_.editor) continue;
        const bool old=edit==original_; QList<QTextEdit::ExtraSelection> highlights;
        const auto& side = old ? alignment.left : alignment.right;
        const auto pad = [&](int from, int to) {
            if (from == to) return;
            auto block = edit->document()->findBlockByNumber(from);
            for (int row = from; row < to; ++row, block = block.next()) {
                QTextEdit::ExtraSelection h;
                h.cursor = QTextCursor(block);
                h.format.setBackground(QColor(128,128,128,22));
                h.format.setProperty(QTextFormat::FullWidthSelection,true);
                highlights.append(h);
            }
        };
        int row = 0;
        for (const auto& range : side.ranges()) {
            pad(row, range.row);
            row = range.row + range.count;
        }
        pad(row, side.rowCount());
        for (const auto& change : model_.hunks) {
            const int count = old ? change.leftCount : change.rightCount;
            const int paired = std::min(change.leftCount, change.rightCount);
            auto block = edit->document()->findBlockByNumber(change.row);
            for (int offset = 0; offset < count; ++offset, block = block.next()) {
                QTextEdit::ExtraSelection h; h.cursor=QTextCursor(block);
                h.format.setBackground(old?QColor(190,55,65,50):QColor(40,170,90,50)); h.format.setProperty(QTextFormat::FullWidthSelection,true); highlights.append(h);
                if (offset >= paired) continue;
                auto a=GitDiff::displayText(model_.left[change.left + offset]),b=GitDiff::displayText(model_.right[change.right + offset]);
                if(a.endsWith('\n')) a.chop(1);
                if(b.endsWith('\n')) b.chop(1);
                int prefix=0,suffix=0; while(prefix<a.size() && prefix<b.size() && a[prefix]==b[prefix]) ++prefix;
                while(suffix<a.size()-prefix && suffix<b.size()-prefix && a[a.size()-suffix-1]==b[b.size()-suffix-1]) ++suffix;
                const int length=(old?a.size():b.size())-prefix-suffix;
                if(length>0) { h.format.setProperty(QTextFormat::FullWidthSelection,false); h.format.setBackground(old?QColor(220,55,65,110):QColor(40,190,90,110)); h.cursor.setPosition(h.cursor.position()+prefix); h.cursor.movePosition(QTextCursor::NextCharacter,QTextCursor::KeepAnchor,length); highlights.append(h); }
            }
        }
        edit->setExtraSelections(highlights);
    }
    const auto patch=QString::fromUtf8(GitDiff::unified(model_,snapshot_.entry.path));
    if(text_->toPlainText()!=patch) text_->setPlainText(patch);
    if(isVisible() && live_.highlight) live_.highlight(&model_);
    updating_=false;
    syncScroll();
}
void GitDiffView::setSharedScrolling(bool enabled) {
    if(sharedScrolling_==enabled) return;
    sharedScrolling_=enabled;
    if(enabled) {
        if(live_.editor) {
            editorVerticalPolicy_=live_.editor->verticalScrollBarPolicy();
            editorHorizontalPolicy_=live_.editor->horizontalScrollBarPolicy();
            live_.editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            // Reserve the same bottom strip in both panes for independent horizontal scrolling.
            live_.editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
            live_.editor->installEventFilter(this);
        }
        scrollConnection_=connect(scrollSource(),&QScrollBar::valueChanged,this,[this] { syncScroll(); });
        rangeConnection_=connect(scrollSource(),&QScrollBar::rangeChanged,this,[this] { syncScroll(); });
    } else {
        disconnect(scrollConnection_);
        disconnect(rangeConnection_);
        if(live_.editor) {
            live_.editor->removeEventFilter(this);
            live_.editor->setVerticalScrollBarPolicy(editorVerticalPolicy_);
            live_.editor->setHorizontalScrollBarPolicy(editorHorizontalPolicy_);
        }
    }
    if(live_.showOverview) live_.showOverview(enabled);
}
bool GitDiffView::eventFilter(QObject* watched,QEvent* event) {
    if(watched==live_.editor && event->type()==QEvent::FontChange) {
        QTimer::singleShot(0,this,[this] {
            if(!sharedScrolling_) return;
            updatePaneMetrics();
            syncScroll();
        });
    }
    if(watched==live_.editor && event->type()==QEvent::Resize)
        QTimer::singleShot(0,this,[this] { syncScroll(); });
    return QWidget::eventFilter(watched,event);
}
void GitDiffView::updatePaneMetrics() {
    const QScopedValueRollback guard(updating_,true);
    const auto font=live_.editor ? live_.editor->font() : FontSettings::font(FontSettings::Area::Editor);
    const QFontMetrics metrics(font);
    const auto* plain=qobject_cast<QPlainTextEdit*>(live_.editor.data());
    lineHeight_=plain ? metrics.lineSpacing() : metrics.height()+3;
    for(auto* edit:{original_,current_}) {
        if(edit==current_ && live_.editor) continue;
        edit->setFont(font);
        edit->verticalScrollBar()->setSingleStep(lineHeight_);
        static_cast<NumberedEdit*>(edit)->setLineCount(edit==original_ ? model_.left.size() : model_.right.size());
        auto* frame=edit->document()->rootFrame();
        auto format=frame->frameFormat();
        format.setTopMargin(4);
        format.setBottomMargin(4);
        format.setLeftMargin(4);
        if(live_.editor) {
            const int offset=live_.editor->viewport()->mapToGlobal(QPoint{}).y()-edit->viewport()->mapToGlobal(QPoint{}).y();
            const int margin=qMax(0,offset+(plain ? int(plain->document()->documentMargin()) : 0));
            format.setTopMargin(margin);
            // The native editor scrolls whole rows, even when the last row is partly visible.
            format.setBottomMargin(lineHeight_-1);
            format.setLeftMargin(12);
        }
        if(frame->frameFormat()!=format) frame->setFrameFormat(format);
        // Defer layout until all rows have their fixed height.
        QTextCursor blockCursor(edit->document());
        blockCursor.beginEditBlock();
        for(auto block=edit->document()->begin();block.isValid();block=block.next()) {
            auto blockFormat=block.blockFormat();
            if(blockFormat.lineHeight()==lineHeight_ && blockFormat.lineHeightType()==QTextBlockFormat::FixedHeight) continue;
            blockFormat.setLineHeight(lineHeight_,QTextBlockFormat::FixedHeight);
            blockCursor.setPosition(block.position());
            blockCursor.setBlockFormat(blockFormat);
        }
        blockCursor.endEditBlock();
    }
}
QScrollBar* GitDiffView::scrollSource() const {
    return (live_.editor ? live_.editor.data() : current_)->verticalScrollBar();
}
int GitDiffView::editorLineForRow(int row) const {
    if(live_.align) return row;
    return model_.alignment.right.lineAtOrAfter(row);
}
void GitDiffView::scrollToRow(int row) {
    auto* source=scrollSource();
    const int value=live_.editor ? editorLineForRow(row) : row*lineHeight_;
    source->setValue(value-source->pageStep()/2);
}
void GitDiffView::syncScroll() {
    if(syncing_ || updating_ || !sharedScrolling_) return;
    const QScopedValueRollback guard(syncing_,true);
    auto* source=scrollSource();
    const QSignalBlocker block(scroll_);
    scroll_->setRange(source->minimum(),source->maximum());
    scroll_->setPageStep(source->pageStep());
    scroll_->setSingleStep(live_.editor ? source->singleStep() : lineHeight_);
    scroll_->setValue(source->value());
    overview_->setPageRows(qMax(1,live_.editor ? source->pageStep() : source->pageStep()/lineHeight_));
    int y=source->value();
    if(live_.editor) {
        const auto& right = model_.alignment.right;
        const int last = std::max(0, right.rowCount() - 1);
        int row = live_.align ? y : right.rowForLine(y);
        row = row < 0 ? last : std::min(row, last);
        y=row*lineHeight_;
    }
    original_->verticalScrollBar()->setValue(y);
}
void GitDiffView::revertFile() {
    if(legacy_ || staged_ || snapshot_.special || snapshot_.entry.conflict() ||
       !GitDiff::textual(snapshot_.left) || !live_.read || !live_.replace) return;
    revertPending_=true; tryRevertFile();
}
void GitDiffView::tryRevertFile() {
    if(!revertPending_ || busy_ || !live_.available || !live_.available()) return;
    revertPending_=false;
    const auto before=live_.read();
    if(!GitDiff::textual(before)) return;
    if(live_.replace(before,snapshot_.left)) {
        documentDirty_=true; pollDocument();
        emit fileReverted(path_);
    }
}
