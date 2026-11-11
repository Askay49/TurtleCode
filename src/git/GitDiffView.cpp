#include "GitDiffView.hpp"
#include "app/FontSettings.hpp"
#include "app/I18n.hpp"
#include <QAbstractTextDocumentLayout>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHideEvent>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextEdit>
#include <QTimer>
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
            QPainter p(this); p.fillRect(rect(),edit_->palette().alternateBase()); p.setPen(edit_->palette().color(QPalette::PlaceholderText)); p.setFont(edit_->font());
            auto block=edit_->cursorForPosition(QPoint(0,0)).block();
            for(;block.isValid();block=block.next()) {
                if(!block.isVisible()) continue;
                const auto r=edit_->cursorRect(QTextCursor(block)); if(r.top()>height()) break;
                p.drawText(QRect(0,r.top(),width()-5,r.height()),Qt::AlignRight|Qt::AlignVCenter,block.userState() < 0 ? QString{} : QString::number(block.userState()+1));
            }
        }
    };
    Gutter* gutter_;
public:
    explicit NumberedEdit(QWidget* parent) : QTextEdit(parent),gutter_(new Gutter(this)) {
        setViewportMargins(48,0,0,0);
        connect(verticalScrollBar(),&QScrollBar::valueChanged,gutter_,[this] { gutter_->update(); });
        connect(this,&QTextEdit::textChanged,gutter_,[this] { gutter_->update(); });
        connect(document()->documentLayout(),&QAbstractTextDocumentLayout::update,gutter_,[this] { gutter_->update(); });
    }
    void resizeEvent(QResizeEvent* event) override { QTextEdit::resizeEvent(event); gutter_->setGeometry(contentsRect().left(),viewport()->y(),46,viewport()->height()); }
protected:
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

};
QTextEdit* pane(QWidget* parent, const char* name) {
    auto* edit=new NumberedEdit(parent); edit->setObjectName(name); edit->setReadOnly(true);
    edit->setAcceptRichText(false); edit->setLineWrapMode(QTextEdit::NoWrap);
    edit->setFont(FontSettings::font(FontSettings::Area::Editor));
    edit->document()->setDocumentMargin(4); edit->document()->setUndoRedoEnabled(false); return edit;
}
QString limited(const QString& text) {
    int offset=0;
    for(int i=0;i<30000;++i) { const auto end=text.indexOf('\n',offset); if(end<0) return text; offset=end+1; }
    return text.left(offset)+"\n"+I18n::text("Diff preview limited to 30000 lines.");
}
}
GitDiffView::GitDiffView(QWidget* parent) : QDockWidget(I18n::text("Git diff"),parent) {
    setObjectName("gitDiffDock"); setAllowedAreas(Qt::LeftDockWidgetArea|Qt::RightDockWidgetArea); setMinimumWidth(260);
    auto* content=new QWidget(this); auto* layout=new QVBoxLayout(content); layout->setContentsMargins(0,0,0,0);
    stack_=new QStackedWidget(content); layout->addWidget(stack_,1);
    split_=new QSplitter(Qt::Horizontal,content); stack_->addWidget(split_);
    current_=pane(split_,"gitDiffNew"); original_=pane(split_,"gitDiffOld");
    split_->addWidget(original_); split_->addWidget(current_); current_->hide();
    text_=new QPlainTextEdit(content); text_->setObjectName("gitDiffText"); text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap); text_->setFont(FontSettings::font(FontSettings::Area::Editor)); stack_->addWidget(text_);
    connect(original_->verticalScrollBar(),&QScrollBar::valueChanged,this,[this](int value) {
        if(syncing_ || updating_) return;
        syncing_=true;
        if(live_.editor) {
            const int line=original_->cursorForPosition(QPoint(0,0)).blockNumber();
            int target=line;
            if(!live_.align) {
                target=model_.right.size();
                for(int i=line;i<model_.rows.size();++i) if(model_.rows[i].right>=0) { target=model_.rows[i].right; break; }
            }
            live_.editor->verticalScrollBar()->setValue(target);
        } else current_->verticalScrollBar()->setValue(value);
        syncing_=false;
        if(live_.editor) syncFromEditor();
    });
    connect(current_->verticalScrollBar(),&QScrollBar::valueChanged,this,[this](int value) {
        if(!syncing_ && !live_.editor) { syncing_=true; original_->verticalScrollBar()->setValue(value); syncing_=false; }
    });
    debounce_=new QTimer(this); debounce_->setSingleShot(true); debounce_->setInterval(100); connect(debounce_,&QTimer::timeout,this,&GitDiffView::rebuild);
    auto* poll=new QTimer(this); poll->setInterval(200); connect(poll,&QTimer::timeout,this,&GitDiffView::pollDocument); poll->start();
    auto* refresh=new QTimer(this); refresh->setInterval(2500); connect(refresh,&QTimer::timeout,this,[this] { if(!legacy_ && isVisible() && !busy_ && isActiveWindow()) emit refreshRequested(); }); refresh->start();
    setWidget(content);
}
void GitDiffView::clearBinding() {
    disconnect(scrollConnection_);
    if(live_.highlight) live_.highlight(nullptr);
    if(live_.align) live_.align({});
}
void GitDiffView::hideEvent(QHideEvent* event) {
    revertPending_=false;
    if(live_.highlight) live_.highlight(nullptr);
    if(live_.align) live_.align({});
    QDockWidget::hideEvent(event);
}
void GitDiffView::display(const QString& path,bool staged,const QString& text) {
    clearBinding(); revertPending_=false; live_={}; ++revision_; legacy_=true; path_=path; staged_=staged;
    setWindowTitle(I18n::text("Git diff")+" — "+QFileInfo(path).fileName());
    text_->setPlainText(limited(text)); stack_->setCurrentWidget(text_); show(); raise();
}
void GitDiffView::displayVersions(const GitDiffSnapshot& snapshot,LiveDocument live) {
    clearBinding(); snapshot_=snapshot; live_=std::move(live); legacy_=false; path_=snapshot.repository+"/"+snapshot.entry.path; staged_=snapshot.staged;
    revertPending_=false;
    current_->setVisible(!live_.editor);
    sourceReady_=live_.available && live_.available();
    documentDirty_=!sourceReady_;
    working_=(live_.read && live_.available && live_.available())?live_.read():snapshot.right;
    if(live_.editor) {
        scrollConnection_=connect(live_.editor->verticalScrollBar(),&QScrollBar::valueChanged,this,[this] { syncFromEditor(); });
    }
    setWindowTitle(I18n::text("Git diff")+" — "+QFileInfo(path_).fileName());
    show(); raise(); rebuild();
}
void GitDiffView::refreshVersions(const GitDiffSnapshot& snapshot) {
    if(legacy_ || snapshot.repository+"/"+snapshot.entry.path!=path_ || snapshot.staged!=staged_) return;
    const bool changed=snapshot_.left!=snapshot.left || snapshot_.special!=snapshot.special ||
        snapshot_.entry.conflict()!=snapshot.entry.conflict() || (previewOnly_ && snapshot_.patch!=snapshot.patch);
    snapshot_=snapshot;
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
        if(live_.align) live_.align(alignment_);
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
            if(live_.align) live_.align(alignment_);
            live_.highlight(&model_);
        }
    }
}
void GitDiffView::rebuild() {
    if(legacy_) return;
    const auto revision=++revision_; const auto left=snapshot_.left,right=working_;
    if(snapshot_.special || !GitDiff::textual(left) || !GitDiff::textual(right) || left.count('\n')>30000 || right.count('\n')>30000 || right.size()>8*1024*1024) {
        previewOnly_=true; if(live_.highlight) live_.highlight(nullptr); if(live_.align) live_.align({});
        text_->setPlainText(limited(snapshot_.patch)); stack_->setCurrentWidget(text_); return;
    }
    previewOnly_=false;
    auto* watcher=new QFutureWatcher<GitDiff::Model>(this);
    connect(watcher,&QFutureWatcher<GitDiff::Model>::finished,this,[this,watcher,revision] {
        watcher->deleteLater(); if(revision!=revision_ || legacy_) return;
        try { model_=watcher->result(); }
        catch(...) {
            previewOnly_=true; if(live_.highlight) live_.highlight(nullptr); if(live_.align) live_.align({});
            text_->setPlainText(limited(snapshot_.patch)); stack_->setCurrentWidget(text_);
            return;
        }
        modelRevision_=revision; render();
    });
    watcher->setFuture(QtConcurrent::run([left,right] { return GitDiff::compare(left,right,true); }));
}
void GitDiffView::render() {
    updating_=true;
    // Every comparison row occupies one visual row on both sides. Padding
    // belongs only to the view; line numbers continue to address source lines.
    QVector<int> leftRows, rightRows;
    for(const auto& row:model_.rows) { leftRows.append(row.left); rightRows.append(row.right); }
    const bool leftEof=snapshot_.left.isEmpty() || snapshot_.left.endsWith('\n');
    const bool rightEof=working_.isEmpty() || working_.endsWith('\n');
    if(leftEof || rightEof) {
        leftRows.append(leftEof ? int(model_.left.size()) : -1);
        rightRows.append(rightEof ? int(model_.right.size()) : -1);
    }
    const auto fill=[](QTextEdit* edit,const QVector<QByteArray>& lines,const QVector<int>& rows) {
        QStringList text;
        for(int line:rows) {
            auto value=line>=0 && line<lines.size() ? GitDiff::displayText(lines[line]) : QString{};
            if(value.endsWith('\n')) value.chop(1);
            text.append(value);
        }
        const auto value=text.join('\n');
        if(documentText(edit)!=value) {
            const int scroll=edit->verticalScrollBar()->value();
            edit->setPlainText(value); edit->verticalScrollBar()->setValue(scroll);
        }
        auto block=edit->document()->begin();
        for(int line:rows) {
            if(!block.isValid()) break;
            block.setUserState(line);
            auto* data=new DiffBlockData; data->newline=line>=0 && line<lines.size() && lines[line].endsWith('\n');
            block.setUserData(data); block=block.next();
        }
    };
    fill(original_,model_.left,leftRows);
    if(!live_.editor) fill(current_,model_.right,rightRows);
    alignment_=rightRows;
    if(live_.align && isVisible()) live_.align(alignment_);
    if(live_.editor) {
        int offset=live_.editor->viewport()->mapToGlobal(QPoint{}).y()-original_->viewport()->mapToGlobal(QPoint{}).y();
        if(auto* plain=qobject_cast<QPlainTextEdit*>(live_.editor.data())) offset+=int(plain->document()->documentMargin());
        original_->document()->setDocumentMargin(qMax(0,offset));
    }
    current_->setVisible(!live_.editor); stack_->setCurrentWidget(split_);
    for(auto* edit:{original_,current_}) {
        if(edit==current_ && live_.editor) continue;
        const bool old=edit==original_; QList<QTextEdit::ExtraSelection> highlights;
        if(live_.editor) edit->setFont(live_.editor->font());
        const qreal height=live_.editor?(qobject_cast<QPlainTextEdit*>(live_.editor.data())?live_.editor->fontMetrics().lineSpacing():live_.editor->fontMetrics().height()+3):edit->fontMetrics().height()+3;
        for(auto b=edit->document()->begin();b.isValid();b=b.next()) {
            auto format=b.blockFormat();
            if(format.lineHeight()!=height || format.lineHeightType()!=QTextBlockFormat::FixedHeight) {
                QTextCursor cursor(b); format.setLineHeight(height,QTextBlockFormat::FixedHeight); cursor.setBlockFormat(format);
            }
        }
        const auto& rows=old?leftRows:rightRows;
        for(int visual=0;visual<rows.size();++visual) if(rows[visual]<0) {
            QTextEdit::ExtraSelection h; h.cursor=QTextCursor(edit->document()->findBlockByNumber(visual));
            h.format.setBackground(QColor(128,128,128,22)); h.format.setProperty(QTextFormat::FullWidthSelection,true); highlights.append(h);
        }
        for(int visual=0;visual<model_.rows.size();++visual) {
            const auto& row=model_.rows[visual]; if(!row.changed) continue;
            const int index=old?row.left:row.right; if(index<0) continue;
            QTextEdit::ExtraSelection h; h.cursor=QTextCursor(edit->document()->findBlockByNumber(visual));
            h.format.setBackground(old?QColor(190,55,65,50):QColor(40,170,90,50)); h.format.setProperty(QTextFormat::FullWidthSelection,true); highlights.append(h);
            if(row.left>=0 && row.right>=0) {
                auto a=GitDiff::displayText(model_.left[row.left]),b=GitDiff::displayText(model_.right[row.right]);
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
    updating_=false; syncFromEditor();
}
void GitDiffView::syncFromEditor() {
    if(syncing_ || updating_ || !live_.editor || previewOnly_) return;
    syncing_=true; const int line=live_.editor->verticalScrollBar()->value();
    int target=qMax(0,original_->document()->blockCount()-1);
    if(live_.align) target=qMin(line,target);
    else for(int row=0;row<model_.rows.size();++row) if(model_.rows[row].right>=line) { target=row; break; }
    const int y=original_->cursorRect(QTextCursor(original_->document()->findBlockByNumber(target))).top()+original_->verticalScrollBar()->value()-int(original_->document()->documentMargin());
    original_->verticalScrollBar()->setValue(y); syncing_=false;
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
