#include "app/IconDialogs.hpp"
#include "LineOperations.hpp"
#include "language/LanguageRegistry.hpp"
#include "document/FileState.hpp"
#include "app/Codicons.hpp"
#include "app/I18n.hpp"
#include "language/HighlightEdit.hpp"
#include "app/ShortcutManager.hpp"
#include "app/ThemeManager.hpp"
#include "EditorWidget.hpp"
#include "core/text/LineAnchor.hpp"
#include <map>
#include "UnicodeNavigation.hpp"
#include "OccurrenceMatching.hpp"

#include <QApplication>
#include <QCompleter>
#include <QAbstractItemView>
#include <QStandardItemModel>
#include <QClipboard>
#include <QColor>
#include <QFile>
#include <QSaveFile>
#include <QSettings>
#include "core/text/ChunkedFile.hpp"
#include <QMimeData>
#include <QStringDecoder>
#include <QProgressDialog>
#include <QPointer>
#include <atomic>
#include <QFileInfo>
#include <QFontDatabase>
#include "app/FontSettings.hpp"
#include "app/WorkspaceBackground.hpp"
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QEvent>
#include <QContextMenuEvent>
#include <QInputMethodEvent>
#include <QGuiApplication>
#include <QInputMethod>
#include <QVariant>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QStringList>
#include <QFrame>
#include <QLineEdit>
#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QRegularExpression>
#include <QJsonObject>
#include <QHelpEvent>
#include <QToolTip>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cctype>
#include <cstdlib>
namespace {
// Qt's Unicode boundary iterator sees only a bounded context for pathological
// lines. Ordinary lines retain their complete grapheme/word behaviour.
// CR belongs to a CRLF terminator, never to the editable line contents.
std::size_t lineTextEnd(const PieceTreeBuffer& buffer,std::size_t line) {
    auto end=buffer.lineEnd(line);
    if(end>buffer.lineStart(line) && end<buffer.size() && buffer.byteAt(end-1)=='\r') --end;
    return end;
}
struct LineWindow { std::size_t begin;std::string bytes; };
LineWindow lineWindow(const PieceTreeBuffer& buffer,std::size_t line,std::size_t position) {
    const auto start=buffer.lineStart(line),end=lineTextEnd(buffer,line);
    auto begin=start,finish=end;
    if(end-start>64u*1024u) {
        begin=buffer.byteAtCodePoint(buffer.codePointsBefore(position>8192?std::max(start,position-8192):start));
        finish=std::min(end,buffer.byteAtCodePoint(buffer.codePointsBefore(std::min(end,position+8192))));
    }
    return {begin,buffer.slice(begin,finish-begin)};
}
std::size_t lineBoundary(const PieceTreeBuffer& buffer,std::size_t line,std::size_t position,bool forward,bool word) {
    const auto window=lineWindow(buffer,line,position);const auto local=position-window.begin;
    return window.begin+(word?(forward?EditorUnicode::nextWord(window.bytes,local):EditorUnicode::previousWord(window.bytes,local))
        :(forward?EditorUnicode::nextGrapheme(window.bytes,local):EditorUnicode::previousGrapheme(window.bytes,local)));
}
}

#include <limits>
#include <tuple>

namespace {
class StoredClipboard final : public QMimeData {
public:
    std::shared_ptr<ChunkedFile> file;
    explicit StoredClipboard(std::shared_ptr<ChunkedFile> data):file(std::move(data)){}
    QStringList formats() const override {return {"text/plain"};}
    bool hasFormat(const QString& format) const override {return format=="text/plain";}
protected:
    QVariant retrieveData(const QString& format,QMetaType type) const override {
        if(format!="text/plain")return {};
        if(type==QMetaType::fromType<QByteArray>())return QByteArray::fromStdString(file->read(0,file->size()));
        QString result;QStringDecoder decoder(QStringDecoder::Utf8);
        for(std::size_t offset=0;offset<file->size();offset+=64*1024){const auto bytes=file->read(offset,std::min<std::size_t>(64*1024,file->size()-offset));result+=decoder(QByteArrayView(bytes.data(),bytes.size()));}
        return result;
    }
};
struct BulkResult {
    std::map<int,int> lineAnchors;
    std::shared_ptr<PieceTreeBuffer> buffer;
    std::shared_ptr<EditHistory> history;
    std::shared_ptr<ChunkedFile> clipboard;
    HistoryResult cursor;
    QString error;
    bool cancelled{false};
};
std::size_t mapPositionThroughChanges(std::size_t position,
                                      const std::vector<EditChange>& sortedChanges) {
    std::ptrdiff_t delta = 0;
    for (const auto& change : sortedChanges) {
        if (position < change.offset) break;
        const auto end = change.offset + change.removed.size();
        if (position <= end) {
            const auto mapped = static_cast<std::ptrdiff_t>(change.offset) + delta +
                                static_cast<std::ptrdiff_t>(change.inserted.size());
            return static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, mapped));
        }
        delta += static_cast<std::ptrdiff_t>(change.inserted.size()) -
                 static_cast<std::ptrdiff_t>(change.removed.size());
    }
    const auto mapped = static_cast<std::ptrdiff_t>(position) + delta;
    return static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, mapped));
}
}

// The map samples at most one bounded line prefix per screen pixel. It never
// copies a whole document or asks the syntax parser to process offscreen text.
class EditorMiniMap final : public QWidget {
public:
    explicit EditorMiniMap(EditorWidget* editor) : QWidget(editor), editor_(editor) {
        setObjectName("editorMiniMap");
        setAccessibleName(I18n::text("Minimap"));
        setToolTip(I18n::text("Click or drag to scroll"));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        connect(editor, &EditorWidget::contentChanged, this, [this] { invalidate(); });
        connect(editor, &EditorWidget::semanticViewportChanged, this, [this] { update(); });
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { invalidate(); });
    }
    void invalidate() { dirty_=true; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        if (width()<=0 || height()<=0) return;
        const int count=rowCount();
        const qreal scale=std::min(3.0,qreal(height())/count);
        if (dirty_ || cache_.size()!=size()) {
            cache_=QImage(size(),QImage::Format_ARGB32_Premultiplied);
            auto& theme=ThemeManager::instance();
            cache_.fill(theme.color("editor.background"));
            QPainter map(&cache_);
            auto ink=theme.color("text.primary"); ink.setAlpha(150);
            map.setPen(ink);
            const int samples=std::min(count,height());
            for (int i=0;i<samples;++i) {
                const int row=count>height()?int(qint64(i)*count/height()):i;
                const int line=editor_->diffRows_.isEmpty()?row:editor_->diffRows_[row];
                if (line<0) continue;
                const auto& buffer=editor_->buffer_;
                const auto start=buffer.lineStart(line);
                const auto bytes=buffer.slice(start,std::min<std::size_t>(256,buffer.lineEnd(line)-start));
                const auto text=QString::fromUtf8(bytes.data(),qsizetype(bytes.size()));
                int column=0;
                for (QChar ch:text) {
                    if (ch==QLatin1Char('\t')) { column+=4-column%4; continue; }
                    if (column>=width()-8) break;
                    if (!ch.isSpace()) map.drawLine(4+column,int(row*scale),4+column,int(row*scale)+1);
                    ++column;
                }
            }
            dirty_=false;
        }
        QPainter painter(this);
        painter.drawImage(0,0,cache_);
        auto color=ThemeManager::instance().color("accent");
        const qreal top=editor_->verticalScrollBar()->value()*scale;
        const qreal bottom=std::min(qreal(height()),top+editor_->verticalScrollBar()->pageStep()*scale);
        const QRectF visible(1,top,width()-2,std::max(2.0,bottom-top));
        color.setAlpha(35); painter.fillRect(visible,color);
        color.setAlpha(130); painter.setPen(color); painter.drawRect(visible.adjusted(0,0,-1,-1));
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button()!=Qt::LeftButton) { event->ignore(); return; }
        const qreal scale=std::min(3.0,qreal(std::max(1,height()))/rowCount());
        const qreal top=editor_->verticalScrollBar()->value()*scale;
        const qreal extent=editor_->verticalScrollBar()->pageStep()*scale;
        dragOffset_=event->position().y()>=top && event->position().y()<top+extent
            ?event->position().y()-top:extent/2;
        scrollTo(event->position().y());
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons()&Qt::LeftButton) scrollTo(event->position().y());
    }
    void wheelEvent(QWheelEvent* event) override {
        QApplication::sendEvent(editor_->viewport(),event);
    }
private:
    int rowCount() const {
        return std::max(1,editor_->diffRows_.isEmpty()
            ?int(std::min<std::size_t>(editor_->buffer_.lineCount(),INT_MAX-1)):int(editor_->diffRows_.size()));
    }
    void scrollTo(qreal y) {
        const qreal scale=std::min(3.0,qreal(std::max(1,height()))/rowCount());
        editor_->verticalScrollBar()->setValue(int(std::clamp((y-dragOffset_)/scale,0.0,qreal(INT_MAX-1))));
    }
    EditorWidget* editor_;
    QImage cache_;
    bool dirty_{true};
    qreal dragOffset_{0};
};

void EditorWidget::layoutMiniMap() {
    if (!miniMap_) return;
    const int mapWidth=miniMapEnabled_?std::min(112,std::max(0,width()/4)):0;
    if (viewportMargins().right()!=mapWidth) setViewportMargins(0,0,mapWidth,0);
    const auto area=viewport()->geometry();
    miniMap_->setGeometry(area.right()+1,area.top(),mapWidth,area.height());
    miniMap_->setVisible(miniMapEnabled_);
}

void EditorWidget::setMiniMapEnabled(bool enabled) {
    const int top=verticalScrollBar()->value(),left=horizontalScrollBar()->value();
    miniMapEnabled_=enabled;
    layoutMiniMap();
    verticalScrollBar()->setValue(top);
    horizontalScrollBar()->setValue(left);
    if (enabled) miniMap_->invalidate();
}

EditorWidget::EditorWidget(QWidget* parent, DocumentPtr document)
    : QAbstractScrollArea(parent), document_(std::move(document)),
      buffer_(document_->buffer()), filePath_(document_->filePath()), history_(document_->history()) {
    connect(this,&EditorWidget::contentChanged,this,[this] {
        // Ordinary edits project the alignment in invalidateSemanticLines().
        // Only bulk replacements without edit details need to discard the map.
        if (!diffRows_.isEmpty() && std::size_t(diffLineRows_.size()) != buffer_.lineCount()) {
            diffHighlights_.clear();
            setDiffAlignment({});
        }
        viewport()->update();
    });
    miniMap_=new EditorMiniMap(this);
    miniMap_->hide();
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    viewport()->installEventFilter(this);
    setFrameShape(QFrame::NoFrame);
    setFont(FontSettings::font(FontSettings::Area::Editor));
    viewport()->setCursor(Qt::IBeamCursor);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    bracketTimer_=new QTimer(this);bracketTimer_->setSingleShot(true);
    connect(bracketTimer_,&QTimer::timeout,this,[this] {
        if(busy_ || !QSettings().value("editor/highlightBrackets",true).toBool())return;
        const auto rules=editingRules();
        if(!codeContext_.at(buffer_,cursorPosition_,rules)){bracketTimer_->start(10);return;}
        bracketMatch_=CodeContext::match(buffer_,cursorPosition_,codeContext_,rules);
        if(bracketMatch_.pending)bracketTimer_->start(10);
        viewport()->update();
    });
    editClock_.start();
    applyHistorySettings();
    highlighter_.setDocument(document_);
    connect(&highlighter_, &SyntaxHighlighter::resourceLimitReached, this, [this](HighlightLimit reason) {
        const auto message=reason==HighlightLimit::SourceMemory
            ? I18n::text("Syntax highlighting paused: text snapshot memory budget reached.")
            : reason==HighlightLimit::ParseTime
            ? I18n::text("Syntax highlighting paused: parsing time limit reached.")
            : reason==HighlightLimit::QueryTime
                ? I18n::text("Highlighting simplified for a complex area: time limit reached.")
                : I18n::text("Highlighting simplified for a dense area: memory limit reached.");
        emit highlightingStatusChanged(message);
    });
    connect(&highlighter_, &SyntaxHighlighter::highlightingReady, this,
            [this] { viewport()->update(); });
    auto* addCursorAbove = new QShortcut(this);
    ShortcutManager::instance().bind("editor.cursorAbove", addCursorAbove);
    addCursorAbove->setContext(Qt::WidgetWithChildrenShortcut);
    connect(addCursorAbove, &QShortcut::activated, this, [this] { if (!busy_) addCursorVertically(-1); });
    auto* addCursorBelow = new QShortcut(this);
    ShortcutManager::instance().bind("editor.cursorBelow", addCursorBelow);
    addCursorBelow->setContext(Qt::WidgetWithChildrenShortcut);
    connect(addCursorBelow, &QShortcut::activated, this, [this] { if (!busy_) addCursorVertically(1); });
    auto* nextOccurrence = new QShortcut(this);
    nextOccurrence->setContext(Qt::WidgetShortcut);
    ShortcutManager::instance().bind("editor.nextOccurrence", nextOccurrence);
    connect(nextOccurrence, &QShortcut::activated, this, &EditorWidget::selectNextOccurrence);
    for(const auto& command:std::vector<std::pair<QString,std::pair<int,bool>>>{
        {"editor.copyLineUp",{-1,true}},{"editor.copyLineDown",{1,true}},
        {"editor.moveLineUp",{-1,false}},{"editor.moveLineDown",{1,false}}}) {
        auto* shortcut=new QShortcut(this);shortcut->setContext(Qt::WidgetShortcut);
        ShortcutManager::instance().bind(command.first,shortcut);
        connect(shortcut,&QShortcut::activated,this,[this,operation=command.second]{editLines(operation.first,operation.second);});
    }
    findBar_ = new QFrame(viewport());
    findBar_->setObjectName(QStringLiteral("fileFindBar"));
    auto* barLayout = new QVBoxLayout(findBar_);
    barLayout->setContentsMargins(6, 5, 6, 5);
    barLayout->setSpacing(3);
    auto* findRow = new QHBoxLayout;
    findRow->setSpacing(3);
    findReplaceToggle_ = new QToolButton(findBar_);
    findReplaceToggle_->setObjectName(QStringLiteral("fileReplaceToggle"));
    findReplaceToggle_->setToolTip(tr("Toggle Replace"));
    findReplaceToggle_->setCheckable(true);
    findReplaceToggle_->setIcon(Codicons::icon("chevron-right"));
    findReplaceToggle_->setFixedWidth(22);
    findReplaceToggle_->setFocusPolicy(Qt::NoFocus);
    findInput_ = new QLineEdit(findBar_);
    findInput_->setObjectName(QStringLiteral("fileFindInput"));
    findInput_->setPlaceholderText(tr("Find"));
    findCount_ = new QLabel(findBar_);
    findCount_->setObjectName(QStringLiteral("fileFindCount"));
    findCount_->setMinimumWidth(55);
    findCase_ = new QToolButton(findBar_);
    findCase_->setObjectName("fileFindCase");
    findCase_->setIcon(Codicons::icon("case-sensitive"));
    findCase_->setToolTip(tr("Match Case"));
    findWord_ = new QToolButton(findBar_);
    findWord_->setObjectName("fileFindWholeWord");
    findWord_->setIcon(Codicons::icon("whole-word"));
    findWord_->setToolTip(tr("Match Whole Word"));
    findRegex_ = new QToolButton(findBar_);
    findRegex_->setObjectName("fileFindRegex");
    findRegex_->setIcon(Codicons::icon("regex"));
    findRegex_->setToolTip(tr("Use Regular Expression"));
    for (auto* button : {findCase_, findWord_, findRegex_}) button->setCheckable(true);
    auto* previous = new QToolButton(findBar_);
    previous->setIcon(Codicons::icon("arrow-up"));
    previous->setToolTip(tr("Previous Match (Shift+Enter)"));
    auto* next = new QToolButton(findBar_);
    next->setIcon(Codicons::icon("arrow-down"));
    next->setToolTip(tr("Next Match (Enter)"));
    auto* close = new QToolButton(findBar_);
    close->setIcon(Codicons::icon("close"));
    close->setToolTip(tr("Close (Escape)"));
    for (auto* button : {findCase_, findWord_, findRegex_, previous, next, close})
        button->setAccessibleName(button->toolTip());
    findRow->addWidget(findReplaceToggle_);
    findRow->addWidget(findInput_, 1);
    for (QWidget* widget : std::initializer_list<QWidget*>{findCase_, findWord_, findRegex_, findCount_, previous, next, close})
        findRow->addWidget(widget);
    barLayout->addLayout(findRow);
    findReplaceRow_ = new QWidget(findBar_);
    auto* replaceRow = new QHBoxLayout(findReplaceRow_);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->addSpacing(25);
    findReplaceInput_ = new QLineEdit(findReplaceRow_);
    findReplaceInput_->setObjectName(QStringLiteral("fileReplaceInput"));
    findReplaceInput_->setPlaceholderText(tr("Replace"));
    auto* replaceOne = new QToolButton(findReplaceRow_);
    replaceOne->setText(tr("Replace"));
    findReplaceAll_ = new QToolButton(findReplaceRow_);
    findReplaceAll_->setText(tr("Replace All"));
    replaceRow->addWidget(findReplaceInput_, 1);
    replaceRow->addWidget(replaceOne);
    replaceRow->addWidget(findReplaceAll_);
    barLayout->addWidget(findReplaceRow_);
    findReplaceRow_->hide();
    findBar_->hide();
    findTimer_ = new QTimer(this);
    findTimer_->setSingleShot(true);
    findTimer_->setInterval(150);
    connect(findTimer_, &QTimer::timeout, this, &EditorWidget::updateFindMatches);
    const auto queueFindUpdate = [this] {
        ++findGeneration_;
        findMatches_.clear();
        activeFindMatch_ = -1;
        findReplaceAll_->setEnabled(false);
        findTimer_->start();
        viewport()->update();
    };
    connect(findInput_, &QLineEdit::textChanged, this, queueFindUpdate);
    connect(findReplaceToggle_, &QToolButton::toggled, this, [this](bool expanded) {
        findReplaceToggle_->setIcon(Codicons::icon(expanded ? "chevron-down" : "chevron-right"));
        findReplaceRow_->setVisible(expanded);
        positionFindBar();
    });
    for (auto* button : {findCase_, findWord_, findRegex_})
        connect(button, &QToolButton::toggled, this, queueFindUpdate);
    connect(previous, &QToolButton::clicked, this, [this] { navigateFind(-1); });
    connect(next, &QToolButton::clicked, this, [this] { navigateFind(1); });
    connect(replaceOne, &QToolButton::clicked, this, [this] { replaceFindMatch(false); });
    connect(findReplaceAll_, &QToolButton::clicked, this, [this] { replaceFindMatch(true); });
    connect(close, &QToolButton::clicked, this, [this] {
        if(findCancelled_)findCancelled_->store(true);
        ++findGeneration_; findTimer_->stop(); findBar_->hide(); findMatches_.clear(); clearSelection(); setFocus(); viewport()->update();
    });
    findInput_->installEventFilter(this);
    findReplaceInput_->installEventFilter(this);
    updateScrollBars();
    setMiniMapEnabled(QSettings().value("editor/minimap",true).toBool());
}

void EditorWidget::loadBuffer(PieceTreeBuffer buffer, const QString& path) {
    diffHighlights_.clear();
    setDiffAlignment({});
    clearSemanticHighlighting();
    lspPolicy_.reset();
    buffer_ = std::move(buffer);
    invalidateLineCache();
    horizontalExtentDirty_ = true;
    filePath_ = path;
    document_->diskState = diskFileState(path);
    highlighter_.setLanguageForPath(path);
    highlighter_.clear();
    highlighter_.setDocumentText(buffer_);
    cursorPosition_ = 0;
    cursors_ = {0};
    cursorAnchors_ = {std::nullopt};
    clearSelection();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    history_.clear();
    history_.markSaved();
    setModified(false);
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
}

void EditorWidget::restoreBuffer(PieceTreeBuffer buffer, const QString& path) {
    loadBuffer(std::move(buffer), path);
    history_.markUnsaved();
    setModified(true);
}

void EditorWidget::reloadBuffer(PieceTreeBuffer buffer, const QString& path) {
    const auto oldLine = currentLine();
    const auto oldColumn = cursorPosition_ - buffer_.lineStart(oldLine);
    const int oldTopLine = verticalScrollBar()->value();
    const int oldHorizontal = horizontalScrollBar()->value();
    loadBuffer(std::move(buffer), path);
    const auto line = std::min(oldLine, buffer_.lineCount() - 1);
    const auto start=buffer_.lineStart(line),end=buffer_.lineEnd(line);
    const auto target=std::min(end,buffer_.byteAtCodePoint(buffer_.codePointsBefore(start+std::min(oldColumn,end-start))));
    const auto column=target-start;
    moveCursor(buffer_.lineStart(line) + column, false);
    verticalScrollBar()->setValue(std::min(oldTopLine, verticalScrollBar()->maximum()));
    horizontalScrollBar()->setValue(std::min(oldHorizontal, horizontalScrollBar()->maximum()));
    viewport()->update();
}

void EditorWidget::completeAsyncSave(const QString& path) {
    const bool languageChanged = QFileInfo(filePath_).suffix().compare(
        QFileInfo(path).suffix(), Qt::CaseInsensitive) != 0;
    filePath_ = path;
    document_->diskState = diskFileState(path);
    if (languageChanged) {
        highlighter_.setLanguageForPath(path);
        highlighter_.clear();
        highlighter_.setDocumentText(buffer_);
    }
    history_.markSaved();
    setModified(history_.isModified());
}

bool EditorWidget::findText(const QString& query, bool forward, Qt::CaseSensitivity sensitivity) {
    if (query.isEmpty() || buffer_.lineCount() == 0) return false;
    const std::size_t startLine = currentLine();
    const std::size_t lineCount = buffer_.lineCount();
    for (std::size_t step = 0; step < lineCount; ++step) {
        const std::size_t lineNo = forward
            ? (startLine + step) % lineCount
            : (startLine + lineCount - step) % lineCount;
        const QString& line = cachedLine(lineNo);
        int from = 0;
        if (lineNo == startLine && step == 0) {
            const auto start = buffer_.lineStart(lineNo);
            const auto local = cursorPosition_ >= start ? cursorPosition_ - start : 0;
            const auto prefixLength = QString::fromUtf8(buffer_.slice(start, local).data(),
                                                         static_cast<qsizetype>(local)).size();
            from = forward ? prefixLength : prefixLength - 1;
        }
        const int index = forward
            ? line.indexOf(query, from, sensitivity)
            : line.lastIndexOf(query, from < 0 ? line.size() : from, sensitivity);
        if (index < 0) continue;
        const auto lineStart = buffer_.lineStart(lineNo);
        const auto begin = lineStart + static_cast<std::size_t>(line.left(index).toUtf8().size());
        const auto end = begin + static_cast<std::size_t>(query.toUtf8().size());
        anchor_ = begin;
        cursorPosition_ = end;
        cursors_ = {cursorPosition_};
        cursorAnchors_ = {anchor_};
        history_.breakCoalescing();
        ensureCursorVisible();
        viewport()->update();
        return true;
    }
    return false;
}

int EditorWidget::replaceAllText(const QString& query, const QString& replacement,
                                 Qt::CaseSensitivity sensitivity) {
    if (busy_ || query.isEmpty()) return 0;
    struct Match { std::size_t begin; std::size_t length; };
    std::vector<Match> matches;
    for (std::size_t lineNo = 0; lineNo < buffer_.lineCount(); ++lineNo) {
        const QString& line = cachedLine(lineNo);
        int offset = 0;
        while ((offset = line.indexOf(query, offset, sensitivity)) >= 0) {
            const auto lineStart = buffer_.lineStart(lineNo);
            const auto begin = lineStart + static_cast<std::size_t>(line.left(offset).toUtf8().size());
            matches.push_back({begin, static_cast<std::size_t>(query.toUtf8().size())});
            offset += std::max<qsizetype>(1, query.size());
        }
    }
    if (matches.empty()) return 0;
    history_.breakCoalescing();
    for (auto it = matches.rbegin(); it != matches.rend(); ++it)
        replaceRange(it->begin, it->length, replacement.toUtf8().toStdString());
    return static_cast<int>(matches.size());
}

void EditorWidget::showFindBar(bool replace) {
    if (!findBar_) return;
    if (!findBar_->isVisible() || replace) findReplaceToggle_->setChecked(replace);
    findBar_->show();
    findReplaceRow_->setVisible(findReplaceToggle_->isChecked());
    positionFindBar();
    if (hasSelection()) {
        const auto [begin, end] = selectionRange();
        if (buffer_.lineIndexAt(begin) == buffer_.lineIndexAt(end) && end - begin < 4096) {
            const auto bytes = buffer_.slice(begin, end - begin);
            findInput_->setText(QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size())));
        }
    }
    findInput_->setFocus();
    findInput_->selectAll();
    updateFindMatches();
}

void EditorWidget::positionFindBar() {
    if (!findBar_) return;
    const int width = std::min(590, std::max(300, viewport()->width() - 16));
    findBar_->setGeometry(std::max(0, viewport()->width() - width - 12), 8,
                          width, findReplaceToggle_->isChecked() ? 78 : 44);
    findBar_->raise();
}

void EditorWidget::updateFindMatches() {
    if(findCancelled_)findCancelled_->store(true);
    findCancelled_=std::make_shared<std::atomic<bool>>(false);const auto cancelled=findCancelled_;
    const auto generation = ++findGeneration_;
    findMatches_.clear();
    activeFindMatch_ = -1;
    findReplaceAll_->setEnabled(false);
    const QString query = findInput_->text();
    if (query.isEmpty()) {
        findCount_->clear(); viewport()->update(); return;
    }
    QString pattern = findRegex_->isChecked() ? query : QRegularExpression::escape(query);
    if (findWord_->isChecked())
        pattern = QStringLiteral("(?<![\\p{L}\\p{N}_])(?:%1)(?![\\p{L}\\p{N}_])").arg(pattern);
    QRegularExpression expression(pattern, findCase_->isChecked()
        ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    if (!expression.isValid()) {
        findCount_->setText(tr("Invalid regex")); viewport()->update(); return;
    }
    const bool regex=findRegex_->isChecked();
    findCount_->setToolTip(regex?I18n::text("Regex search skips lines larger than 256 KiB. Use literal search for those lines."):QString());
    const auto scan = [expression,regex,query,cancelled](const PieceTreeBuffer& buffer) {
        std::vector<FindMatch> matches;
        constexpr std::size_t limit = 10000;
        if(!regex) {
            std::string pending;std::size_t base=0,processedThrough=0;
            const auto scanWindow=[&](bool final) {
                if(cancelled->load())return false;
                std::size_t valid=pending.size();
                if(!final && valid) {
                    auto last=valid-1;while(last && (static_cast<unsigned char>(pending[last])&0xc0)==0x80)--last;
                    const auto c=static_cast<unsigned char>(pending[last]);const std::size_t count=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
                    if(valid-last<count)valid=last;
                }
                const auto text=QString::fromUtf8(pending.data(),valid);
                auto safe=final?text.size():std::max<qsizetype>(0,text.size()-query.size()-2);
                if(safe>0 && safe<text.size() && text[safe].isLowSurrogate())--safe;
                auto found=expression.globalMatch(text);
                while(found.hasNext() && matches.size()<limit) {
                    const auto match=found.next();if(match.capturedStart()>=safe)break;if(match.capturedLength()==0)continue;
                    const auto begin=base+static_cast<std::size_t>(text.left(match.capturedStart()).toUtf8().size());
                    const auto end=base+static_cast<std::size_t>(text.left(match.capturedEnd()).toUtf8().size());
                    if(begin>=processedThrough && (matches.empty() || begin>=matches.back().end))matches.push_back({begin,end});
                }
                processedThrough=base+static_cast<std::size_t>(text.left(safe).toUtf8().size());
                auto retain=std::max<qsizetype>(0,safe-2);if(retain>0 && text[retain].isLowSurrogate())--retain;
                const auto consumed=static_cast<std::size_t>(text.left(retain).toUtf8().size());pending.erase(0,consumed);base+=consumed;
                return matches.size()<limit && !cancelled->load();
            };
            buffer.forEachChunk([&](std::string_view bytes){if(cancelled->load())return false;pending.append(bytes);return scanWindow(false);});
            if(matches.size()<limit && !cancelled->load())scanWindow(true);
            return matches;
        }
        buffer.forEachLineBounded(256*1024,[&](std::size_t lineStart, std::string_view bytes,bool truncated) {
            if(cancelled->load())return false;
            if(truncated)return true;
            const QString line = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
            auto iterator = expression.globalMatch(line);
            qsizetype lastColumn = 0;
            std::size_t lastByte = 0;
            const auto byteAtColumn = [&](qsizetype column) {
                lastByte += static_cast<std::size_t>(line.mid(lastColumn, column - lastColumn).toUtf8().size());
                lastColumn = column;
                return lastByte;
            };
            while (iterator.hasNext() && matches.size() < limit) {
                const auto match = iterator.next();
                if (match.capturedLength() == 0) continue;
                const auto begin = lineStart + byteAtColumn(match.capturedStart());
                const auto end = lineStart + byteAtColumn(match.capturedEnd());
                matches.push_back({begin, end});
            }
            return matches.size() < limit;
        });
        return matches;
    };
    const auto display = [this](std::vector<FindMatch> matches) {
        findMatches_ = std::move(matches);
        findReplaceAll_->setEnabled(!findMatches_.empty() && findMatches_.size() < 10000);
        findReplaceAll_->setToolTip(findMatches_.size() == 10000
            ? tr("Too many matches to replace safely; narrow the search") : QString());
        const auto [selectionBegin, selectionEnd] = selectionRange();
        for (std::size_t i = 0; i < findMatches_.size(); ++i) {
            if (findMatches_[i].begin == selectionBegin && findMatches_[i].end == selectionEnd && hasSelection()) {
                activeFindMatch_ = static_cast<int>(i); break;
            }
        }
        findCount_->setText(QStringLiteral("%1/%2%3").arg(activeFindMatch_ + 1)
            .arg(findMatches_.size()).arg(findMatches_.size() == 10000 ? QStringLiteral("+") : QString()));
        viewport()->update();
    };
    if (buffer_.size() < 2 * 1024 * 1024) {
        display(scan(buffer_));
    } else {
        findCount_->setText(tr("Searching…"));
        auto* watcher = new QFutureWatcher<std::vector<FindMatch>>(this);
        connect(watcher, &QFutureWatcher<std::vector<FindMatch>>::finished, this,
                [this, watcher, generation, display] {
            auto matches = watcher->result();
            watcher->deleteLater();
            if (generation == findGeneration_ && findBar_->isVisible()) display(std::move(matches));
        });
        const auto doc=document_;
        watcher->setFuture(QtConcurrent::run([doc,scan,cancelled] {
            if(cancelled->load())return std::vector<FindMatch>{};
            auto snapshot=doc->buffer().snapshot();return scan(snapshot);
        }));
    }
}

void EditorWidget::navigateFind(int direction) {
    if (findTimer_->isActive()) { findTimer_->stop(); updateFindMatches(); }
    if (findMatches_.empty()) return;
    int index = activeFindMatch_;
    if (index < 0) {
        if (direction > 0) {
            index = 0;
            while (index + 1 < static_cast<int>(findMatches_.size()) &&
                   findMatches_[index].begin < cursorPosition_) ++index;
        } else {
            index = static_cast<int>(findMatches_.size()) - 1;
            while (index > 0 && findMatches_[index].end > cursorPosition_) --index;
        }
    } else {
        index = (index + direction + static_cast<int>(findMatches_.size())) % static_cast<int>(findMatches_.size());
    }
    activeFindMatch_ = index;
    const auto match = findMatches_[index];
    cursors_ = {match.end}; cursorAnchors_ = {match.begin}; syncPrimaryCursor();
    history_.breakCoalescing();
    updateScrollBars(); ensureCursorVisible(true);
    findCount_->setText(QStringLiteral("%1/%2").arg(index + 1).arg(findMatches_.size()));
    viewport()->update();
}

void EditorWidget::findNext(bool backward) {
    if (!findBar_->isVisible()) showFindBar();
    navigateFind(backward ? -1 : 1);
}

void EditorWidget::replaceFindMatch(bool all) {
    if (busy_) return;
    if (findTimer_->isActive()) { findTimer_->stop(); updateFindMatches(); }
    if (findMatches_.empty()) return;
    if (all && findMatches_.size() >= 10000) return;
    if (!all && activeFindMatch_ < 0) navigateFind(1);
    const auto before = cursors_;
    const QString replacement = findReplaceInput_->text();
    if(all && (buffer_.size()>=1024*1024 || findMatches_.size()>256)){replaceAllInBackground(replacement);return;}
    struct Replacement { std::size_t begin; std::size_t end; std::string inserted; };
    std::vector<Replacement> replacements;
    replacements.reserve(all ? findMatches_.size() : 1);
    if (!findRegex_->isChecked()) {
        const auto bytes = replacement.toUtf8().toStdString();
        if (all) {
            for (const auto match : findMatches_)
                replacements.push_back({match.begin, match.end, bytes});
        } else {
            const auto match = findMatches_[activeFindMatch_];
            replacements.push_back({match.begin, match.end, bytes});
        }
    } else {
        QString pattern = findInput_->text();
        if (findWord_->isChecked())
            pattern = QStringLiteral("(?<![\\p{L}\\p{N}_])(?:%1)(?![\\p{L}\\p{N}_])").arg(pattern);
        const QRegularExpression expression(pattern, findCase_->isChecked()
            ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        const auto addRegexReplacement = [&](FindMatch match, const QString& line, qsizetype column) {
            QString expanded;
            const auto found = expression.match(line, column);
            if (found.hasMatch() && found.capturedStart() == column) {
                for (qsizetype i = 0; i < replacement.size(); ++i) {
                    if (replacement[i] == '$' && i + 1 < replacement.size()) {
                        const QChar next = replacement[i + 1];
                        if (next == '$') { expanded += '$'; ++i; continue; }
                        if (next.isDigit() && next.digitValue() <= found.lastCapturedIndex()) {
                            expanded += found.captured(next.digitValue()); ++i; continue;
                        }
                    }
                    expanded += replacement[i];
                }
            } else {
                expanded = replacement;
            }
            replacements.push_back({match.begin, match.end, expanded.toUtf8().toStdString()});
        };
        if (all) {
            std::size_t matchIndex = 0;
            buffer_.forEachLine([&](std::size_t lineStart, std::string_view bytes) {
                const auto lineEnd = lineStart + bytes.size();
                if (matchIndex >= findMatches_.size() || findMatches_[matchIndex].begin >= lineEnd)
                    return matchIndex < findMatches_.size();
                const QString line = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
                std::size_t previousByte = 0;
                qsizetype column = 0;
                while (matchIndex < findMatches_.size() && findMatches_[matchIndex].begin < lineEnd) {
                    const auto match = findMatches_[matchIndex++];
                    const auto byteColumn = match.begin - lineStart;
                    column += QString::fromUtf8(bytes.data() + previousByte,
                        static_cast<qsizetype>(byteColumn - previousByte)).size();
                    previousByte = byteColumn;
                    addRegexReplacement(match, line, column);
                }
                return matchIndex < findMatches_.size();
            });
        } else {
            const auto match = findMatches_[activeFindMatch_];
            const auto lineNo = buffer_.lineIndexAt(match.begin);
            const auto lineStart = buffer_.lineStart(lineNo);
            const auto bytes = buffer_.line(lineNo);
            const QString line = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
            const auto column = QString::fromUtf8(bytes.data(),
                static_cast<qsizetype>(match.begin - lineStart)).size();
            addRegexReplacement(match, line, column);
        }
    }
    if (replacements.empty()) return;
    const auto cursorAfter = replacements.front().begin + replacements.front().inserted.size();
    std::vector<EditChange> changes;
    changes.reserve(replacements.size());
    constexpr std::size_t maxGroupSpan = 64 * 1024;
    constexpr std::size_t maxGroupMatches = 256;
    for (std::size_t first = 0; first < replacements.size();) {
        std::size_t last = first + 1;
        if (all) {
            while (last < replacements.size() && last - first < maxGroupMatches &&
                   replacements[last].end - replacements[first].begin <= maxGroupSpan)
                ++last;
        }
        const auto begin = replacements[first].begin;
        const auto end = replacements[last - 1].end;
        std::string removed = buffer_.slice(begin, end - begin);
        std::string inserted;
        inserted.reserve(removed.size());
        auto cursor = begin;
        for (auto index = first; index < last; ++index) {
            const auto& item = replacements[index];
            inserted.append(removed, cursor - begin, item.begin - cursor);
            inserted += item.inserted;
            cursor = item.end;
        }
        changes.push_back({begin, std::move(removed), std::move(inserted)});
        first = last;
    }
    applyBatchEdits(std::move(changes), before, {cursorAfter});
    findTimer_->stop(); updateFindMatches();
    if (!all) navigateFind(1);
}

bool EditorWidget::loadFile(const QString& path) {
    PieceTreeBuffer loaded;
    if(!loaded.loadFile(QFile::encodeName(path).toStdString())) return false;
    loadBuffer(std::move(loaded), path);
    return true;
}

void EditorWidget::refreshSyntaxHighlighting() {
    highlighter_.setLanguageForPath(filePath_);
    highlighter_.clear();
    if (highlighter_.usesTreeSitter()) highlighter_.setDocumentText(buffer_);
    viewport()->update();
}


void EditorWidget::goToLine(int oneBasedLine) {
    const auto line = static_cast<std::size_t>(std::max(1, oneBasedLine) - 1);
    moveCursor(buffer_.lineStart(std::min(line, buffer_.lineCount() - 1)), false);
    ensureCursorVisible(true);
    viewport()->update();
}

QPoint EditorWidget::navigationPosition() const {
    const auto line=buffer_.lineIndexAt(cursorPosition_);
    const auto units=buffer_.utf16Before(cursorPosition_)-buffer_.utf16Before(buffer_.lineStart(line));
    return {static_cast<int>(std::min<std::size_t>(line,INT_MAX-1))+1,static_cast<int>(std::min<std::size_t>(units,INT_MAX-1))+1};
}

void EditorWidget::setNavigationPosition(int oneBasedLine, int oneBasedColumn) {
    const auto line = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(1, oneBasedLine) - 1), buffer_.lineCount() - 1);
    const auto lineStart = buffer_.lineStart(line);
    cursorPosition_=std::min(lineTextEnd(buffer_,line),buffer_.byteAtUtf16(buffer_.utf16Before(lineStart)+static_cast<std::size_t>(std::max(0,oneBasedColumn-1))));
    cursors_ = {cursorPosition_};
    cursorAnchors_ = {std::nullopt};
    anchor_.reset();
    preferredColumn_ = static_cast<std::size_t>(-1);
    history_.breakCoalescing();
    ensureCursorVisible();
    viewport()->update();
}

void EditorWidget::revealMatch(int oneBasedLine, int oneBasedColumn, int utf16Length, bool centerVertically) {
    const auto line = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(1, oneBasedLine) - 1), buffer_.lineCount() - 1);
    const auto lineStart = buffer_.lineStart(line);
    const auto units=buffer_.utf16Before(lineStart);
    const auto begin=std::min(buffer_.lineEnd(line),buffer_.byteAtUtf16(units+static_cast<std::size_t>(std::max(0,oneBasedColumn-1))));
    const auto end=std::min(buffer_.lineEnd(line),buffer_.byteAtUtf16(buffer_.utf16Before(begin)+static_cast<std::size_t>(std::max(0,utf16Length))));
    anchor_ = begin;
    cursorPosition_ = end;
    cursors_ = {cursorPosition_};
    cursorAnchors_ = {anchor_};
    history_.breakCoalescing();
    updateScrollBars();
    ensureCursorVisible(centerVertically);
    viewport()->update();
}

void EditorWidget::updateFilePathAfterRename(const QString& path) {
    clearSemanticHighlighting();
    filePath_ = path;
    document_->diskState = diskFileState(path);
    highlighter_.setLanguageForPath(filePath_);
    highlighter_.clear();
    if (highlighter_.usesTreeSitter()) highlighter_.setDocumentText(buffer_);
    viewport()->update();
}

void EditorWidget::markFileMissing() {
    // Retain the path and Undo history, but protect this in-memory copy on close.
    history_.markUnsaved();
    setModified(true);
}

void EditorWidget::detachDeletedFile() {
    clearSemanticHighlighting();
    filePath_.clear();
    highlighter_.setLanguageForPath(QString());
    highlighter_.clear();
    viewport()->update();
}

bool EditorWidget::saveFile(const QString& path) {
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly)) return false;
    if(!buffer_.forEachChunk([&file](std::string_view chunk) {
        return file.write(chunk.data(),static_cast<qint64>(chunk.size()))==static_cast<qint64>(chunk.size());
    }) || !file.commit()) return false;
    const bool languageChanged=QFileInfo(filePath_).suffix().compare(QFileInfo(path).suffix(),Qt::CaseInsensitive)!=0;
    filePath_ = path;
    document_->diskState = diskFileState(path);
    if(languageChanged) {
        highlighter_.setLanguageForPath(path);
        highlighter_.clear();
        if (highlighter_.usesTreeSitter()) highlighter_.setDocumentText(buffer_);
    }
    history_.markSaved();
    setModified(history_.isModified());
    return true;
}

const QString& EditorWidget::cachedLine(std::size_t line) const {
    const qulonglong key = static_cast<qulonglong>(line);
    auto it = lineCache_.find(key);
    if (it != lineCache_.end()) return it.value();
    const std::string bytes = buffer_.line(line);
    if(lineCache_.size()>=512) lineCache_.erase(lineCache_.begin());
    return lineCache_.insert(key, QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()))).value();
}

void EditorWidget::invalidateLineCache(std::size_t changedFrom) {
    codeContext_.invalidate(changedFrom);
    bracketRevision_=~quint64(0);bracketMatch_={};
    if(findCancelled_)findCancelled_->store(true);
    lineCache_.clear();
    if (findBar_ && findBar_->isVisible()) {
        ++findGeneration_;
        findMatches_.clear();
        findTimer_->start();
    }
    hitTestLine_ = -1;
    hitTestOffsets_.clear();
    hitTestColumns_.clear();
}

void EditorWidget::rebuildHitTestCache(int line) const {
    if (hitTestLine_ == line) return;
    hitTestLine_ = line;
    hitTestOffsets_.clear();
    hitTestColumns_.clear();
    const std::string bytes = buffer_.line(static_cast<std::size_t>(line));
    hitTestOffsets_.push_back(0);
    hitTestColumns_.push_back(0);
    std::size_t byteOffset = 0;
    while (byteOffset < bytes.size()) {
        const auto next = EditorUnicode::nextGrapheme(bytes, byteOffset);
        const QString prefix = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(next));
        hitTestOffsets_.push_back(next);
        hitTestColumns_.push_back(fontMetrics().horizontalAdvance(prefix));
        byteOffset = next;
    }
}

int EditorWidget::lineHeight() const { return fontMetrics().height() + 3; }
int EditorWidget::gutterWidth() const {
    return fontMetrics().horizontalAdvance(QString::number(buffer_.lineCount())) + 24;
}
int EditorWidget::textAreaWidth() const {
    return std::max(1, viewport()->width() - gutterWidth() - 16);
}

int EditorWidget::cursorPixelColumn() const {
    const auto lineNo = currentLine();
    const auto start = buffer_.lineStart(lineNo);
    if(buffer_.lineEnd(lineNo)-start>64u*1024u) {
        const auto columns=buffer_.codePointsBefore(cursorPosition_)-buffer_.codePointsBefore(start);
        return static_cast<int>(std::min<std::size_t>(columns*std::max(1,fontMetrics().horizontalAdvance('M')),std::numeric_limits<int>::max()/2));
    }
    const auto prefixBytes = buffer_.slice(start, cursorPosition_ - start);
    const QString prefix = QString::fromUtf8(prefixBytes.data(), static_cast<qsizetype>(prefixBytes.size()));
    return fontMetrics().horizontalAdvance(prefix);
}

int EditorWidget::visualRowForLine(int line) const {
    return line >= 0 && line < diffLineRows_.size() ? diffLineRows_[line] : line;
}
int EditorWidget::documentLineForVisualRow(int row) const {
    if (diffRows_.isEmpty()) return row;
    if (row >= diffRows_.size()) return static_cast<int>(std::min<std::size_t>(buffer_.lineCount(), INT_MAX-1));
    row = std::max(0, row);
    // A click/viewport in padding resolves to the next real line, or EOF.
    for (int i = row; i < diffRows_.size(); ++i) if (diffRows_[i] >= 0) return diffRows_[i];
    for (int i = row; i >= 0; --i) if (diffRows_[i] >= 0) return diffRows_[i];
    return 0;
}
void EditorWidget::setDiffAlignment(const QVector<int>& rows) {
    if (rows == diffRows_) return;
    const int oldTop = verticalScrollBar()->value();
    const int topLine = documentLineForVisualRow(oldTop);
    const int cursorLine = static_cast<int>(currentLine());
    const int cursorRow = visualRowForLine(cursorLine);
    const bool anchorCursor = !diffRows_.isEmpty() && !rows.isEmpty()
        && cursorRow >= oldTop && cursorRow < oldTop + verticalScrollBar()->pageStep();
    // Accept only a complete, ordered map for this exact document revision.
    int next = 0;
    for (int line : rows) {
        if (line < -1 || (line >= 0 && line != next++)) return;
    }
    if (!rows.isEmpty() && std::size_t(next) != buffer_.lineCount()) return;
    diffRows_ = rows;
    if (miniMap_) miniMap_->invalidate();
    diffLineRows_.clear();
    for (int row = 0; row < rows.size(); ++row) if (rows[row] >= 0) diffLineRows_.append(row);
    updateScrollBars();
    verticalScrollBar()->setValue(anchorCursor
        ? oldTop + visualRowForLine(cursorLine) - cursorRow : visualRowForLine(topLine));
    viewport()->update();
    emit semanticViewportChanged();
}

void EditorWidget::adjustDiffHighlights(const HighlightEdit& edit) {
    if (diffHighlights_.isEmpty()) return;
    QHash<int,QPair<int,int>> projected;
    const auto add = [&](int row, int start, int length) {
        auto it = projected.find(row);
        if (it == projected.end()) { projected.insert(row,{start,length}); return; }
        if (length <= 0) return;
        if (it->second <= 0) { *it = {start,length}; return; }
        const int end = std::max(it->first + it->second, start + length);
        it->first = std::min(it->first,start); it->second = end - it->first;
    };
    bool touched = false;
    for (auto it = diffHighlights_.cbegin(); it != diffHighlights_.cend(); ++it) {
        const auto row = static_cast<std::size_t>(it.key());
        // Full-line color survives even if an edit removes every marked character.
        if (row < edit.row) add(it.key(),0,0);
        else if (row > edit.oldEndRow) add(int(row - edit.oldEndRow + edit.newEndRow),0,0);
        else touched = true;
        edit.project(row, DiffHighlight{it.key(),it->first,it->second},
            [&](std::size_t target, const DiffHighlight& span) { add(int(target),span.start,span.length); });
    }
    if (touched) {
        // New text in an already changed line inherits its diff color while
        // the worker computes the exact comparison, including split/join edits.
        for (auto row = edit.row; row <= edit.newEndRow; ++row) {
            const int start = row == edit.row ? edit.column : 0;
            int end = edit.newEndColumn;
            if (row != edit.newEndRow) {
                const auto bytes = buffer_.slice(buffer_.lineStart(row),buffer_.lineEnd(row)-buffer_.lineStart(row));
                end = QString::fromUtf8(bytes.data(),static_cast<qsizetype>(bytes.size())).size();
            }
            add(int(row),start,std::max(0,end-start));
        }
    }
    diffHighlights_ = std::move(projected);
}

void EditorWidget::adjustDiffAlignment(std::size_t firstLine, int removedLines, int insertedLines) {
    if (diffRows_.isEmpty() || (!removedLines && !insertedLines)) return;
    const int first = static_cast<int>(firstLine);
    const int row = visualRowForLine(first);
    if (first < 0 || first >= diffLineRows_.size()) { setDiffAlignment({}); return; }
    // Keep the last comparison's spacers while a fresh diff is being computed.
    // Deleted real lines become provisional spacers; inserted lines first use
    // adjacent spacers, so typing never collapses the whole comparison.
    for (int& line : diffRows_) {
        if (line > first && line <= first + removedLines) line = -1;
        else if (line > first + removedLines) line += insertedLines - removedLines;
    }
    const int at = row + 1;
    int reusable = 0;
    while (reusable < insertedLines && at + reusable < diffRows_.size() && diffRows_[at + reusable] < 0)
        ++reusable;
    if (insertedLines > reusable) diffRows_.insert(at, insertedLines - reusable, -1);
    for (int i = 0; i < insertedLines; ++i) diffRows_[at + i] = first + i + 1;
    diffLineRows_.clear();
    for (int i = 0; i < diffRows_.size(); ++i)
        if (diffRows_[i] >= 0) diffLineRows_.append(i);
}

void EditorWidget::updateScrollBars() {
    const auto maxLines = static_cast<std::size_t>(std::numeric_limits<int>::max() - 1);
    const auto count = diffRows_.isEmpty() ? static_cast<int>(std::min(buffer_.lineCount(), maxLines)) : int(diffRows_.size());
    const int verticalPage = std::max(1, viewport()->height() / lineHeight());
    verticalScrollBar()->setPageStep(verticalPage);
    verticalScrollBar()->setRange(0, std::max(0, count - verticalPage));
    verticalScrollBar()->setSingleStep(1);

    const auto lineNo = currentLine();
    if (horizontalExtentDirty_ || cachedHorizontalLine_ != lineNo) {
        const auto start=buffer_.lineStart(lineNo),end=buffer_.lineEnd(lineNo);
        if(end-start>64u*1024u) {
            const auto columns=buffer_.codePointsBefore(end)-buffer_.codePointsBefore(start);
            cachedHorizontalWidth_=static_cast<int>(std::min<std::size_t>(columns*std::max(1,fontMetrics().horizontalAdvance('M')),std::numeric_limits<int>::max()/2));
        } else {
            const auto bytes=buffer_.line(lineNo);
            cachedHorizontalWidth_=fontMetrics().horizontalAdvance(QString::fromUtf8(bytes.data(),bytes.size()));
        }
        cachedHorizontalLine_ = lineNo;
        horizontalExtentDirty_ = false;
    }
    horizontalScrollBar()->setPageStep(textAreaWidth());
    horizontalScrollBar()->setSingleStep(std::max(8, fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4));
    horizontalScrollBar()->setRange(0, std::max(0, cachedHorizontalWidth_ - textAreaWidth()));
    const int gutter=gutterWidth();
    if(cachedGutterWidth_!=gutter) {
        cachedGutterWidth_=gutter;emit gutterWidthChanged();
    }
}

void EditorWidget::ensureCursorVisible(bool centerVertically) {
    const int line = visualRowForLine(static_cast<int>(std::min<std::size_t>(currentLine(),
        static_cast<std::size_t>(std::numeric_limits<int>::max() - 1))));
    const int page = std::max(1, verticalScrollBar()->pageStep());
    // Explicit navigation leaves context around the target. Typing and cursor
    // movement retain minimal scrolling; the scrollbar clamps near BOF/EOF.
    if (centerVertically) verticalScrollBar()->setValue(line - page / 2);
    else if (line < verticalScrollBar()->value()) verticalScrollBar()->setValue(line);
    else if (line >= verticalScrollBar()->value() + page)
        verticalScrollBar()->setValue(line - page + 1);

    const int cursorX = cursorPixelColumn();
    const int offset = horizontalScrollBar()->value();
    const int width = textAreaWidth();
    if (cursorX < offset) horizontalScrollBar()->setValue(cursorX);
    else if (cursorX > offset + width - 8)
        horizontalScrollBar()->setValue(cursorX - width + 8);
}

std::size_t EditorWidget::currentLine() const {
    return buffer_.lineIndexAt(cursorPosition_);
}
void EditorWidget::clearSelection() {
    anchor_.reset();
    if (!cursorAnchors_.empty()) cursorAnchors_.front().reset();
}
void EditorWidget::clearAllSelections() {
    anchor_.reset();
    cursorAnchors_.assign(cursors_.size(), std::nullopt);
}
void EditorWidget::syncPrimaryCursor() {
    if (cursors_.empty()) cursors_.push_back(0);
    if (cursorAnchors_.size() != cursors_.size())
        cursorAnchors_.resize(cursors_.size(), std::nullopt);
    cursorPosition_ = std::min(cursors_.front(), buffer_.size());
    cursors_.front() = cursorPosition_;
    anchor_ = cursorAnchors_.front();
}
void EditorWidget::normalizeCursors() {
    if (cursorAnchors_.size() != cursors_.size())
        cursorAnchors_.resize(cursors_.size(), std::nullopt);
    std::vector<std::size_t> positions;
    std::vector<std::optional<std::size_t>> anchors;
    positions.reserve(cursors_.size());
    anchors.reserve(cursorAnchors_.size());
    for (std::size_t i = 0; i < cursors_.size(); ++i) {
        const auto position = std::min(cursors_[i], buffer_.size());
        auto selectionAnchor = cursorAnchors_[i];
        if (selectionAnchor) *selectionAnchor = std::min(*selectionAnchor, buffer_.size());
        bool duplicate = false;
        for (std::size_t j = 0; j < positions.size(); ++j) {
            if (positions[j] == position && anchors[j] == selectionAnchor) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            positions.push_back(position);
            anchors.push_back(selectionAnchor);
        }
    }
    cursors_ = std::move(positions);
    cursorAnchors_ = std::move(anchors);
    syncPrimaryCursor();
}
void EditorWidget::addCursorVertically(int direction) {
    if (busy_ || direction == 0 || cursors_.empty()) return;
    std::size_t sourceIndex = 0;
    for (std::size_t i = 1; i < cursors_.size(); ++i) {
        const auto candidateLine = buffer_.lineIndexAt(cursors_[i]);
        const auto sourceLine = buffer_.lineIndexAt(cursors_[sourceIndex]);
        if ((direction < 0 && candidateLine < sourceLine) ||
            (direction > 0 && candidateLine > sourceLine))
            sourceIndex = i;
    }
    const auto sourceCursor = cursors_[sourceIndex];
    const auto sourceLine = buffer_.lineIndexAt(sourceCursor);
    const auto targetLine = static_cast<long long>(sourceLine) + direction;
    if (targetLine < 0 || targetLine >= static_cast<long long>(buffer_.lineCount())) return;

    const auto targetStart = buffer_.lineStart(static_cast<std::size_t>(targetLine));
    const auto targetEnd = lineTextEnd(buffer_,static_cast<std::size_t>(targetLine));
    const auto column = sourceCursor - buffer_.lineStart(sourceLine);
    const auto extraCursor = std::min(targetStart + column, targetEnd);
    if (std::find(cursors_.begin(), cursors_.end(), extraCursor) == cursors_.end()) {
        cursors_.push_back(extraCursor);
        cursorAnchors_.push_back(std::nullopt);
        normalizeCursors();
    }
    history_.breakCoalescing();
    viewport()->update();
}
bool EditorWidget::cursorHasSelection(std::size_t index) const {
    return index < cursors_.size() && index < cursorAnchors_.size() && cursorAnchors_[index] &&
           *cursorAnchors_[index] != cursors_[index];
}
std::pair<std::size_t, std::size_t> EditorWidget::cursorSelectionRange(std::size_t index) const {
    if (!cursorHasSelection(index)) return {cursors_[index], cursors_[index]};
    return {std::min(*cursorAnchors_[index], cursors_[index]),
            std::max(*cursorAnchors_[index], cursors_[index])};
}
std::pair<std::size_t, std::size_t> EditorWidget::selectionRange() const {
    if (!hasSelection()) return {cursorPosition_, cursorPosition_};
    return {std::min(*anchor_, cursorPosition_), std::max(*anchor_, cursorPosition_)};
}

QString EditorWidget::selectedText() const {
    const auto [begin, end] = selectionRange();
    const auto bytes = buffer_.slice(begin, end - begin);
    return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
}

std::pair<QString,bool> EditorWidget::occurrenceQuery() const {
    if (!preeditText_.isEmpty()) return {};
    const auto [begin,end] = selectionRange();
    const auto row = buffer_.lineIndexAt(hasSelection()?begin:cursorPosition_);
    const auto start = buffer_.lineStart(row);
    // Keep automatic highlighting bounded, including on exceptional long lines
    // and large/multiline selections. It never materializes the whole document.
    if (buffer_.lineEnd(row)-start > 64u*1024u) return {};
    if (hasSelection() && (end-begin>4096 || buffer_.lineIndexAt(end)!=row)) return {};
    const auto& text = cachedLine(row);
    qsizetype first = buffer_.utf16Before(hasSelection()?begin:cursorPosition_) - buffer_.utf16Before(start);
    qsizetype last = first;
    if (hasSelection()) {
        last += buffer_.utf16Before(end)-buffer_.utf16Before(begin);
    } else {
        // At the right edge of an identifier, keep that identifier active.
        if (!OccurrenceMatching::isWord(text,first) && OccurrenceMatching::isWord(text,first-1)) --first;
        if (!OccurrenceMatching::isWord(text,first)) return {};
        last=first+1;
        while (OccurrenceMatching::isWord(text,first-1)) --first;
        while (OccurrenceMatching::isWord(text,last)) ++last;
    }
    if (last-first>4096) return {};
    const auto query=text.mid(first,last-first);
    if (query.trimmed().isEmpty()) return {};
    return {query,OccurrenceMatching::wholeWord(text,first,last)};
}

void EditorWidget::paintEvent(QPaintEvent*) {
    updateBracketMatch();
    QPainter painter(viewport());
    // Keep glyph metrics, line drawing and cursor X calculations on one font.
    painter.setFont(font());
    const QFontMetricsF textMetrics(font());
    WorkspaceBackground::instance().paint(viewport(), painter, viewport()->rect(), ThemeManager::instance().color("editor.background"));
    auto currentLineBackground = ThemeManager::instance().color("editor.currentLine");
    if (WorkspaceBackground::instance().enabled()) currentLineBackground.setAlpha(100);
    const int lh = lineHeight();
    const int firstLine = verticalScrollBar()->value();
    const int visibleLines = viewport()->height() / lh + 2;
    highlighter_.setVisibleRange(static_cast<std::size_t>(documentLineForVisualRow(firstLine)),
        static_cast<std::size_t>(documentLineForVisualRow(firstLine+visibleLines-1)));
    const int gutter = gutterWidth();
    const int horizontalOffset = horizontalScrollBar()->value();
    WorkspaceBackground::instance().paint(viewport(), painter, QRect(0, 0, gutter, viewport()->height()), ThemeManager::instance().color("sidebar.background"));
    const auto [selBegin, selEnd] = selectionRange();
    const auto activeLine = currentLine();
    const auto [occurrence,wholeOccurrence] = occurrenceQuery();

    const int visualCount = diffRows_.isEmpty() ? static_cast<int>(std::min<std::size_t>(buffer_.lineCount(), INT_MAX-1)) : int(diffRows_.size());
    const int visibleEnd = static_cast<int>(std::min<long long>(visualCount, static_cast<long long>(firstLine) + visibleLines));
    for (int visualRow = firstLine; visualRow < visibleEnd; ++visualRow) {
        const int lineIndex = diffRows_.isEmpty() ? visualRow : diffRows_[visualRow];
        const int debugY = (visualRow - firstLine) * lh;
        if (lineIndex < 0) {
            painter.fillRect(QRect(gutter, debugY, viewport()->width()-gutter, lh), QColor(128,128,128,22));
            continue;
        }
        const auto lineNo = static_cast<std::size_t>(lineIndex);
        if(debugBreakpoints_.contains(lineIndex+1)) {
            painter.setPen(Qt::NoPen); painter.setBrush(debugDisabledBreakpoints_.contains(lineIndex+1)?QColor(130,130,130):QColor(230,65,70));
            painter.drawEllipse(QPoint(7,debugY+lh/2),5,5); painter.setBrush(Qt::NoBrush);
        }
        if(debugLine_==lineIndex+1) {
            painter.fillRect(QRect(gutter,debugY,viewport()->width()-gutter,lh),QColor(230,180,35,75));
            painter.setPen(QColor(240,195,60)); painter.drawText(QRect(0,debugY,15,lh),Qt::AlignCenter,QString::fromUtf8("➜"));
        }
        const auto start = buffer_.lineStart(lineNo);
        const auto end = buffer_.lineEnd(lineNo);
        if(end-start>64u*1024u) {
            const int y=(visualRow-firstLine)*lh,cell=std::max(1,fontMetrics().horizontalAdvance('M'));
            if(lineNo==activeLine && debugLine_!=lineIndex+1)painter.fillRect(QRect(gutter,y,viewport()->width()-gutter,lh),currentLineBackground);
            painter.setPen(ThemeManager::instance().color("text.muted"));
            painter.drawText(QRect(8,y,gutter-14,lh),Qt::AlignVCenter|Qt::AlignRight,QString::number(lineIndex+1));
            const auto base=buffer_.codePointsBefore(start);
            const auto firstColumn=static_cast<std::size_t>(horizontalOffset/cell);
            const auto first=std::min(end,buffer_.byteAtCodePoint(base+firstColumn));
            const auto last=std::min(end,buffer_.byteAtCodePoint(base+firstColumn+viewport()->width()/cell+4));
            const auto bytes=buffer_.slice(first,last-first);auto chars=QString::fromUtf8(bytes.data(),bytes.size()).toUcs4();
            const auto before=buffer_.codePointsBefore(first)-base;int x=gutter+12+static_cast<int>(before*cell)-horizontalOffset;
            painter.save();painter.setClipRect(QRect(gutter,y,viewport()->width()-gutter,lh));
            const auto selectedFirst=selBegin<=first?0:buffer_.codePointsBefore(std::min(selBegin,last))-buffer_.codePointsBefore(first);
            const auto selectedLast=selEnd>=last?static_cast<std::size_t>(chars.size()):buffer_.codePointsBefore(std::max(selEnd,first))-buffer_.codePointsBefore(first);
            for(qsizetype i=0;i<chars.size();++i) {
                if(hasSelection() && selBegin<last && selEnd>first && static_cast<std::size_t>(i)>=selectedFirst && static_cast<std::size_t>(i)<selectedLast)
                    painter.fillRect(QRect(x,y,cell,lh),ThemeManager::instance().color("editor.selection"));
                painter.setPen(ThemeManager::instance().color("text.primary"));
                painter.drawText(QPoint(x,y+(lh-fontMetrics().height())/2+fontMetrics().ascent()),QString::fromUcs4(reinterpret_cast<const char32_t*>(&chars[i]),1));x+=cell;
            }
            painter.restore();continue;
        }
        const QString& line = cachedLine(lineNo);
        const int y = (visualRow - firstLine) * lh;

        if (lineNo == activeLine && debugLine_!=lineIndex+1)
            painter.fillRect(QRect(gutter, y, viewport()->width() - gutter, lh), currentLineBackground);
        const auto diff=diffHighlights_.constFind(lineIndex);
        if(diff!=diffHighlights_.cend()) {
            painter.fillRect(QRect(gutter,y,viewport()->width()-gutter,lh),QColor(40,170,90,50));
            if(diff->second>0) {
                const int x=gutter+12+fontMetrics().horizontalAdvance(line.left(diff->first))-horizontalOffset;
                const int width=fontMetrics().horizontalAdvance(line.mid(diff->first,diff->second));
                painter.fillRect(QRect(x,y,std::max(2,width),lh),QColor(40,190,90,110));
            }
        }
        // Use the same row rectangle for all text. The drawText(x, y, text)
        // overload treats y as the baseline, which clipped row zero and made
        // the cursor's top-relative geometry disagree with the glyphs.
        painter.setPen(ThemeManager::instance().color("text.muted"));
        painter.drawText(QRect(8, y, gutter - 14, lh),
                         Qt::AlignVCenter | Qt::AlignRight,
                         QString::number(lineIndex + 1));

        painter.save();
        painter.setClipRect(QRect(gutter, 0, viewport()->width() - gutter,
                                  viewport()->height()), Qt::IntersectClip);
        if (!occurrence.isEmpty()) {
            // Only visible ordinary lines are scanned. Cap geometry work for
            // highly repetitive lines; search/selection are painted on top.
            int painted=0;
            qreal nextX=gutter+12-horizontalOffset;
            qsizetype measured=0;
            const qreal width=textMetrics.horizontalAdvance(occurrence);
            for (qsizetype match=line.indexOf(occurrence);match>=0 && painted<256;
                 match=line.indexOf(occurrence,match+occurrence.size())) {
                if (wholeOccurrence && (OccurrenceMatching::isWord(line,match-1) ||
                                        OccurrenceMatching::isWord(line,match+occurrence.size()))) continue;
                const qreal x=nextX+textMetrics.horizontalAdvance(line.mid(measured,match-measured));
                nextX=x+width;
                measured=match+occurrence.size();
                if (x>=viewport()->width()) break;
                if (x+width<=gutter) continue;
                painter.fillRect(QRectF(x,y+2,std::max(qreal(2),width),lh-4),ThemeManager::instance().color("editor.occurrence"));
                ++painted;
            }
        }
        if (!findMatches_.empty()) {
            const auto first = std::lower_bound(findMatches_.begin(), findMatches_.end(), start,
                [](const FindMatch& match, std::size_t offset) { return match.end <= offset; });
            for (auto it = first; it != findMatches_.end() && it->begin < end; ++it) {
                const auto a = std::max(start, it->begin) - start;
                const auto b = std::min(end, it->end) - start;
                const int x = gutter + 12 + fontMetrics().horizontalAdvance(
                    line.left(QString::fromUtf8(buffer_.slice(start, a).c_str()).size())) - horizontalOffset;
                const int width = fontMetrics().horizontalAdvance(QString::fromUtf8(
                    buffer_.slice(start + a, b - a).c_str()));
                const auto index = static_cast<int>(it - findMatches_.begin());
                painter.fillRect(QRect(x, y + 2, std::max(2, width), lh - 4),
                    index == activeFindMatch_ ? ThemeManager::instance().color("search.activeMatch") : ThemeManager::instance().color("search.match"));
            }
        }
        if (selBegin < end && selEnd > start) {
            const auto a = std::max(selBegin, start) - start;
            const auto b = std::min(selEnd, end) - start;
            const auto prefix = buffer_.slice(start, a);
            const auto selected = buffer_.slice(start + a, b - a);
            const QString qPrefix = QString::fromUtf8(prefix.data(), static_cast<qsizetype>(prefix.size()));
            const QString qSelected = QString::fromUtf8(selected.data(), static_cast<qsizetype>(selected.size()));
            const int x = gutter + 12 + fontMetrics().horizontalAdvance(qPrefix) - horizontalOffset;
            const int width = fontMetrics().horizontalAdvance(qSelected);
            painter.fillRect(QRect(x, y + 2, std::max(2, width), lh - 4), ThemeManager::instance().color("editor.selection"));
        }
        if (selBegin <= end && selEnd > end && end < buffer_.size() && buffer_.byteAt(end) == '\n') {
            const int x = gutter + 12 + fontMetrics().horizontalAdvance(line) - horizontalOffset;
            painter.fillRect(QRect(x, y + 2, std::max(2, viewport()->width() - x), lh - 4),
                             ThemeManager::instance().color("editor.selection"));
        }
        for (std::size_t cursorIndex = 1; cursorIndex < cursors_.size(); ++cursorIndex) {
            if (cursorIndex >= cursorAnchors_.size() || !cursorAnchors_[cursorIndex]) continue;
            const auto extraBegin = std::min(*cursorAnchors_[cursorIndex], cursors_[cursorIndex]);
            const auto extraEnd = std::max(*cursorAnchors_[cursorIndex], cursors_[cursorIndex]);
            if (extraBegin >= end || extraEnd <= start) continue;
            const auto a = std::max(extraBegin, start) - start;
            const auto b = std::min(extraEnd, end) - start;
            const auto prefix = buffer_.slice(start, a);
            const auto selected = buffer_.slice(start + a, b - a);
            const QString qPrefix = QString::fromUtf8(prefix.data(), static_cast<qsizetype>(prefix.size()));
            const QString qSelected = QString::fromUtf8(selected.data(), static_cast<qsizetype>(selected.size()));
            const int x = gutter + 12 + fontMetrics().horizontalAdvance(qPrefix) - horizontalOffset;
            const int width = fontMetrics().horizontalAdvance(qSelected);
            painter.fillRect(QRect(x, y + 2, std::max(2, width), lh - 4), ThemeManager::instance().color("editor.selection"));
        }
        for(const auto position:{bracketMatch_.first,bracketMatch_.second}) {
            if(!position || *position<start || *position>=end || (!bracketMatch_.second && !bracketMatch_.unmatched))continue;
            const auto prefix=buffer_.slice(start,*position-start);
            const int x=gutter+12+fontMetrics().horizontalAdvance(QString::fromUtf8(prefix.data(),prefix.size()))-horizontalOffset;
            const int width=fontMetrics().horizontalAdvance(QChar::fromLatin1(buffer_.byteAt(*position)));
            auto color=ThemeManager::instance().color(bracketMatch_.unmatched?"diagnostic.error":"accent");
            auto fill=color;fill.setAlpha(45);painter.fillRect(QRect(x,y+1,std::max(2,width),lh-2),fill);
            painter.setPen(color);painter.drawRect(QRect(x,y+1,std::max(2,width)-1,lh-3));
        }
        QString displayLine = line;
        if (lineNo == activeLine && !preeditText_.isEmpty()) {
            const auto localBytes = std::min(cursorPosition_ - start, end - start);
            const auto prefixBytes = buffer_.slice(start, localBytes);
            const QString compositionPrefix = QString::fromUtf8(prefixBytes.data(), static_cast<qsizetype>(prefixBytes.size()));
            displayLine = compositionPrefix + preeditText_ + line.mid(compositionPrefix.size());
            const int preeditX = gutter + 12 + fontMetrics().horizontalAdvance(compositionPrefix) - horizontalOffset;
            const int preeditWidth = fontMetrics().horizontalAdvance(preeditText_);
            painter.setPen(ThemeManager::instance().color("syntax.variable"));
            painter.drawLine(preeditX, y + lh - 2, preeditX + preeditWidth, y + lh - 2);
        }
        const int textX = gutter + 12 - horizontalOffset;
        const int baseline = y + (lh - fontMetrics().height()) / 2 + fontMetrics().ascent();
        if (!preeditText_.isEmpty() && lineNo == activeLine) {
            painter.setPen(ThemeManager::instance().color("text.primary"));
            painter.drawText(QPoint(textX, baseline), displayLine);
        } else {
            const auto spans = highlighter_.highlightLine(lineNo, start, line,
                [this](std::size_t index) {
                    const std::string bytes = buffer_.slice(buffer_.lineStart(index),std::min<std::size_t>(64*1024,buffer_.lineEnd(index)-buffer_.lineStart(index)));
                    return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
                });
            auto roleFor = [](SyntaxHighlighter::Kind kind) {
                switch (kind) {
                case SyntaxHighlighter::Kind::Keyword: return QStringLiteral("syntax.keyword");
                case SyntaxHighlighter::Kind::Type: return QStringLiteral("syntax.type");
                case SyntaxHighlighter::Kind::Function: return QStringLiteral("syntax.function");
                case SyntaxHighlighter::Kind::Variable: return QStringLiteral("syntax.variable");
                case SyntaxHighlighter::Kind::String: return QStringLiteral("syntax.string");
                case SyntaxHighlighter::Kind::Comment: return QStringLiteral("syntax.comment");
                case SyntaxHighlighter::Kind::Number: return QStringLiteral("syntax.number");
                case SyntaxHighlighter::Kind::Preprocessor: return QStringLiteral("syntax.preprocessor");
                case SyntaxHighlighter::Kind::Parameter: return QStringLiteral("syntax.parameter");
                case SyntaxHighlighter::Kind::Property: return QStringLiteral("syntax.property");
                case SyntaxHighlighter::Kind::BuiltinType: return QStringLiteral("syntax.builtinType");
                case SyntaxHighlighter::Kind::Boolean: return QStringLiteral("syntax.boolean");
                case SyntaxHighlighter::Kind::Operator: return QStringLiteral("syntax.operator");
                case SyntaxHighlighter::Kind::Punctuation: return QStringLiteral("syntax.punctuation");
                case SyntaxHighlighter::Kind::Constant: return QStringLiteral("syntax.constant");
                }
                return QStringLiteral("text.primary");
            };
            const auto semantic=semanticLines_.value(lineIndex);
            QVector<int> boundaries{0,static_cast<int>(line.size())};
            for(const auto& span:spans) { boundaries.append(std::clamp(span.start,0,static_cast<int>(line.size()))); boundaries.append(std::clamp(span.start+span.length,0,static_cast<int>(line.size()))); }
            for(const auto& token:semantic) { boundaries.append(token.start); boundaries.append(token.start+token.length); }
            std::sort(boundaries.begin(),boundaries.end()); boundaries.erase(std::unique(boundaries.begin(),boundaries.end()),boundaries.end());
            struct Segment { int start,length; HighlightStyle style; };
            QVector<Segment> segments; int lexicalIndex=0,semanticIndex=0;
            auto& theme=ThemeManager::instance();
            for(int i=0;i+1<boundaries.size();++i) {
                const int position=boundaries[i],length=boundaries[i+1]-position;
                if(!length || position<0 || position>=line.size()) continue;
                while(lexicalIndex<spans.size() && spans[lexicalIndex].start+spans[lexicalIndex].length<=position) ++lexicalIndex;
                auto style=theme.highlightStyle("text.primary");
                if(lexicalIndex<spans.size() && spans[lexicalIndex].start<=position)
                    style=theme.captureStyle(spans[lexicalIndex].capture,roleFor(spans[lexicalIndex].kind));
                while(semanticIndex<semantic.size() && semantic[semanticIndex].start+semantic[semanticIndex].length<=position) ++semanticIndex;
                if(semanticIndex<semantic.size() && semantic[semanticIndex].start<=position) {
                    const auto overlay=theme.semanticStyle(semantic[semanticIndex].type,semantic[semanticIndex].modifiers);
                    if(overlay.color.isValid()) style.color=overlay.color;
                    style.fontStyles|=overlay.fontStyles;
                }
                if(!segments.isEmpty() && segments.last().start+segments.last().length==position && segments.last().style.color==style.color && segments.last().style.fontStyles==style.fontStyles)
                    segments.last().length+=length;
                else segments.append({position,length,style});
            }
            qreal x=textX;
            for(const auto& segment:segments) {
                QFont styled=font(); styled.setBold(segment.style.fontStyles&1); styled.setItalic(segment.style.fontStyles&2);
                styled.setUnderline(segment.style.fontStyles&4); styled.setStrikeOut(segment.style.fontStyles&8);
                painter.setFont(styled); painter.setPen(segment.style.color);
                const auto part=line.mid(segment.start,segment.length);
                painter.drawText(QPointF(x,baseline),part);
                // Keep fractional advances across color/style boundaries.
                // Rounding every run accumulates drift from whole-line caret,
                // selection and mouse hit-test coordinates.
                x+=textMetrics.horizontalAdvance(part);
            }
            painter.setFont(font());
        }
        const auto diagnosticIt = diagnostics_.constFind(lineIndex);
        if (diagnosticIt != diagnostics_.cend()) {
            for (const auto& diagnostic : diagnosticIt.value()) {
                const int begin = std::clamp(diagnostic.start, 0, static_cast<int>(line.size()));
                const int end = diagnostic.end < 0 ? static_cast<int>(line.size())
                    : std::clamp(diagnostic.end, begin, static_cast<int>(line.size()));
                const int x1 = textX + fontMetrics().horizontalAdvance(line.left(begin));
                const int x2 = textX + fontMetrics().horizontalAdvance(line.left(end));
                painter.setPen(diagnostic.severity == 1 ? ThemeManager::instance().color("diagnostic.error") : ThemeManager::instance().color("diagnostic.warning"));
                painter.drawLine(x1, y + lh - 2, std::max(x1 + 3, x2), y + lh - 2);
            }
        }
        painter.restore();
    }

    const auto lineNo = currentLine();
    int cursorX=gutter+12+cursorPixelColumn()-horizontalOffset;
    const int cursorY = (visualRowForLine(static_cast<int>(lineNo)) - firstLine) * lh;
    const int glyphHeight = fontMetrics().height();
    const int cursorTop = cursorY + (lh - glyphHeight) / 2 + 1;
    const int cursorHeight = std::max(1, glyphHeight - 2);

    if (!preeditText_.isEmpty())
        cursorX += fontMetrics().horizontalAdvance(preeditText_.left(preeditCursor_));
    if (hasFocus() && cursorY + lh > 0 && cursorY < viewport()->height())
        painter.fillRect(QRect(cursorX, cursorTop, 2, cursorHeight), ThemeManager::instance().color("editor.cursor"));

    if (hasFocus()) {
        for (const auto position : cursors_) {
            if (position == cursorPosition_) continue;
            const auto extraLine = buffer_.lineIndexAt(position);
            const auto extraStart = buffer_.lineStart(extraLine);
            const int extraY=(visualRowForLine(static_cast<int>(extraLine))-firstLine)*lh;
            if(extraY+lh<=0 || extraY>=viewport()->height())continue;
            int width=0;
            if(buffer_.lineEnd(extraLine)-extraStart>64u*1024u) {
                const auto columns=buffer_.codePointsBefore(position)-buffer_.codePointsBefore(extraStart);
                width=static_cast<int>(std::min<std::size_t>(INT_MAX/2,columns*std::max(1,fontMetrics().horizontalAdvance('M'))));
            } else {
                const auto prefixBytes=buffer_.slice(extraStart,position-extraStart);
                width=fontMetrics().horizontalAdvance(QString::fromUtf8(prefixBytes.data(),static_cast<qsizetype>(prefixBytes.size())));
            }
            const int extraX=gutter+12+width-horizontalOffset;
            if (extraY + lh > 0 && extraY < viewport()->height())
                painter.fillRect(QRect(extraX, extraY + (lh - glyphHeight) / 2 + 1, 2, cursorHeight),
                                 ThemeManager::instance().color("syntax.variable"));
        }
    }
}

void EditorWidget::setModified(bool value) {
    if(!semanticAdjusted_) { ++semanticGeneration_; setProperty("semanticPending",false); }
    semanticAdjusted_=false;
    ++lspRevision_;
    hideCompletions();
    if (modified_ != value) {
        modified_ = value;
        emit modifiedChanged();
    }
    emit contentChanged();
    const auto notice=history_.takeNotice();if(!notice.empty())emit highlightingStatusChanged(I18n::text(QString::fromStdString(notice)));
}

bool EditorWidget::lspEligible() const {
    return !filePath_.isEmpty() && LspAdaptivePolicy::transportFits(buffer_.size());
}

QString EditorWidget::lspText() const {
    if (!lspEligible()) return {};
    const std::string bytes = buffer_.text();
    return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
}

QPoint EditorWidget::lspPosition() const {
    const auto position=navigationPosition();return {position.x()-1,position.y()-1};
}

QPoint EditorWidget::cursorScreenPosition() const {
    return viewport()->mapToGlobal(inputCursorRect().bottomLeft());
}

void EditorWidget::hideCompletions() {
    if (completer_) completer_->popup()->hide();
}
bool EditorWidget::shouldAutoComplete() const {
    if (busy_ || cursors_.size() != 1 || hasSelection()) return false;
    const auto start = buffer_.lineStart(currentLine());
    const auto bytes = buffer_.slice(start, cursorPosition_ - start);
    const auto text = QString::fromUtf8(bytes.data(), bytes.size());
    if (text.isEmpty()) return false;
    const auto ch = text.back();
    return ch.isLetterOrNumber() || ch == '_' || ch == '.' || text.endsWith("->") || text.endsWith("::");
}
void EditorWidget::showCompletions(const QJsonArray& items) {
    if (busy_ || !hasFocus() || cursors_.size() > 1) return;
    if (!completer_) {
        completer_ = new QCompleter(this);
        completer_->setWidget(this);
        completer_->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
        completer_->setMaxVisibleItems(12);
        completer_->popup()->installEventFilter(this);
        connect(completer_, qOverload<const QModelIndex&>(&QCompleter::activated), this, [this](const QModelIndex& index) {
            const auto item = index.data(Qt::UserRole).toJsonObject();
            hideCompletions();
            if (completionRevision_ == lspRevision_ && completionPosition_ == lspPosition()) applyCompletion(item);
        });
    }
    completionRevision_ = lspRevision_; completionPosition_ = lspPosition();
    auto* model = new QStandardItemModel(completer_);
    int count = 0;
    for (const auto& value : items) {
        const auto object = value.toObject(); const auto label = object.value("label").toString();
        if (label.isEmpty() || object.value("insertTextFormat").toInt(1) == 2) continue;
        auto* item = new QStandardItem(Codicons::completionIcon(object.value("kind").toInt()), label);
        item->setData(object, Qt::UserRole);
        const auto documentation = object.value("documentation");
        item->setToolTip(object.value("detail").toString() + "\n" + (documentation.isString() ? documentation.toString() : documentation.toObject().value("value").toString()));
        model->appendRow(item); if (++count >= 200) break;
    }
    // QCompleter owns and deletes its previous child model in setModel().
    completer_->setModel(model);
    if (!count) { hideCompletions(); return; }
    completer_->setCompletionPrefix({});
    auto rect = inputCursorRect(); rect.setWidth(480); rect.moveTop(rect.bottom());
    completer_->complete(rect);
    completer_->popup()->setCurrentIndex(completer_->completionModel()->index(0, 0));
}
void EditorWidget::applyCompletion(const QJsonObject& item) {
    if (busy_ || item.value("insertTextFormat").toInt(1) == 2 || cursors_.size() > 1) return;
    auto locate = [this](const QJsonObject& point) -> std::optional<std::size_t> {
        const int line = point.value("line").toInt(-1), column = point.value("character").toInt(-1);
        if (line < 0 || static_cast<std::size_t>(line) >= buffer_.lineCount() || column < 0) return {};
        const auto bytes = buffer_.line(line); const auto text = QString::fromUtf8(bytes.data(), bytes.size());
        if (column > text.size() || (column < text.size() && text[column].isLowSurrogate())) return {};
        return buffer_.lineStart(line) + text.left(column).toUtf8().size();
    };
    const auto edit = item.value("textEdit").toObject();
    QString inserted = edit.value("newText").toString(item.value("insertText").toString(item.value("label").toString()));
    std::size_t begin = cursorPosition_, end = cursorPosition_;
    if (!edit.isEmpty()) {
        auto range = edit.value("range").toObject();
        if (range.isEmpty()) range = edit.value("replace").toObject();
        const auto a = locate(range.value("start").toObject()), b = locate(range.value("end").toObject());
        if (!a || !b || *b < *a) return;
        begin = *a; end = *b;
    } else {
        const auto start = buffer_.lineStart(currentLine());
        while (begin > start) {
            const auto bytes = buffer_.slice(start, begin-start);
            const auto text = QString::fromUtf8(bytes.data(), bytes.size());
            if (text.isEmpty() || !(text.back().isLetterOrNumber() || text.back() == '_')) break;
            begin = start + text.left(text.size()-1).toUtf8().size();
        }
    }
    std::vector<EditChange> changes{{begin, buffer_.slice(begin, end-begin), inserted.toUtf8().toStdString()}};
    for (const auto& value : item.value("additionalTextEdits").toArray()) {
        const auto extra = value.toObject(), range = extra.value("range").toObject();
        const auto a = locate(range.value("start").toObject()), b = locate(range.value("end").toObject());
        if (!a || !b || *b < *a) return;
        changes.push_back({*a, buffer_.slice(*a, *b-*a), extra.value("newText").toString().toUtf8().toStdString()});
    }
    std::sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    for (std::size_t i = 1; i < changes.size(); ++i)
        if (changes[i].offset < changes[i-1].offset + changes[i-1].removed.size() || changes[i].offset == changes[i-1].offset) return;
    const auto target = mapPositionThroughChanges(end, changes);
    history_.breakCoalescing();
    applyingCompletion_ = true;
    applyBatchEdits(std::move(changes), {cursorPosition_}, {target});
    applyingCompletion_ = false;
}

void EditorWidget::setDiagnostics(const QJsonArray& items) {
    diagnostics_.clear();
    for (const auto& value : items) {
        const auto object = value.toObject();
        const auto range = object.value(QStringLiteral("range")).toObject();
        const auto start = range.value(QStringLiteral("start")).toObject();
        const auto end = range.value(QStringLiteral("end")).toObject();
        const int firstLine = start.value(QStringLiteral("line")).toInt();
        const int lastLine = end.value(QStringLiteral("line")).toInt();
        if (firstLine < 0 || lastLine < firstLine || lastLine - firstLine > 1000) continue;
        const QString message = object.value(QStringLiteral("message")).toString();
        const int severity = object.value(QStringLiteral("severity")).toInt(1);
        for (int line = firstLine; line <= lastLine; ++line) {
            diagnostics_[line].push_back({
                line == firstLine ? start.value(QStringLiteral("character")).toInt() : 0,
                line == lastLine ? end.value(QStringLiteral("character")).toInt() : -1,
                severity, message});
        }
    }
    viewport()->update();
}
void EditorWidget::moveCursor(std::size_t position, bool keepAnchor) {
    const auto previousOccurrence=occurrenceQuery();
    const auto previousLine = currentLine();
    const bool previousSelection = hasSelection();
    const int previousVerticalOffset = verticalScrollBar()->value();
    const int previousHorizontalOffset = horizontalScrollBar()->value();
    if (!keepAnchor) clearSelection();
    else if (!anchor_) anchor_ = cursorPosition_;
    history_.breakCoalescing();
    const auto previousCursor = cursorPosition_;
    const auto targetCursor = std::min(position, buffer_.size());
    if (cursors_.size() > 1) {
        if (targetCursor >= previousCursor) {
            const auto delta = targetCursor - previousCursor;
            for (auto& cursor : cursors_)
                cursor = std::min(buffer_.size(), cursor + delta);
        } else {
            const auto delta = previousCursor - targetCursor;
            for (auto& cursor : cursors_)
                cursor = cursor >= delta ? cursor - delta : 0;
        }
        if (!keepAnchor) clearAllSelections();
        normalizeCursors();
    } else {
        cursorPosition_ = targetCursor;
        cursors_ = {cursorPosition_};
        cursorAnchors_ = {anchor_};
    }
    preferredColumn_ = static_cast<std::size_t>(-1);
    updateScrollBars();
    ensureCursorVisible();

    const bool scrolled = previousVerticalOffset != verticalScrollBar()->value() ||
                          previousHorizontalOffset != horizontalScrollBar()->value();
    if (scrolled || previousSelection || hasSelection() || previousOccurrence!=occurrenceQuery()) {
        viewport()->update();
    } else {
        const auto updateLine = [this](std::size_t line) {
            const auto first = static_cast<std::size_t>(verticalScrollBar()->value());
            const long long y = (static_cast<long long>(visualRowForLine(int(line))) - static_cast<long long>(first)) * lineHeight();
            if (y + lineHeight() > 0 && y < viewport()->height())
                viewport()->update(QRect(0, static_cast<int>(y), viewport()->width(), lineHeight()));
        };
        updateLine(previousLine);
        updateLine(currentLine());
    }
    if (hasFocus() && QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition);
}

std::size_t EditorWidget::lineSelectionEnd(std::size_t line) const {
    return line + 1 < buffer_.lineCount() ? buffer_.lineStart(line + 1) : buffer_.size();
}

void EditorWidget::selectLineRange(std::size_t begin, std::size_t end) {
    cursors_ = {begin};
    cursorPosition_ = begin;
    cursorAnchors_ = {std::nullopt};
    anchor_.reset();
    moveCursor(end, true);
}

void EditorWidget::replaceRange(std::size_t begin, std::size_t length, const std::string& value) {
    if(length>=1024*1024 || value.size()>=1024*1024) {
        const auto before=cursorPosition_;
        runBulkOperation(I18n::text("Editing text…"),[begin,length,value,before](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
            const auto count=std::min(length,buffer.size()-std::min(begin,buffer.size()));auto payload=std::make_shared<ChunkedFile>();
            for(std::size_t at=0;at<count;at+=64*1024){payload->append(buffer.slice(begin+at,std::min<std::size_t>(64*1024,count-at)));step(static_cast<int>(at*40/std::max<std::size_t>(1,count)));}
            buffer.erase(begin,count);
            for(std::size_t at=0;at<value.size();at+=64*1024){auto chunk=std::string_view(value).substr(at,64*1024);payload->append(chunk);buffer.insert(begin+at,chunk);step(40+static_cast<int>(at*60/std::max<std::size_t>(1,value.size())));}
            history.recordOnDisk(begin,payload,count,value.size(),before,begin+value.size(),0);return HistoryResult{begin+value.size(),{begin+value.size()}};
        });return;
    }
    begin = std::min(begin, buffer_.size());
    length = std::min(length, buffer_.size() - begin);
    const auto changedLine = buffer_.lineIndexAt(begin);
    const auto changedColumn = begin - buffer_.lineStart(changedLine);
    const auto removed = buffer_.slice(begin, length);
    const auto cursorBefore = cursorPosition_;
    if (length) buffer_.erase(begin, length);
    if (!value.empty()) buffer_.insert(begin, value);
    horizontalExtentDirty_ = true;
    invalidateLineCache(begin);
    highlighter_.invalidateFromLine(changedLine);
    if (highlighter_.usesTreeSitter()) highlighter_.applyEdit(begin, changedLine, changedColumn, removed, value, buffer_);
    invalidateSemanticLines(changedLine,changedColumn, removed, value);
    cursorPosition_ = begin + value.size();
    cursors_ = {cursorPosition_};
    cursorAnchors_ = {std::nullopt};
    history_.record(begin, removed, value, cursorBefore, cursorPosition_,
                    EditKind::Standalone, editClock_.elapsed());
    clearSelection();
    preferredColumn_ = static_cast<std::size_t>(-1);
    setModified(history_.isModified());
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
    if (hasFocus() && QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition | Qt::ImSurroundingText);
}

void EditorWidget::applyBatchEdits(std::vector<EditChange> changes,
                                   const std::vector<std::size_t>& cursorsBefore,
                                   const std::vector<std::size_t>& cursorsAfter) {
    if (changes.empty()) return;
    std::sort(changes.begin(), changes.end(), [](const EditChange& a, const EditChange& b) {
        return a.offset < b.offset;
    });
    changes.erase(std::unique(changes.begin(), changes.end(), [](const EditChange& a, const EditChange& b) {
        return a.offset == b.offset && a.removed == b.removed && a.inserted == b.inserted;
    }), changes.end());
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        const auto row=buffer_.lineIndexAt(it->offset),column=it->offset-buffer_.lineStart(row);
        if (!it->removed.empty()) buffer_.erase(it->offset, it->removed.size());
        if (!it->inserted.empty()) buffer_.insert(it->offset, it->inserted);
        trackHighlightEdit(it->offset,row,column,it->removed,it->inserted);
    }
    invalidateLineCache(changes.empty()?buffer_.size():changes.front().offset);
    horizontalExtentDirty_ = true;
    history_.recordBatch(std::move(changes), cursorsBefore, cursorsAfter, editClock_.elapsed());
    cursors_ = cursorsAfter;
    cursorAnchors_.assign(cursors_.size(), std::nullopt);
    normalizeCursors();
    clearAllSelections();
    setModified(history_.isModified());
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
}

std::size_t EditorWidget::previousGraphemePosition() const {
    const auto line=currentLine(),start=buffer_.lineStart(line);
    if(cursorPosition_==start)return line?lineTextEnd(buffer_,line-1):0;
    return lineBoundary(buffer_,line,cursorPosition_,false,false);
}
std::size_t EditorWidget::nextGraphemePosition() const {
    const auto line=currentLine(),end=lineTextEnd(buffer_,line);
    if(cursorPosition_>=end)return std::min(buffer_.size(),buffer_.lineEnd(line)+1);
    return lineBoundary(buffer_,line,cursorPosition_,true,false);
}
std::size_t EditorWidget::previousWordPosition() const {
    const auto line=currentLine(),start=buffer_.lineStart(line);
    if(cursorPosition_==start && line)return lineBoundary(buffer_,line-1,buffer_.lineEnd(line-1),false,true);
    return lineBoundary(buffer_,line,cursorPosition_,false,true);
}
std::size_t EditorWidget::nextWordPosition() const {
    const auto line=currentLine(),end=lineTextEnd(buffer_,line);
    if(cursorPosition_>=end){if(end>=buffer_.size())return buffer_.size();return lineBoundary(buffer_,line+1,buffer_.lineStart(line+1),true,true);}
    return lineBoundary(buffer_,line,cursorPosition_,true,true);
}

void EditorWidget::replaceSelection(const std::string& value) {
    if (cursors_.size() > 1) {
        insertText(value, EditKind::Standalone);
        return;
    }
    if (!hasSelection()) {
        insertText(value, EditKind::Standalone);
        return;
    }
    const auto [begin, end] = selectionRange();
    if(end-begin>=1024*1024 || value.size()>=1024*1024){replaceRange(begin,end-begin,value);return;}
    const auto changedLine = buffer_.lineIndexAt(begin);
    const auto changedColumn = begin - buffer_.lineStart(changedLine);
    const auto removed = buffer_.slice(begin, end - begin);
    const auto cursorBefore = cursorPosition_;
    buffer_.erase(begin, end - begin);
    if (!value.empty()) buffer_.insert(begin, value);
    horizontalExtentDirty_ = true;
    invalidateLineCache(begin);
    highlighter_.invalidateFromLine(changedLine);
    if (highlighter_.usesTreeSitter()) highlighter_.applyEdit(begin, changedLine, changedColumn, removed, value, buffer_);
    invalidateSemanticLines(changedLine,changedColumn, removed, value);
    cursorPosition_ = begin + value.size();
    cursors_ = {cursorPosition_};
    cursorAnchors_ = {std::nullopt};
    history_.record(begin, removed, value, cursorBefore, cursorPosition_,
                    EditKind::Standalone, editClock_.elapsed());
    clearSelection();
    preferredColumn_ = static_cast<std::size_t>(-1);
    setModified(history_.isModified());
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
    if (hasFocus() && QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition | Qt::ImSurroundingText);
}

void EditorWidget::insertText(const std::string& value, EditKind kind) {
    if(value.size()>=1024*1024 || selectionsNeedBackground()){replaceSelectionsInBackground(QString::fromUtf8(value.data(),static_cast<qsizetype>(value.size())));return;}
    bool anySelection = false;
    for (std::size_t i = 0; i < cursors_.size(); ++i)
        anySelection = anySelection || cursorHasSelection(i);
    if (value.empty() && !anySelection) return;
    if (cursors_.size() > 1) {
        const auto beforeCursors = cursors_;
        std::vector<EditChange> changes;
        changes.reserve(beforeCursors.size());
        for (std::size_t i = 0; i < beforeCursors.size(); ++i) {
            const auto cursor = beforeCursors[i];
            const auto anchor = i < cursorAnchors_.size() ? cursorAnchors_[i] : std::nullopt;
            const auto begin = anchor ? std::min(*anchor, cursor) : cursor;
            const auto end = anchor ? std::max(*anchor, cursor) : cursor;
            if (value.empty() && begin == end) continue;
            changes.push_back({begin, buffer_.slice(begin, end - begin), value});
        }
        std::sort(changes.begin(), changes.end(), [](const EditChange& a, const EditChange& b) {
            return a.offset < b.offset;
        });
        std::vector<std::size_t> afterCursors;
        afterCursors.reserve(beforeCursors.size());
        for (const auto cursor : beforeCursors)
            afterCursors.push_back(mapPositionThroughChanges(cursor, changes));
        applyBatchEdits(std::move(changes), beforeCursors, afterCursors);
        return;
    }
    const auto before = cursorPosition_;
    const auto changedLine = buffer_.lineIndexAt(cursorPosition_);
    const auto changedColumn = cursorPosition_ - buffer_.lineStart(changedLine);
    buffer_.insert(cursorPosition_, value);
    horizontalExtentDirty_ = true;
    invalidateLineCache(before);
    highlighter_.invalidateFromLine(changedLine);
    if (highlighter_.usesTreeSitter()) highlighter_.applyEdit(before, changedLine, changedColumn, {}, value, buffer_);
    invalidateSemanticLines(changedLine,changedColumn, {}, value);
    cursorPosition_ += value.size();
    cursors_ = {cursorPosition_};
    cursorAnchors_ = {std::nullopt};
    history_.record(before, {}, value, before, cursorPosition_, kind, editClock_.elapsed());
    clearSelection();
    preferredColumn_ = static_cast<std::size_t>(-1);
    setModified(history_.isModified());
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
    if (hasFocus() && QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition | Qt::ImSurroundingText);
}

void EditorWidget::deleteBackward(bool byWord) {
    if(selectionsNeedBackground()){replaceSelectionsInBackground({});return;}
    if (cursors_.size() > 1) {
        const auto beforeCursors = cursors_;
        std::vector<EditChange> changes;
        for (std::size_t i = 0; i < beforeCursors.size(); ++i) {
            const auto position = beforeCursors[i];
            if (i < cursorAnchors_.size() && cursorAnchors_[i] && *cursorAnchors_[i] != position) {
                const auto begin = std::min(*cursorAnchors_[i], position);
                const auto end = std::max(*cursorAnchors_[i], position);
                changes.push_back({begin, buffer_.slice(begin, end - begin), {}});
                continue;
            }
            if (position == 0) continue;
            const auto line = buffer_.lineIndexAt(position);
            const auto start = buffer_.lineStart(line);
            const auto begin=(position==start)
                ? (byWord && line?lineBoundary(buffer_,line-1,buffer_.lineEnd(line-1),false,true):(line?lineTextEnd(buffer_,line-1):0))
                : lineBoundary(buffer_,line,position,false,byWord);
            if (begin < position) changes.push_back({begin, buffer_.slice(begin, position - begin), {}});
        }
        std::vector<std::size_t> afterCursors;
        std::sort(changes.begin(), changes.end(), [](const EditChange& a, const EditChange& b) {
            return a.offset < b.offset;
        });
        for (const auto position : beforeCursors)
            afterCursors.push_back(mapPositionThroughChanges(position, changes));
        applyBatchEdits(std::move(changes), beforeCursors, afterCursors);
        return;
    }
    if (hasSelection()) { const auto [a,b]=selectionRange();if(b-a>=1024*1024){replaceRange(a,b-a,{});return;}replaceSelection({}); return; }
    if (cursorPosition_ == 0) return;
    const auto begin = byWord ? previousWordPosition() : previousGraphemePosition();
    if (begin >= cursorPosition_) return;
    const auto changedLine = buffer_.lineIndexAt(begin);
    const auto changedColumn = begin - buffer_.lineStart(changedLine);
    const auto removed = buffer_.slice(begin, cursorPosition_ - begin);
    const auto before = cursorPosition_;
    buffer_.erase(begin, cursorPosition_ - begin);
    horizontalExtentDirty_ = true;
    invalidateLineCache(begin);
    highlighter_.invalidateFromLine(changedLine);
    if (highlighter_.usesTreeSitter()) highlighter_.applyEdit(begin, changedLine, changedColumn, removed, {}, buffer_);
    invalidateSemanticLines(changedLine,changedColumn, removed, {});
    cursorPosition_ = begin;
    cursors_ = {cursorPosition_};
    cursorAnchors_ = {std::nullopt};
    history_.record(begin, removed, {}, before, cursorPosition_,
                    byWord ? EditKind::Standalone : EditKind::Backspace, editClock_.elapsed());
    clearSelection();
    setModified(history_.isModified());
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
    if (hasFocus() && QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition | Qt::ImSurroundingText);
}

void EditorWidget::deleteForward(bool byWord) {
    if(selectionsNeedBackground()){replaceSelectionsInBackground({});return;}
    if (cursors_.size() > 1) {
        const auto beforeCursors = cursors_;
        std::vector<EditChange> changes;
        for (std::size_t i = 0; i < beforeCursors.size(); ++i) {
            const auto position = beforeCursors[i];
            if (i < cursorAnchors_.size() && cursorAnchors_[i] && *cursorAnchors_[i] != position) {
                const auto begin = std::min(*cursorAnchors_[i], position);
                const auto end = std::max(*cursorAnchors_[i], position);
                changes.push_back({begin, buffer_.slice(begin, end - begin), {}});
                continue;
            }
            if (position >= buffer_.size()) continue;
            const auto line = buffer_.lineIndexAt(position);
            const auto end = lineTextEnd(buffer_,line);
            const auto target=(position>=end)
                ? (byWord && end<buffer_.size()?lineBoundary(buffer_,line+1,buffer_.lineStart(line+1),true,true):std::min(buffer_.size(),buffer_.lineEnd(line)+1))
                : lineBoundary(buffer_,line,position,true,byWord);
            if (target > position) changes.push_back({position, buffer_.slice(position, target - position), {}});
        }
        std::vector<std::size_t> afterCursors;
        std::sort(changes.begin(), changes.end(), [](const EditChange& a, const EditChange& b) {
            return a.offset < b.offset;
        });
        for (const auto position : beforeCursors)
            afterCursors.push_back(mapPositionThroughChanges(position, changes));
        applyBatchEdits(std::move(changes), beforeCursors, afterCursors);
        return;
    }
    if (hasSelection()) { const auto [a,b]=selectionRange();if(b-a>=1024*1024){replaceRange(a,b-a,{});return;}replaceSelection({}); return; }
    if (cursorPosition_ >= buffer_.size()) return;
    const auto end = byWord ? nextWordPosition() : nextGraphemePosition();
    if (end <= cursorPosition_) return;
    const auto changedLine = buffer_.lineIndexAt(cursorPosition_);
    const auto changedColumn = cursorPosition_ - buffer_.lineStart(changedLine);
    const auto removed = buffer_.slice(cursorPosition_, end - cursorPosition_);
    const auto before = cursorPosition_;
    buffer_.erase(cursorPosition_, end - cursorPosition_);
    horizontalExtentDirty_ = true;
    invalidateLineCache(cursorPosition_);
    highlighter_.invalidateFromLine(changedLine);
    if (highlighter_.usesTreeSitter()) highlighter_.applyEdit(before, changedLine, changedColumn, removed, {}, buffer_);
    invalidateSemanticLines(changedLine,changedColumn, removed, {});
    history_.record(cursorPosition_, removed, {}, before, cursorPosition_,
                    byWord ? EditKind::Standalone : EditKind::DeleteForward, editClock_.elapsed());
    cursors_ = {cursorPosition_};
    cursorAnchors_ = {std::nullopt};
    setModified(history_.isModified());
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
    if (hasFocus() && QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition | Qt::ImSurroundingText);
}

void EditorWidget::undo() {
    if(history_.undoNeedsBackground()){runBulkOperation(I18n::text("Undoing…"),[](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){step(0);auto result=history.undo(buffer,{},[&]{step(50);});step(100);if(!result)throw std::runtime_error("Undo failed");return *result;});return;}
    const auto result = history_.undo(buffer_,[this](std::size_t offset,std::size_t row,std::size_t column,
                                                          std::string_view removed,std::string_view inserted) {
        trackHighlightEdit(offset,row,column,removed,inserted);
    });
    if (!result) return;
    horizontalExtentDirty_ = true;
    invalidateLineCache(buffer_.size());
    cursorPosition_ = result->cursorPosition;
    cursors_ = result->cursorPositions.empty() ? std::vector<std::size_t>{cursorPosition_}
                                                : result->cursorPositions;
    cursorAnchors_.assign(cursors_.size(), std::nullopt);
    normalizeCursors();
    clearAllSelections();
    preferredColumn_ = static_cast<std::size_t>(-1);
    setModified(history_.isModified());
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
    if (hasFocus() && QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition | Qt::ImSurroundingText);
}

void EditorWidget::redo() {
    if(history_.redoNeedsBackground()){runBulkOperation(I18n::text("Redoing…"),[](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step){step(0);auto result=history.redo(buffer,{},[&]{step(50);});step(100);if(!result)throw std::runtime_error("Redo failed");return *result;});return;}
    const auto result = history_.redo(buffer_,[this](std::size_t offset,std::size_t row,std::size_t column,
                                                          std::string_view removed,std::string_view inserted) {
        trackHighlightEdit(offset,row,column,removed,inserted);
    });
    if (!result) return;
    horizontalExtentDirty_ = true;
    invalidateLineCache(buffer_.size());
    cursorPosition_ = result->cursorPosition;
    cursors_ = result->cursorPositions.empty() ? std::vector<std::size_t>{cursorPosition_}
                                                : result->cursorPositions;
    cursorAnchors_.assign(cursors_.size(), std::nullopt);
    normalizeCursors();
    clearAllSelections();
    preferredColumn_ = static_cast<std::size_t>(-1);
    setModified(history_.isModified());
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
    if (hasFocus() && QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition | Qt::ImSurroundingText);
}

void EditorWidget::selectNextOccurrence() {
    if (busy_) return;

        const auto isWord = OccurrenceMatching::isWord;
        std::size_t queryBegin = 0;
        std::size_t queryEnd = 0;
        if (hasSelection()) {
            std::tie(queryBegin, queryEnd) = selectionRange();
        } else {
            const auto line = currentLine();
            const auto lineStart = buffer_.lineStart(line);
            const auto text = cachedLine(line);
            qsizetype local = buffer_.utf16Before(cursorPosition_) - buffer_.utf16Before(lineStart);
            if (local == text.size() && local > 0) --local;
            if (text.isEmpty() || !isWord(text,local)) return;
            qsizetype begin = local;
            qsizetype end = local + 1;
            while (isWord(text,begin - 1)) --begin;
            while (isWord(text,end)) ++end;
            queryBegin = lineStart + static_cast<std::size_t>(text.left(begin).toUtf8().size());
            queryEnd = lineStart + static_cast<std::size_t>(text.left(end).toUtf8().size());
            anchor_ = queryBegin;
            cursorPosition_ = queryEnd;
            cursors_[0] = cursorPosition_;
            cursorAnchors_[0] = anchor_;
        }
        const QString query = QString::fromUtf8(buffer_.slice(queryBegin, queryEnd - queryBegin).data(),
                                                static_cast<qsizetype>(queryEnd - queryBegin));
        if (query.isEmpty()) return;
        const auto queryLine = buffer_.lineIndexAt(queryBegin);
        const auto source = cachedLine(queryLine);
        const auto sourceBegin = static_cast<qsizetype>(buffer_.utf16Before(queryBegin) -
                                                       buffer_.utf16Before(buffer_.lineStart(queryLine)));
        const auto sourceEnd = sourceBegin + query.size();
        const bool wholeWord = OccurrenceMatching::wholeWord(source,sourceBegin,sourceEnd);
        // Continue after the furthest selected occurrence.  Searching from the
        // first occurrence again makes repeated Ctrl+D add the same cursor.
        std::size_t searchStart = queryEnd;
        for (std::size_t i = 0; i < cursors_.size(); ++i) {
            const auto cursor = cursors_[i];
            const auto anchor = i < cursorAnchors_.size() && cursorAnchors_[i]
                ? *cursorAnchors_[i] : cursor;
            searchStart = std::max(searchStart, std::max(anchor, cursor));
        }
        const auto isSelected = [this](std::size_t begin, std::size_t end) {
            for (std::size_t i = 0; i < cursors_.size(); ++i) {
                const auto cursor = cursors_[i];
                const auto anchor = i < cursorAnchors_.size() && cursorAnchors_[i]
                    ? *cursorAnchors_[i] : cursor;
                if (std::min(anchor, cursor) == begin && std::max(anchor, cursor) == end)
                    return true;
            }
            return false;
        };
        const auto startLine = buffer_.lineIndexAt(searchStart);
        const auto lineCount = buffer_.lineCount();
        for (std::size_t step = 0; step < lineCount; ++step) {
            const auto line = (startLine + step) % lineCount;
            const auto lineStart = buffer_.lineStart(line);
            const auto text = cachedLine(line);
            const auto fromBytes = line == startLine && step == 0 ? searchStart - lineStart : 0;
            const auto from = QString::fromUtf8(buffer_.slice(lineStart, fromBytes).data(),
                                                static_cast<qsizetype>(fromBytes)).size();
            auto match = text.indexOf(query, from, Qt::CaseSensitive);
            while (match >= 0) {
                const auto matchBegin = lineStart + static_cast<std::size_t>(text.left(match).toUtf8().size());
                const auto matchEnd = matchBegin + queryEnd - queryBegin;
                if ((!wholeWord || (!isWord(text,match-1) && !isWord(text,match+query.size()))) &&
                    !isSelected(matchBegin, matchEnd)) {
                    cursors_.push_back(matchEnd);
                    cursorAnchors_.push_back(matchBegin);
                    history_.breakCoalescing();
                    viewport()->update();
                    return;
                }
                match = text.indexOf(query, match + std::max<qsizetype>(1, query.size()), Qt::CaseSensitive);
            }
        }
        return;
    }

bool EditorWidget::event(QEvent* event) {
    // QWidget consumes Tab for focus traversal before calling keyPressEvent.
    // Reserve plain Tab/Backtab only while the code surface itself has focus;
    // find/replace fields retain their normal focus chain.
    if(hasFocus() && (event->type()==QEvent::ShortcutOverride || event->type()==QEvent::KeyPress || event->type()==QEvent::KeyRelease)) {
        auto* key=static_cast<QKeyEvent*>(event);
        if((key->key()==Qt::Key_Tab || key->key()==Qt::Key_Backtab)
            && !(key->modifiers() & (Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))) {
            if(event->type()==QEvent::KeyPress)keyPressEvent(key);
            key->accept();return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

void EditorWidget::keyPressEvent(QKeyEvent* event) {
    if (completer_ && completer_->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter: case Qt::Key_Return: case Qt::Key_Escape: case Qt::Key_Tab: case Qt::Key_Backtab:
            event->ignore(); return;
        default:
            if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right || event->key() == Qt::Key_Home || event->key() == Qt::Key_End) hideCompletions();
            break;
        }
    }
    if (busy_) { event->accept(); return; }
    if (event->key()==Qt::Key_Escape && event->modifiers()==Qt::NoModifier && (cursors_.size()>1 || hasSelection())) {
        cursors_={cursorPosition_};
        clearAllSelections();
        preferredColumn_=static_cast<std::size_t>(-1);
        history_.breakCoalescing();
        updateScrollBars();
        ensureCursorVisible();
        viewport()->update();
        event->accept();
        return;
    }
    for(const auto& command:std::vector<std::pair<QString,std::pair<int,bool>>>{
        {"editor.copyLineUp",{-1,true}},{"editor.copyLineDown",{1,true}},
        {"editor.moveLineUp",{-1,false}},{"editor.moveLineDown",{1,false}}})
        if(ShortcutManager::instance().matches(command.first,event)) {editLines(command.second.first,command.second.second);return;}
    if(ShortcutManager::instance().matches("editor.toggleLineComment",event)){toggleLineComment();return;}
    if(ShortcutManager::instance().matches("editor.toggleBlockComment",event)){toggleBlockComment();return;}
    const auto lineNo = currentLine();
    const auto lineStart = buffer_.lineStart(lineNo);
    const auto lineEnd = lineTextEnd(buffer_,lineNo);
    const auto column = cursorPosition_ - lineStart;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    if (ctrl && event->matches(QKeySequence::Undo)) { undo(); return; }
    if (ctrl && event->matches(QKeySequence::Redo)) { redo(); return; }
    if (ctrl && event->matches(QKeySequence::Copy)) {
        std::size_t size=0;for(std::size_t i=0;i<cursors_.size();++i)if(cursorHasSelection(i)){const auto [a,b]=cursorSelectionRange(i);size+=b-a;}
        if(size>=1024*1024){copySelection();return;}
        QStringList selections;
        for (std::size_t i = 0; i < cursors_.size(); ++i) {
            if (!cursorHasSelection(i)) continue;
            const auto [a, b] = cursorSelectionRange(i);
            const auto bytes = buffer_.slice(a, b - a);
            selections.push_back(QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size())));
        }
        if (!selections.isEmpty()) QApplication::clipboard()->setText(selections.join('\n'));
        return;
    }
    if (ctrl && event->matches(QKeySequence::Cut)) {
        std::size_t size=0;for(std::size_t i=0;i<cursors_.size();++i)if(cursorHasSelection(i)){const auto [a,b]=cursorSelectionRange(i);size+=b-a;}
        if(size>=1024*1024){copySelection(true);return;}
        QStringList selections;
        for (std::size_t i = 0; i < cursors_.size(); ++i) {
            if (!cursorHasSelection(i)) continue;
            const auto [a, b] = cursorSelectionRange(i);
            const auto bytes = buffer_.slice(a, b - a);
            selections.push_back(QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size())));
        }
        if (!selections.isEmpty()) {
            QApplication::clipboard()->setText(selections.join('\n'));
            replaceSelection({});
        }
        return;
    }
    if (ctrl && event->matches(QKeySequence::Paste)) { pasteClipboard();return; }
    if (ctrl && event->matches(QKeySequence::SelectAll)) { history_.breakCoalescing(); anchor_ = 0; cursorPosition_ = buffer_.size(); cursors_ = {cursorPosition_}; cursorAnchors_ = {0}; viewport()->update(); return; }
    if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_L) {
        const auto [selectedBegin, selectedEnd] = selectionRange();
        const bool extend = hasSelection() && anchor_ == selectedBegin &&
            cursorPosition_ == selectedEnd && selectedEnd < buffer_.size() &&
            selectedBegin == buffer_.lineStart(buffer_.lineIndexAt(selectedBegin)) &&
            selectedEnd == buffer_.lineStart(buffer_.lineIndexAt(selectedEnd));
        const auto line = extend ? buffer_.lineIndexAt(selectedEnd) : lineNo;
        selectLineRange(extend ? selectedBegin : buffer_.lineStart(line), lineSelectionEnd(line));
        return;
    }

    if (ShortcutManager::instance().matches("editor.nextOccurrence", event)) {
        selectNextOccurrence(); return;
    }

    if (cursors_.size() > 1 &&
        (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right ||
         event->key() == Qt::Key_Up || event->key() == Qt::Key_Down ||
         event->key() == Qt::Key_Home || event->key() == Qt::Key_End)) {
        std::vector<std::size_t> moved;
        moved.reserve(cursors_.size());
        for (std::size_t cursorIndex = 0; cursorIndex < cursors_.size(); ++cursorIndex) {
            const auto cursor = cursors_[cursorIndex];
            const auto line = buffer_.lineIndexAt(cursor);
            const auto start = buffer_.lineStart(line);
            const auto end = lineTextEnd(buffer_,line);
            std::size_t target = cursor;
            const auto window=lineWindow(buffer_,line,cursor);
            const auto& text=window.bytes;
            if (!shift && cursorHasSelection(cursorIndex) && event->key() == Qt::Key_Left) {
                target = cursorSelectionRange(cursorIndex).first;
            } else if (!shift && cursorHasSelection(cursorIndex) && event->key() == Qt::Key_Right) {
                target = cursorSelectionRange(cursorIndex).second;
            } else if (event->key() == Qt::Key_Left) {
                if (ctrl) target = window.begin + EditorUnicode::previousWord(text, cursor-window.begin);
                else if (cursor == start) target = line == 0 ? 0 : lineTextEnd(buffer_,line-1);
                else target = window.begin + EditorUnicode::previousGrapheme(text,cursor-window.begin);
            } else if (event->key() == Qt::Key_Right) {
                if (ctrl) target = window.begin + EditorUnicode::nextWord(text,cursor-window.begin);
                else if (cursor >= end) target = std::min(buffer_.size(), buffer_.lineEnd(line) + 1);
                else target = window.begin + EditorUnicode::nextGrapheme(text,cursor-window.begin);
            } else if (event->key() == Qt::Key_Home) {
                target = ctrl ? 0 : start;
            } else if (event->key() == Qt::Key_End) {
                target = ctrl ? buffer_.size() : end;
            } else {
                const auto targetLine = event->key() == Qt::Key_Up
                    ? (line == 0 ? 0 : line - 1)
                    : std::min(buffer_.lineCount() - 1, line + 1);
                const auto targetStart = buffer_.lineStart(targetLine);
                const auto targetEnd = lineTextEnd(buffer_,targetLine);
                if(targetEnd-targetStart>64u*1024u){moved.push_back(std::min(targetEnd,buffer_.byteAtCodePoint(buffer_.codePointsBefore(targetStart+std::min(cursor-start,targetEnd-targetStart)))));continue;}
                const auto targetText = buffer_.line(targetLine);
                const auto local = std::min(cursor - start, targetText.size());
                const auto safeLocal = (local == targetText.size() || local == 0)
                    ? local
                    : (EditorUnicode::nextGrapheme(targetText,
                                                   EditorUnicode::previousGrapheme(targetText, local)) == local
                        ? local : EditorUnicode::previousGrapheme(targetText, local));
                target = std::min(targetStart + safeLocal, targetEnd);
            }
            moved.push_back(std::min(target, buffer_.size()));
        }
        history_.breakCoalescing();
        if (cursorAnchors_.size() != cursors_.size())
            cursorAnchors_.assign(cursors_.size(), std::nullopt);
        if (shift) {
            for (std::size_t i = 0; i < cursors_.size(); ++i)
                if (!cursorAnchors_[i]) cursorAnchors_[i] = cursors_[i];
        } else {
            clearAllSelections();
        }
        cursors_ = std::move(moved);
        normalizeCursors();
        if (shift) anchor_ = cursorAnchors_.front();
        else clearAllSelections();
        preferredColumn_ = static_cast<std::size_t>(-1);
        updateScrollBars();
        ensureCursorVisible();
        viewport()->update();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Left: {
        const auto [a,b] = selectionRange();
        const auto target = hasSelection() && !shift ? a : (ctrl ? previousWordPosition() : previousGraphemePosition());
        moveCursor(target, shift); break;
    }
    case Qt::Key_Right: {
        const auto [a,b] = selectionRange();
        const auto target = hasSelection() && !shift ? b : (ctrl ? nextWordPosition() : nextGraphemePosition());
        moveCursor(target, shift); break;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        if (preferredColumn_ == static_cast<std::size_t>(-1)) preferredColumn_ = column;
        const auto target = static_cast<long long>(lineNo) + (event->key() == Qt::Key_Up ? -1 : 1);
        if (target >= 0 && target < static_cast<long long>(buffer_.lineCount())) {
            const auto s = buffer_.lineStart(static_cast<std::size_t>(target));
            const auto e = lineTextEnd(buffer_,static_cast<std::size_t>(target));
            if(e-s>64u*1024u){
                const auto local=std::min(preferredColumn_,e-s);
                moveCursor(std::max(s,std::min(e,buffer_.byteAtCodePoint(buffer_.codePointsBefore(s+local)))) ,shift);
                preferredColumn_=column;break;
            }
            const std::string targetText = buffer_.line(static_cast<std::size_t>(target));
            const std::size_t local = std::min(preferredColumn_, targetText.size());
            const auto previous = EditorUnicode::previousGrapheme(targetText, local);
            const std::size_t safeLocal = (local == targetText.size() ||
                EditorUnicode::nextGrapheme(targetText, previous) == local) ? local : previous;
            moveCursor(std::min(s + safeLocal, e), shift);
            preferredColumn_ = column;
        }
        break;
    }
    case Qt::Key_Home: moveCursor(ctrl ? 0 : lineStart, shift); break;
    case Qt::Key_End: moveCursor(ctrl ? buffer_.size() : lineEnd, shift); break;
    case Qt::Key_Backspace: if(!ctrl && smartBackspace())return;deleteBackward(ctrl); return;
    case Qt::Key_Delete: deleteForward(ctrl); return;
    case Qt::Key_Return: case Qt::Key_Enter: smartEnter(); return;
    case Qt::Key_Backtab: indentSelection(true);return;
    case Qt::Key_Tab: indentSelection(shift);return;
    default:
        if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && !event->text().isEmpty() && event->text().at(0).isPrint()) {
            if(event->text().size()==1 && event->text()[0].unicode()<128 && smartCharacter(event->text()[0].toLatin1()))return;
            if (cursors_.size() > 1) insertText(event->text().toUtf8().toStdString(), EditKind::Standalone);
            else if (hasSelection()) replaceSelection(event->text().toUtf8().toStdString());
            else insertText(event->text().toUtf8().toStdString(), EditKind::Insert);
            return;
        }
        event->ignore(); return;
    }
}

int EditorWidget::lineAtY(int y) const {
    const auto line = static_cast<long long>(verticalScrollBar()->value()) +
                      std::max(0, y) / lineHeight();
    return documentLineForVisualRow(static_cast<int>(std::min<long long>(line, std::numeric_limits<int>::max() - 1LL)));
}
std::size_t EditorWidget::byteColumnAtX(int x, int lineNo) const {
    const auto start=buffer_.lineStart(lineNo),end=lineTextEnd(buffer_,lineNo);
    if(end-start>64u*1024u) {
        const auto column=std::max(0,x-gutterWidth()-12+horizontalScrollBar()->value())/std::max(1,fontMetrics().horizontalAdvance('M'));
        return std::min(end,buffer_.byteAtCodePoint(buffer_.codePointsBefore(start)+column))-start;
    }
    rebuildHitTestCache(lineNo);
    const int target = x - gutterWidth() - 12 + horizontalScrollBar()->value();
    if (target <= 0) return 0;
    auto it = std::lower_bound(hitTestColumns_.cbegin(), hitTestColumns_.cend(), target);
    if (it == hitTestColumns_.cend()) return std::min(hitTestOffsets_.back(),end-start);
    const auto index = static_cast<qsizetype>(it - hitTestColumns_.cbegin());
    if (index == 0) return 0;
    const int rightDistance = hitTestColumns_[index] - target;
    const int leftDistance = target - hitTestColumns_[index - 1];
    return std::min(hitTestOffsets_[rightDistance < leftDistance ? index : index - 1],end-start);
}
void EditorWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (busy_) { event->accept(); return; }
    hideCompletions();
    if (event->reason() == QContextMenuEvent::Mouse) {
        const auto point = viewport()->mapFromGlobal(event->globalPos());
        const int line = lineAtY(point.y());
        if (point.x() < gutterWidth() || line < 0 || static_cast<std::size_t>(line) >= buffer_.lineCount()) {
            event->accept(); return;
        }
        cursors_ = {cursorPosition_}; cursorAnchors_ = {std::nullopt};
        moveCursor(buffer_.lineStart(line) + byteColumnAtX(point.x(), line), false);
        viewport()->update();
    }
    setFocus();
    emit codeContextMenuRequested(event->reason() == QContextMenuEvent::Mouse ? event->globalPos() : cursorScreenPosition());
    event->accept();
}
void EditorWidget::mousePressEvent(QMouseEvent* event) {
    hideCompletions();
    if (busy_) { event->accept(); return; }
    if (event->button() == Qt::LeftButton && event->position().x()<15) {
        const int row=verticalScrollBar()->value()+std::max(0,event->position().toPoint().y())/lineHeight();
        if (!diffRows_.isEmpty() && row < diffRows_.size() && diffRows_[row] < 0) { event->accept(); return; }
        const int line=lineAtY(event->position().toPoint().y());
        if(line>=0 && static_cast<std::size_t>(line)<buffer_.lineCount()) emit breakpointToggled(line+1);
        event->accept(); return;
    }
    if (event->button() == Qt::LeftButton) {
        draggingWords_ = false;
        draggingLines_ = false;
        const int clickedLine = lineAtY(event->position().toPoint().y());
        const int lineNo = std::clamp(clickedLine, 0, static_cast<int>(buffer_.lineCount()) - 1);
        const bool hadMultipleCursors = cursors_.size() > 1;
        const bool tripleClick = !(event->modifiers() & Qt::AltModifier) &&
            tripleClickClock_.isValid() &&
            tripleClickClock_.elapsed() <= QApplication::doubleClickInterval() &&
            (event->position().toPoint() - lastDoubleClickPoint_).manhattanLength() <= QApplication::startDragDistance();
        tripleClickClock_.invalidate();
        if (lineNo >= 0 && static_cast<std::size_t>(lineNo) < buffer_.lineCount()) {
            if (tripleClick) {
                lineSelectionBegin_ = buffer_.lineStart(static_cast<std::size_t>(lineNo));
                lineSelectionEnd_ = lineSelectionEnd(static_cast<std::size_t>(lineNo));
                selectLineRange(lineSelectionBegin_, lineSelectionEnd_);
                draggingSelection_ = true;
                draggingLines_ = true;
                setFocus();
                event->accept();
                return;
            }
            const auto pos = clickedLine >= static_cast<int>(buffer_.lineCount()) ? buffer_.size()
                : buffer_.lineStart(static_cast<std::size_t>(lineNo)) + byteColumnAtX(event->position().toPoint().x(), lineNo);
            if (event->modifiers() & Qt::AltModifier) {
                history_.breakCoalescing();
                clearAllSelections();
                if (std::find(cursors_.begin(), cursors_.end(), pos) == cursors_.end())
                    { cursors_.push_back(pos); cursorAnchors_.push_back(std::nullopt); }
                normalizeCursors();
                ensureCursorVisible();
                viewport()->update();
                draggingSelection_ = false;
                setFocus();
                return;
            } else {
                if (cursors_.size() > 1) {
                    cursors_ = {cursorPosition_};
                    cursorAnchors_ = {std::nullopt};
                    clearSelection();
                }
                moveCursor(pos, event->modifiers() & Qt::ShiftModifier);
                // moveCursor only invalidates the old/new primary rows. Removed
                // secondary cursors and selections may have occupied other rows.
                if (hadMultipleCursors) viewport()->update();
            }
            if (!(event->modifiers() & Qt::ShiftModifier)) anchor_ = cursorPosition_;
            draggingSelection_ = true;
            setFocus();
        }
    }
}
void EditorWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (busy_) { event->accept(); return; }
    if (event->button() != Qt::LeftButton || (event->modifiers() & Qt::AltModifier)) {
        mousePressEvent(event);
        return;
    }
    const int lineNo = lineAtY(event->position().toPoint().y());
    if (lineNo < 0 || static_cast<std::size_t>(lineNo) >= buffer_.lineCount()) return;
    const auto start = buffer_.lineStart(static_cast<std::size_t>(lineNo));
    const auto column = byteColumnAtX(event->position().toPoint().x(), lineNo);
    const auto window=lineWindow(buffer_,static_cast<std::size_t>(lineNo),start+column);
    auto range=EditorUnicode::selectionRangeAt(window.bytes,start+column-window.begin);
    range.first+=window.begin-start;range.second+=window.begin-start;
    wordSelectionBegin_ = start + range.first;
    wordSelectionEnd_ = start + range.second;
    cursors_ = {wordSelectionBegin_};
    cursorPosition_ = wordSelectionBegin_;
    cursorAnchors_ = {std::nullopt};
    anchor_.reset();
    moveCursor(wordSelectionEnd_, true);
    draggingSelection_ = true;
    draggingWords_ = true;
    lastDoubleClickPoint_ = event->position().toPoint();
    tripleClickClock_.restart();
    setFocus();
    event->accept();
}
void EditorWidget::mouseMoveEvent(QMouseEvent* event) {
    if (busy_) { event->accept(); return; }
    if (draggingSelection_ && (event->buttons() & Qt::LeftButton)) {
        const auto point = event->position().toPoint();
        const int y = point.y();
        if (y < 0) verticalScrollBar()->setValue(std::max(0, verticalScrollBar()->value() - 1));
        else if (y >= viewport()->height()) verticalScrollBar()->setValue(verticalScrollBar()->value() + 1);
        if (point.x() < gutterWidth() + 12)
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - horizontalScrollBar()->singleStep());
        else if (point.x() >= viewport()->width())
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() + horizontalScrollBar()->singleStep());
        const int lineNo = std::clamp(lineAtY(y), 0, static_cast<int>(buffer_.lineCount()) - 1);
        const auto pos = buffer_.lineStart(static_cast<std::size_t>(lineNo)) + byteColumnAtX(point.x(), lineNo);
        if (draggingLines_) {
            const auto begin = buffer_.lineStart(static_cast<std::size_t>(lineNo));
            const auto end = lineSelectionEnd(static_cast<std::size_t>(lineNo));
            if (begin < lineSelectionBegin_) {
                anchor_ = lineSelectionEnd_;
                cursorAnchors_ = {anchor_};
                moveCursor(begin, true);
            } else {
                anchor_ = lineSelectionBegin_;
                cursorAnchors_ = {anchor_};
                moveCursor(std::max(end, lineSelectionEnd_), true);
            }
        } else if (draggingWords_) {
            const auto start=buffer_.lineStart(static_cast<std::size_t>(lineNo));
            const auto window=lineWindow(buffer_,static_cast<std::size_t>(lineNo),pos);
            auto range=EditorUnicode::selectionRangeAt(window.bytes,pos-window.begin);
            range.first+=window.begin-start;range.second+=window.begin-start;
            const auto begin = buffer_.lineStart(static_cast<std::size_t>(lineNo)) + range.first;
            const auto end = buffer_.lineStart(static_cast<std::size_t>(lineNo)) + range.second;
            if (begin < wordSelectionBegin_) {
                anchor_ = wordSelectionEnd_;
                cursorAnchors_ = {anchor_};
                moveCursor(begin, true);
            } else {
                anchor_ = wordSelectionBegin_;
                cursorAnchors_ = {anchor_};
                moveCursor(std::max(end, wordSelectionEnd_), true);
            }
        } else {
            moveCursor(pos, true);
        }
        return;
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void EditorWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        draggingSelection_ = false;
        draggingWords_ = false;
        draggingLines_ = false;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

bool EditorWidget::eventFilter(QObject* watched, QEvent* event) {
    if(completer_ && watched==completer_->popup() && completer_->popup()->isVisible()
        && (event->type()==QEvent::ShortcutOverride || event->type()==QEvent::KeyPress)) {
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->key()==Qt::Key_Tab && key->modifiers()==Qt::NoModifier) {
            if(event->type()==QEvent::KeyPress) {
                const auto index=completer_->popup()->currentIndex();
                if(index.isValid())completer_->activated(index);else hideCompletions();
            }
            key->accept();return true;
        }
    }
    if ((watched == findInput_ || watched == findReplaceInput_) && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            if(findCancelled_)findCancelled_->store(true);
        ++findGeneration_; findTimer_->stop(); findBar_->hide(); findMatches_.clear(); clearSelection(); setFocus(); viewport()->update();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            navigateFind(key->modifiers() & Qt::ShiftModifier ? -1 : 1);
            return true;
        }
    }
    if (watched == viewport()) {
        switch (event->type()) {
        case QEvent::ToolTip: {
            auto* help = static_cast<QHelpEvent*>(event);
            const int line = lineAtY(help->pos().y());
            const auto diagnostics = diagnostics_.constFind(line);
            if (diagnostics != diagnostics_.cend() && !diagnostics.value().isEmpty()) {
                QToolTip::showText(help->globalPos(), diagnostics.value().first().message, viewport());
                return true;
            }
            if (!busy_ && line >= 0 && static_cast<std::size_t>(line) < buffer_.lineCount()) {
                const auto column = byteColumnAtX(help->pos().x(), line);
                const auto start=buffer_.lineStart(line);
                const int utf16=static_cast<int>(std::min<std::size_t>(INT_MAX,buffer_.utf16Before(start+column)-buffer_.utf16Before(start)));
                emit hoverRequested(line, utf16, help->globalPos());
                return true;
            }
            break;
        }
        case QEvent::ContextMenu:
            contextMenuEvent(static_cast<QContextMenuEvent*>(event));
            return true;
        case QEvent::MouseButtonPress:
            mousePressEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::MouseButtonDblClick:
            mouseDoubleClickEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::MouseMove:
            mouseMoveEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::MouseButtonRelease:
            mouseReleaseEvent(static_cast<QMouseEvent*>(event));
            return true;
        default:
            break;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

void EditorWidget::inputMethodEvent(QInputMethodEvent* event) {
    if (busy_) { event->accept(); return; }
    if (!event->commitString().isEmpty()) {
        const std::string committed = event->commitString().toUtf8().toStdString();
        if (event->replacementLength() != 0 || event->replacementStart() != 0) {
            // QInputMethodEvent replacement offsets are character (UTF-16) offsets,
            // while PieceTreeBuffer uses UTF-8 byte offsets. Translate explicitly.
            const std::size_t windowStart = cursorPosition_ > 8192 ? cursorPosition_ - 8192 : 0;
            const std::string windowBytes = buffer_.slice(windowStart, 16384);
            const QString allText = QString::fromUtf8(windowBytes.data(), static_cast<qsizetype>(windowBytes.size()));
            const std::string prefixBytes = buffer_.slice(windowStart, cursorPosition_ - windowStart);
            const int cursorUtf16 = QString::fromUtf8(prefixBytes.data(), static_cast<qsizetype>(prefixBytes.size())).size();
            const int beginUtf16 = std::clamp(cursorUtf16 + event->replacementStart(), 0, static_cast<int>(allText.size()));
            const int endUtf16 = std::clamp(beginUtf16 + std::max(0, event->replacementLength()),
                                            beginUtf16, static_cast<int>(allText.size()));
            const auto begin = windowStart + static_cast<std::size_t>(allText.left(beginUtf16).toUtf8().size());
            const auto end = windowStart + static_cast<std::size_t>(allText.left(endUtf16).toUtf8().size());
            replaceRange(begin, end - begin, committed);
        } else {
            replaceSelection(committed);
        }
    }

    preeditText_ = event->preeditString();
    preeditCursor_ = preeditText_.size();
    for (const auto& attribute : event->attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            preeditCursor_ = std::clamp(attribute.start, 0, static_cast<int>(preeditText_.size()));
            break;
        }
    }
    event->accept();
    viewport()->update();
    if (QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition | Qt::ImSurroundingText);
}

QRect EditorWidget::inputCursorRect() const {
    const auto lineNo = currentLine();
    int x=gutterWidth()+12+cursorPixelColumn()-horizontalScrollBar()->value();
    if (!preeditText_.isEmpty()) x += fontMetrics().horizontalAdvance(preeditText_.left(preeditCursor_));
    const int y = (visualRowForLine(static_cast<int>(lineNo)) - verticalScrollBar()->value()) * lineHeight();
    return QRect(x, y, 2, lineHeight());
}

QVariant EditorWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    constexpr std::size_t limit=8192;
    const auto begin=buffer_.byteAtCodePoint(buffer_.codePointsBefore(cursorPosition_>limit/2?cursorPosition_-limit/2:0));
    const auto end=std::min(buffer_.size(),buffer_.byteAtCodePoint(buffer_.codePointsBefore(std::min(buffer_.size(),cursorPosition_+limit/2))));
    switch (query) {
    case Qt::ImCursorRectangle: return inputCursorRect();
    case Qt::ImFont: return font();
    case Qt::ImAbsolutePosition: return static_cast<int>(std::min<std::size_t>(INT_MAX,buffer_.utf16Before(cursorPosition_)));
    case Qt::ImSurroundingText: {
        const std::string surrounding = buffer_.slice(begin,end-begin);
        return QString::fromUtf8(surrounding.data(), static_cast<qsizetype>(surrounding.size()));
    }
    case Qt::ImCursorPosition: {
        return static_cast<int>(buffer_.utf16Before(cursorPosition_)-buffer_.utf16Before(begin));
    }
    case Qt::ImAnchorPosition: {
        const auto position=std::clamp(anchor_.value_or(cursorPosition_),begin,end);
        return static_cast<int>(buffer_.utf16Before(position)-buffer_.utf16Before(begin));
    }
    case Qt::ImCurrentSelection: {
        const auto [begin, end] = selectionRange();
        const std::string selected = buffer_.slice(begin, std::min<std::size_t>(limit,end-begin));
        return QString::fromUtf8(selected.data(), static_cast<qsizetype>(selected.size()));
    }
    default: return QAbstractScrollArea::inputMethodQuery(query);
    }
}

void EditorWidget::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    layoutMiniMap();
    positionFindBar();
    updateScrollBars();
    ensureCursorVisible();
    emit semanticViewportChanged();
}

void EditorWidget::scrollContentsBy(int, int) {
    emit semanticViewportChanged();
    // The viewport is painted from the current logical scroll offsets; scrolling
    // pixels from the previous frame would leave stale glyphs behind.
    viewport()->update();
}

bool EditorWidget::validateWorkspaceEdits(const QByteArray& expected,const std::vector<EditChange>& changes) const {
    if (busy_ || buffer_.text()!=expected.toStdString()) return false;
    std::size_t end=0; bool first=true;
    for (const auto& change:changes) {
        if (change.offset>buffer_.size() || change.removed.size()>buffer_.size()-change.offset ||
            (!first && change.offset<end) || buffer_.slice(change.offset,change.removed.size())!=change.removed) return false;
        end=change.offset+change.removed.size(); first=false;
    }
    return true;
}
void EditorWidget::applyWorkspaceEdits(std::vector<EditChange> changes) {
    if (changes.empty()) return;
    const auto before=cursors_;
    std::vector<std::size_t> after;
    for (auto cursor:before) after.push_back(mapPositionThroughChanges(cursor,changes));
    history_.breakCoalescing();
    applyingCompletion_=true;
    applyBatchEdits(std::move(changes),before,after);
    applyingCompletion_=false;
}

void EditorWidget::setSemanticHighlighting(SemanticHighlightResult result,quint64 revision,quint64 generation) {
    if(!result.valid || revision!=lspRevision_ || generation!=semanticGeneration_ || busy_) return;
    semanticLines_=std::move(result.lines); viewport()->update();
}
void EditorWidget::clearSemanticHighlighting() {
    ++semanticGeneration_;
    setProperty("semanticPending",false);
    if(!semanticLines_.isEmpty()) { semanticLines_.clear(); viewport()->update(); }
}
int EditorWidget::semanticTokenCount() const {
    int count=0; for(const auto& row:semanticLines_) count+=row.size(); return count;
}
void EditorWidget::invalidateSemanticLines(std::size_t firstLine,std::size_t byteColumn,std::string_view removed,std::string_view inserted) {
    const int removedLines = static_cast<int>(std::count(removed.begin(),removed.end(),'\n'));
    const int insertedLines = static_cast<int>(std::count(inserted.begin(),inserted.end(),'\n'));
    adjustDiffAlignment(firstLine, removedLines, insertedLines);
    emit debugLinesEdited(static_cast<int>(firstLine),static_cast<int>(byteColumn),removedLines,insertedLines);
    ++semanticGeneration_; semanticAdjusted_=true;
    const auto prefix=buffer_.slice(buffer_.lineStart(firstLine),byteColumn);
    const int column=QString::fromUtf8(prefix.data(),static_cast<qsizetype>(prefix.size())).size();
    HighlightEdit edit(firstLine,column,
        QString::fromUtf8(removed.data(),static_cast<qsizetype>(removed.size())),
        QString::fromUtf8(inserted.data(),static_cast<qsizetype>(inserted.size())));
    adjustDiffHighlights(edit);
    QHash<int,QVector<SemanticToken>> projected;
    for(auto it=semanticLines_.cbegin();it!=semanticLines_.cend();++it)
        for(const auto& token:it.value()) edit.project(it.key(),token,[&](std::size_t row,SemanticToken copy) {
            projected[static_cast<int>(row)].append(std::move(copy));
        });
    for(auto& row:projected) std::sort(row.begin(),row.end(),[](const auto& a,const auto& b){return a.start<b.start;});
    semanticLines_=std::move(projected);
}

void EditorWidget::trackHighlightEdit(std::size_t offset,std::size_t row,std::size_t column,
                                     std::string_view removed,std::string_view inserted) {
    codeContext_.invalidate(offset);
    if(highlighter_.usesTreeSitter()) highlighter_.applyEdit(offset,row,column,removed,inserted,buffer_);
    invalidateSemanticLines(row,column,removed,inserted);
}

QJsonObject EditorWidget::visibleLspRange() const {
    const int first=std::max(0,documentLineForVisualRow(verticalScrollBar()->value())-80);
    const int last=std::min(static_cast<int>(buffer_.lineCount())-1,documentLineForVisualRow(verticalScrollBar()->value()+viewport()->height()/lineHeight())+80);
    const auto units=buffer_.utf16Before(buffer_.lineEnd(last))-buffer_.utf16Before(buffer_.lineStart(last));
    return {{"start",QJsonObject{{"line",first},{"character",0}}},{"end",QJsonObject{{"line",last},{"character",static_cast<int>(std::min<std::size_t>(units,INT_MAX))}}}};
}
QString EditorWidget::lspRangeText(int first,int last) const {
    const auto begin=buffer_.lineStart(static_cast<std::size_t>(first));
    const auto end=buffer_.lineEnd(static_cast<std::size_t>(last));
    const auto bytes=buffer_.slice(begin,end-begin);
    return QString::fromUtf8(bytes.data(),static_cast<qsizetype>(bytes.size()));
}

void EditorWidget::applyHistorySettings() {
    QSettings settings;EditHistory::Limits limits;
    limits.memoryBytes=static_cast<std::size_t>(std::clamp(settings.value("editor/historyMemoryMiB",64).toInt(),1,4096))*1024u*1024u;
    limits.spillToDisk=settings.value("editor/historySpillToDisk",true).toBool();
    limits.diskBytes=static_cast<std::size_t>(std::clamp(settings.value("editor/historyDiskMiB",0).toInt(),0,1048576))*1024u*1024u;
    history_.setLimits(limits);
    const auto notice=history_.takeNotice();if(!notice.empty())emit highlightingStatusChanged(I18n::text(QString::fromStdString(notice)));
}

void EditorWidget::applyFontSettings() {
    hideCompletions();
    setFont(FontSettings::font(FontSettings::Area::Editor));
    horizontalExtentDirty_=true;
    updateScrollBars();
    ensureCursorVisible();
    positionFindBar();
    emit semanticViewportChanged();
    viewport()->update();
}

void EditorWidget::runBulkOperation(const QString& label,BulkTask task,bool modifies,std::function<void()> committed) {
    if(busy_)return;
    setBusy(true);const auto revision=lspRevision_;const auto doc=document_;QPointer<EditorWidget> guard(this);
    auto cancelled=std::make_shared<std::atomic<bool>>(false);auto percent=std::make_shared<std::atomic<int>>(0);
    connect(this,&QObject::destroyed,[cancelled]{cancelled->store(true);});
    auto* progress=new IconDialogs::ProgressDialog(label,I18n::text("Cancel"),0,100,this);progress->setWindowModality(Qt::NonModal);progress->setMinimumDuration(300);
    connect(progress,&QProgressDialog::canceled,progress,[cancelled]{cancelled->store(true);});
    auto* tick=new QTimer(progress);tick->setInterval(50);connect(tick,&QTimer::timeout,progress,[progress,percent,cancelled]{if(progress->wasCanceled())cancelled->store(true);else progress->setValue(percent->load());});tick->start();
    auto* watcher=new QFutureWatcher<std::shared_ptr<BulkResult>>(this);
    connect(watcher,&QFutureWatcher<std::shared_ptr<BulkResult>>::finished,this,[this,guard,watcher,progress,revision,modifies,cancelled,committed] {
        const auto result=watcher->result();
        if(progress->wasCanceled())cancelled->store(true);
        watcher->deleteLater();progress->deleteLater();if(!guard)return;
        setBusy(false);
        if(result->cancelled || cancelled->load() || lspRevision_!=revision){applyHistorySettings();return;}
        if(!result->error.isEmpty()){emit highlightingStatusChanged(result->error);return;}
        if(modifies) {
            QMap<int,int> anchors;
            for(const auto& [original,mapped]:result->lineAnchors) anchors.insert(original,mapped);
            if(!anchors.isEmpty()) emit debugAnchorsMoved(anchors);
            diffHighlights_.clear();
            const auto top=verticalScrollBar()->value();buffer_=std::move(*result->buffer);history_=std::move(*result->history);
            cursorPosition_=std::min(result->cursor.cursorPosition,buffer_.size());cursors_=result->cursor.cursorPositions.empty()?std::vector<std::size_t>{cursorPosition_}:result->cursor.cursorPositions;
            cursorAnchors_.assign(cursors_.size(),std::nullopt);clearAllSelections();invalidateLineCache();horizontalExtentDirty_=true;
            clearSemanticHighlighting();highlighter_.setDocumentText(buffer_);setModified(history_.isModified());updateScrollBars();verticalScrollBar()->setValue(top);ensureCursorVisible();viewport()->update();
        }
        if(committed)committed();
        if(result->clipboard)QApplication::clipboard()->setMimeData(new StoredClipboard(result->clipboard));
        applyHistorySettings();
    });
    watcher->setFuture(QtConcurrent::run([doc,task=std::move(task),cancelled,percent,anchors=debugBreakpoints_.values()] {
        auto result=std::make_shared<BulkResult>();
        const auto step=[&](int progress){if(cancelled->load())throw std::runtime_error("Operation cancelled");percent->store(progress);};
        try {
            step(0);result->buffer=std::make_shared<PieceTreeBuffer>(doc->buffer().snapshot());result->history=std::make_shared<EditHistory>(doc->history());step(0);
            for(int line:anchors) result->lineAnchors.emplace(line,line);
            if(!anchors.isEmpty()) result->buffer->lineEditObserver=[&](std::size_t row,std::size_t column,std::size_t removed,std::size_t inserted){
                if(!removed && !inserted) return;
                for(auto& [original,line]:result->lineAnchors) { (void)original; line=projectLineAnchor(line,static_cast<int>(row),static_cast<int>(column),static_cast<int>(removed),static_cast<int>(inserted)); }
            };
            result->cursor=task(*result->buffer,*result->history,result->clipboard,step);result->buffer->lineEditObserver={};step(100);
        } catch(const std::exception& error){result->cancelled=cancelled->load();result->error=QString::fromUtf8(error.what());}
        return result;
    }));
}
void EditorWidget::pasteClipboard() {
    const auto* stored=dynamic_cast<const StoredClipboard*>(QApplication::clipboard()->mimeData());
    const auto source=stored?stored->file:std::shared_ptr<ChunkedFile>{};
    const QString text=source?QString{}:QApplication::clipboard()->text();
    if(!source && text.size()<512*1024){replaceSelection(text.toUtf8().toStdString());return;}
    replaceSelectionsInBackground(text,source);
}
bool EditorWidget::selectionsNeedBackground() const {
    std::size_t size=0;for(std::size_t i=0;i<cursors_.size();++i){const auto [a,b]=cursorSelectionRange(i);size+=b-a;if(size>=1024*1024)return true;}
    return false;
}
void EditorWidget::replaceSelectionsInBackground(const QString& text,std::shared_ptr<ChunkedFile> source) {
    std::vector<std::pair<std::size_t,std::size_t>> ranges;
    for(std::size_t i=0;i<cursors_.size();++i)ranges.push_back(cursorSelectionRange(i));
    std::sort(ranges.begin(),ranges.end());ranges.erase(std::unique(ranges.begin(),ranges.end()),ranges.end());
    const auto before=cursors_;
    runBulkOperation(I18n::text("Editing text…"),[ranges,text,source,before](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
        auto inserted=source?source:std::make_shared<ChunkedFile>();
        if(!source)for(qsizetype at=0;at<text.size();) {
            auto end=std::min<qsizetype>(at+32768,text.size());if(end<text.size() && text[end].isLowSurrogate())--end;
            const auto chunk=text.mid(at,end-at).toUtf8();inserted->append({chunk.data(),static_cast<std::size_t>(chunk.size())});at=end;step(static_cast<int>(at*20/std::max<qsizetype>(1,text.size())));
        }
        auto historyPayload=std::make_shared<ChunkedFile>();std::vector<EditHistory::StoredChange> plan;
        std::vector<std::size_t> afterCursors;std::ptrdiff_t delta=0;
        for(auto [a,b]:ranges){
            for(auto at=a;at<b;at+=64*1024){historyPayload->append(buffer.slice(at,std::min<std::size_t>(64*1024,b-at)));step(20);}
            for(std::size_t at=0;at<inserted->size();at+=64*1024){historyPayload->append(inserted->read(at,std::min<std::size_t>(64*1024,inserted->size()-at)));step(30);}
            plan.push_back({a,b-a,inserted->size()});afterCursors.push_back(static_cast<std::size_t>(static_cast<std::ptrdiff_t>(a+inserted->size())+delta));
            delta+=static_cast<std::ptrdiff_t>(inserted->size())-static_cast<std::ptrdiff_t>(b-a);
        }
        for(auto range=ranges.rbegin();range!=ranges.rend();++range){
            buffer.erase(range->first,range->second-range->first);
            for(std::size_t at=0;at<inserted->size();at+=64*1024){const auto bytes=inserted->read(at,std::min<std::size_t>(64*1024,inserted->size()-at));buffer.insert(range->first+at,bytes);step(40+static_cast<int>(at*50/std::max<std::size_t>(1,inserted->size())));}
        }
        history.recordDiskBatch(plan,historyPayload,before,afterCursors,0);return HistoryResult{afterCursors.back(),afterCursors};
    });
}
void EditorWidget::copySelection(bool cut) {
    if(busy_)return;
    std::vector<std::pair<std::size_t,std::size_t>> ranges;
    for(std::size_t i=0;i<cursors_.size();++i)if(cursorHasSelection(i))ranges.push_back(cursorSelectionRange(i));
    if(ranges.empty())return;
    std::sort(ranges.begin(),ranges.end());const auto before=cursorPosition_;
    runBulkOperation(cut?I18n::text("Cutting…"):I18n::text("Copying…"),[ranges,before,cut](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>& clipboard,const BulkStep& step){
        clipboard=std::make_shared<ChunkedFile>();std::size_t processed=0,total=0;for(auto [a,b]:ranges)total+=b-a;
        for(auto [a,b]:ranges){if(processed)clipboard->append("\n");for(auto at=a;at<b;at+=64*1024){auto bytes=buffer.slice(at,std::min<std::size_t>(64*1024,b-at));clipboard->append(bytes);processed+=bytes.size();step(static_cast<int>(processed*80/std::max<std::size_t>(1,total)));}}
        if(cut) {
            if(ranges.size()==1){auto [a,b]=ranges[0];buffer.erase(a,b-a);history.recordOnDisk(a,clipboard,b-a,0,before,a,0);}
            else{
                auto payload=std::make_shared<ChunkedFile>();std::vector<EditHistory::StoredChange> changes;
                for(auto [a,b]:ranges){for(auto at=a;at<b;at+=64*1024){payload->append(buffer.slice(at,std::min<std::size_t>(64*1024,b-at)));step(85);}changes.push_back({a,b-a,0});}
                for(auto it=ranges.rbegin();it!=ranges.rend();++it)buffer.erase(it->first,it->second-it->first);
                history.recordDiskBatch(changes,payload,{before},{ranges[0].first},0);
            }
        }
        return HistoryResult{cut?ranges[0].first:before,{cut?ranges[0].first:before}};
    },cut);
}

void EditorWidget::replaceAllInBackground(const QString& replacement) {
    const auto matches=findMatches_;const auto cursors=cursors_;const bool regex=findRegex_->isChecked();
    QString pattern=findInput_->text();if(findWord_->isChecked())pattern=QStringLiteral("(?<![\\p{L}\\p{N}_])(?:%1)(?![\\p{L}\\p{N}_])").arg(pattern);
    const auto options=findCase_->isChecked()?QRegularExpression::NoPatternOption:QRegularExpression::CaseInsensitiveOption;
    runBulkOperation(I18n::text("Replacing matches…"),[matches,cursors,replacement,regex,pattern,options](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
        auto payload=std::make_shared<ChunkedFile>();std::vector<EditHistory::StoredChange> plan;std::vector<std::size_t> insertedAt;
        QRegularExpression expression(pattern,options);std::ptrdiff_t delta=0;
        for(std::size_t i=0;i<matches.size();++i) {
            step(static_cast<int>(i*50/std::max<std::size_t>(1,matches.size())));const auto match=matches[i];
            QString expanded=replacement;
            if(regex) {
                const auto row=buffer.lineIndexAt(match.begin),start=buffer.lineStart(row),end=buffer.lineEnd(row);
                if(end-start>256*1024)throw std::runtime_error("Regex replacement on very long lines is limited; use literal replacement.");
                const auto bytes=buffer.slice(start,end-start);const auto line=QString::fromUtf8(bytes.data(),bytes.size());
                const auto column=QString::fromUtf8(bytes.data(),match.begin-start).size();const auto found=expression.match(line,column);expanded.clear();
                if(!found.hasMatch() || found.capturedStart()!=column)throw std::runtime_error("Search results changed; retry replacement.");
                for(qsizetype at=0;at<replacement.size();++at){
                    if(replacement[at]=='$' && at+1<replacement.size()){
                        if(replacement[at+1]=='$'){expanded+='$';++at;continue;}
                        const int group=replacement[at+1].digitValue();if(group>=0 && group<=found.lastCapturedIndex()){expanded+=found.captured(group);++at;continue;}
                    }
                    expanded+=replacement[at];
                }
            }
            const auto removed=match.end-match.begin;
            for(std::size_t at=0;at<removed;at+=64*1024){payload->append(buffer.slice(match.begin+at,std::min<std::size_t>(64*1024,removed-at)));step(static_cast<int>(i*50/std::max<std::size_t>(1,matches.size())));}
            const auto bytes=expanded.toUtf8();insertedAt.push_back(payload->size());payload->append({bytes.data(),static_cast<std::size_t>(bytes.size())});
            plan.push_back({match.begin,removed,static_cast<std::size_t>(bytes.size())});
            delta+=bytes.size()-static_cast<std::ptrdiff_t>(removed);
        }
        for(std::size_t i=plan.size();i-- >0;){step(50+static_cast<int>((plan.size()-i)*50/std::max<std::size_t>(1,plan.size())));const auto change=plan[i];buffer.erase(change.offset,change.removed);
            for(std::size_t at=0;at<change.inserted;at+=64*1024){const auto bytes=payload->read(insertedAt[i]+at,std::min<std::size_t>(64*1024,change.inserted-at));buffer.insert(change.offset+at,bytes);}}
        const auto after=static_cast<std::size_t>(static_cast<std::ptrdiff_t>(matches.back().end)+delta);
        history.recordDiskBatch(plan,payload,cursors,{after},0);return HistoryResult{after,{after}};
    });
}

EditorWidget::~EditorWidget() {if(findCancelled_)findCancelled_->store(true);}

void EditorWidget::copyLines(int direction) { editLines(direction,true); }
void EditorWidget::moveLines(int direction) { editLines(direction,false); }
void EditorWidget::editLines(int direction,bool copy) {
    if(busy_ || direction==0 || !isEnabled())return;
    direction=direction<0?-1:1;
    const auto before=cursors_;const auto anchors=cursorAnchors_;
    std::size_t affected=0;
    for(auto block:LineOperations::ranges(buffer_,before,anchors)) {
        if(!copy && ((direction<0 && block.first==0) || (direction>0 && block.last+1==buffer_.lineCount())))continue;
        const auto first=block.first-(!copy && direction<0?1:0),last=block.last+(!copy && direction>0?1:0);
        affected+=(last+1<buffer_.lineCount()?buffer_.lineStart(last+1):buffer_.size())-buffer_.lineStart(first);
    }
    auto plan=std::make_shared<LineOperations::Plan>();
    const auto restoreSelection=[this,plan] {
        cursors_=plan->cursors;cursorAnchors_=plan->anchors;normalizeCursors();
        preferredColumn_=static_cast<std::size_t>(-1);ensureCursorVisible();viewport()->update();
    };
    if(affected>=1024*1024) {
        runBulkOperation(I18n::text("Editing lines…"),[before,anchors,direction,copy,plan](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
            *plan=LineOperations::prepare(buffer,before,anchors,direction,copy,[&]{step(10);});
            for(auto it=plan->changes.rbegin();it!=plan->changes.rend();++it) {
                step(50);buffer.erase(it->offset,it->removed.size());buffer.insert(it->offset,it->inserted);
            }
            history.recordBatch(std::move(plan->changes),before,plan->cursors,0);
            return HistoryResult{plan->cursors.front(),plan->cursors};
        },true,restoreSelection);
        return;
    }
    *plan=LineOperations::prepare(buffer_,before,anchors,direction,copy);
    if(plan->changes.empty())return;
    history_.breakCoalescing();hideCompletions();
    applyBatchEdits(std::move(plan->changes),before,plan->cursors);
    restoreSelection();
}

CodeContext::Rules EditorWidget::editingRules() const {
    CodeContext::Rules rules;
    const auto* language=LanguageRegistry::languageForPath(filePath_);
    const auto id=language?language->id:QString();
    if(QStringList{"python","bash","ruby","perl","cmake","make","meson","nix","dockerfile","nginx","yaml","toml"}.contains(id) || QFileInfo(filePath_).suffix()=="cmake" || QFileInfo(filePath_).fileName()=="CMakeLists.txt") {
        rules.line="#";rules.open=id=="nix"?"/*":"";rules.close=id=="nix"?"*/":"";rules.rawCpp=false;rules.triple=id=="python";
    } else if(id=="ini" || id=="assembly") {
        rules.line=";";rules.open.clear();rules.close.clear();rules.rawCpp=false;
    } else if(id=="sql") {
        rules.line="--";rules.rawCpp=false;
    } else if(id=="lua") {
        rules.line="--";rules.open="--[[";rules.close="]]";rules.rawCpp=false;
    } else if(QStringList{"html","xml","markdown"}.contains(id)) {
        rules.line.clear();rules.open="<!--";rules.close="-->";rules.rawCpp=false;
    } else {
        rules.rawCpp=id=="cpp" || id=="c" || id.isEmpty();
        rules.backtick=id=="javascript" || id=="typescript" || id=="tsx" || id=="go";
        if(id=="css" || id=="json")rules.line.clear();
        if(id=="json"){rules.open.clear();rules.close.clear();}
    }
    return rules;
}

void EditorWidget::updateBracketMatch() {
    if(!QSettings().value("editor/highlightBrackets",true).toBool()) {
        bracketTimer_->stop();bracketMatch_={};bracketRevision_=~quint64(0);return;
    }
    if(bracketRevision_==lspRevision_ && bracketCursor_==cursorPosition_ && bracketPath_==filePath_)return;
    bracketRevision_=lspRevision_;bracketCursor_=cursorPosition_;bracketPath_=filePath_;
    bracketMatch_={};bracketTimer_->start(25);
}

void EditorWidget::applySmartPlan(SmartEditing::Plan plan) {
    if(plan.cursors.empty())return;
    if(!plan.changes.empty()) {
        history_.breakCoalescing();hideCompletions();
        applyBatchEdits(std::move(plan.changes),cursors_,plan.cursors);
    } else history_.breakCoalescing();
    cursors_=std::move(plan.cursors);cursorAnchors_=std::move(plan.anchors);normalizeCursors();
    preferredColumn_=static_cast<std::size_t>(-1);updateScrollBars();ensureCursorVisible();viewport()->update();
}

void EditorWidget::runSmartCommand(SmartBuilder builder) {
    if(busy_ || !isEnabled())return;
    std::size_t affected=0,lines=0;
    for(const auto block:LineOperations::ranges(buffer_,cursors_,cursorAnchors_)) {
        affected+=(block.last+1<buffer_.lineCount()?buffer_.lineStart(block.last+1):buffer_.size())-buffer_.lineStart(block.first);
        lines+=block.last-block.first+1;
    }
    if(affected>=1024*1024 || lines>4096) {
        const auto before=cursors_;auto plan=std::make_shared<SmartEditing::Plan>();
        runBulkOperation(I18n::text("Editing code…"),[builder,before,plan](PieceTreeBuffer& buffer,EditHistory& history,std::shared_ptr<ChunkedFile>&,const BulkStep& step) {
            *plan=builder(buffer,[&]{step(20);});
            for(auto it=plan->changes.rbegin();it!=plan->changes.rend();++it) {
                step(60);buffer.erase(it->offset,it->removed.size());buffer.insert(it->offset,it->inserted);
            }
            history.breakCoalescing();history.recordBatch(std::move(plan->changes),before,plan->cursors,0);
            return HistoryResult{plan->cursors.front(),plan->cursors};
        },true,[this,plan]{cursors_=plan->cursors;cursorAnchors_=plan->anchors;normalizeCursors();ensureCursorVisible();viewport()->update();});
        return;
    }
    try {applySmartPlan(builder(buffer_,{}));}
    catch(const std::exception& error){emit highlightingStatusChanged(I18n::text(QString::fromUtf8(error.what())));}
}

void EditorWidget::toggleLineComment() {
    const auto before=cursors_;const auto anchors=cursorAnchors_;const auto rules=editingRules();
    if(rules.line.empty() && rules.open.empty()){emit highlightingStatusChanged(I18n::text("Comments are not supported for this file type."));return;}
    runSmartCommand([before,anchors,rules](const PieceTreeBuffer& buffer,const std::function<void()>& checkpoint) {
        return rules.line.empty()?SmartEditing::blockComments(buffer,before,anchors,rules,checkpoint):SmartEditing::lineComments(buffer,before,anchors,rules,checkpoint);
    });
}
void EditorWidget::toggleBlockComment() {
    const auto before=cursors_;const auto anchors=cursorAnchors_;const auto rules=editingRules();
    if(rules.line.empty() && rules.open.empty()){emit highlightingStatusChanged(I18n::text("Comments are not supported for this file type."));return;}
    runSmartCommand([before,anchors,rules](const PieceTreeBuffer& buffer,const std::function<void()>& checkpoint) {
        return SmartEditing::blockComments(buffer,before,anchors,rules,checkpoint);
    });
}
void EditorWidget::indentSelection(bool unindent) {
    if(busy_)return;
    const auto before=cursors_;const auto anchors=cursorAnchors_;const auto width=tabWidth_;
    bool selected=false;for(std::size_t i=0;i<before.size();++i)selected=selected || cursorHasSelection(i);
    if(!selected && !unindent){replaceSelection(std::string(width,' '));return;}
    runSmartCommand([before,anchors,unindent,width](const PieceTreeBuffer& buffer,const std::function<void()>& checkpoint) {
        return SmartEditing::indent(buffer,before,anchors,unindent,width,checkpoint);
    });
}

bool EditorWidget::smartCharacter(char character) {
    if(!QSettings().value("editor/autoPairs",true).toBool())return false;
    const char closing=CodeContext::closeFor(character);
    if(!closing && selectionsNeedBackground())return false;
    if(!closing && character!=')' && character!=']' && character!='}')return false;
    struct Target {std::size_t begin,end;bool surround{false},pair{false},skip{false};};
    std::vector<Target> targets;SmartEditing::Plan plan;bool handled=false;
    const auto rules=editingRules();
    for(std::size_t i=0;i<cursors_.size();++i) {
        const auto [a,b]=cursorSelectionRange(i);Target target{a,b};
        if(a!=b && closing) {
            target.surround=true;handled=true;
            plan.changes.push_back({a,{},std::string(1,character)});plan.changes.push_back({b,{},std::string(1,closing)});
        } else {
            const auto state=codeContext_.at(buffer_,a,rules);
            const char next=a<buffer_.size()?buffer_.byteAt(a):'\0',previous=a?buffer_.byteAt(a-1):'\0';
            const bool quote=character=='\'' || character=='"';
            const bool inQuote=state && !state->escaped && ((character=='"' && state->mode==CodeContext::Mode::Double) || (character=='\'' && state->mode==CodeContext::Mode::Single));
            target.skip=a==b && next==character && ((quote && inQuote) || (!quote && !closing && state && state->mode==CodeContext::Mode::Code));
            const bool safeNext=next=='\0' || next==' ' || next=='\t' || next=='\r' || next=='\n' || next==')' || next==']' || next=='}' || next==',' || next==';' || next==':';
            target.pair=a==b && closing && state && state->mode==CodeContext::Mode::Code && safeNext && (!quote || (!CodeContext::word(previous) && previous!='\\'));
            if(target.skip)handled=true;
            else if(target.pair){handled=true;plan.changes.push_back({a,{},std::string{character,closing}});}
            else plan.changes.push_back({a,buffer_.slice(a,b-a),std::string(1,character)});
        }
        targets.push_back(target);
    }
    if(!handled)return false;
    try {
        SmartEditing::finish(plan,cursors_,cursorAnchors_);
        for(std::size_t i=0;i<targets.size();++i) {
            const auto target=targets[i];
            if(target.surround) {
                const auto low=SmartEditing::map(target.begin,plan.changes),high=SmartEditing::map(target.end,plan.changes,false);
                const bool forward=cursors_[i]>cursorAnchors_[i].value_or(cursors_[i]);
                plan.cursors[i]=forward?high:low;plan.anchors[i]=forward?low:high;
            } else {
                plan.anchors[i].reset();
                plan.cursors[i]=target.skip?SmartEditing::map(target.begin,plan.changes)+1:
                    SmartEditing::map(target.begin,plan.changes,false)+1;
            }
        }
        applySmartPlan(std::move(plan));
    } catch(const std::exception& error){emit highlightingStatusChanged(I18n::text(QString::fromUtf8(error.what())));}
    return true;
}

bool EditorWidget::smartBackspace() {
    if(!QSettings().value("editor/autoPairs",true).toBool() || selectionsNeedBackground())return false;
    SmartEditing::Plan plan;bool paired=false;const auto rules=editingRules();
    for(std::size_t i=0;i<cursors_.size();++i) {
        const auto position=cursors_[i];auto [a,b]=cursorSelectionRange(i);
        if(a==b && position && position<buffer_.size()) {
            const auto opening=buffer_.byteAt(position-1),closing=CodeContext::closeFor(opening);
            const auto state=codeContext_.at(buffer_,position-1,rules);
            if(closing && buffer_.byteAt(position)==closing && state && state->mode==CodeContext::Mode::Code) {
                a=position-1;b=position+1;paired=true;
            }
        }
        if(a==b && position){a=lineBoundary(buffer_,buffer_.lineIndexAt(position),position,false,false);if(a==position)a=position-1;}
        if(b>a)plan.changes.push_back({a,buffer_.slice(a,b-a),{}});
    }
    if(!paired)return false;
    try {
        SmartEditing::finish(plan,cursors_,cursorAnchors_);
        for(auto& anchor:plan.anchors)anchor.reset();
        applySmartPlan(std::move(plan));
    } catch(const std::exception& error){emit highlightingStatusChanged(I18n::text(QString::fromUtf8(error.what())));}
    return true;
}

void EditorWidget::smartEnter() {
    const auto newline=SmartEditing::eol(buffer_);
    if(!QSettings().value("editor/autoIndent",true).toBool()){replaceSelection(newline);return;}
    const auto before=cursors_;const auto anchors=cursorAnchors_;const auto rules=editingRules();const auto width=tabWidth_;
    std::vector<bool> code;
    for(std::size_t i=0;i<before.size();++i) {
        const auto position=std::min(before[i],anchors[i].value_or(before[i]));
        const auto state=codeContext_.at(buffer_,position,rules);code.push_back(state && state->mode==CodeContext::Mode::Code);
    }
    runSmartCommand([before,anchors,code,width,newline](const PieceTreeBuffer& buffer,const std::function<void()>& checkpoint) {
        SmartEditing::Plan plan;std::vector<std::size_t> begins,inside;
        for(std::size_t i=0;i<before.size();++i) {
            if(checkpoint)checkpoint();
            const auto a=std::min(before[i],anchors[i].value_or(before[i]));auto b=std::max(before[i],anchors[i].value_or(before[i]));
            const auto row=buffer.lineIndexAt(a),start=LineOperations::contentStart(buffer,row);
            auto indentEnd=start;const auto limit=std::min(a,start+64*1024);CodeContext::Reader reader(buffer);
            while(indentEnd<limit && (reader.at(indentEnd)==' ' || reader.at(indentEnd)=='\t'))++indentEnd;
            const auto indent=buffer.slice(start,indentEnd-start);
            auto left=a;while(left>indentEnd && (reader.at(left-1)==' ' || reader.at(left-1)=='\t'))--left;
            const bool opens=code[i] && left>start && reader.at(left-1)=='{';
            std::string unit(width,' ');if(!indent.empty() && indent.back()=='\t')unit="\t";
            const auto inner=indent+(opens?unit:"");std::string inserted=newline+inner;
            const auto cursor=inserted.size();auto right=b;
            while(right<buffer.lineEnd(buffer.lineIndexAt(b)) && right-b<64*1024 && (reader.at(right)==' ' || reader.at(right)=='\t'))++right;
            if(opens && reader.at(right)=='}'){b=right;inserted+=newline+indent;}
            begins.push_back(a);inside.push_back(cursor);plan.changes.push_back({a,buffer.slice(a,b-a),inserted});
        }
        SmartEditing::finish(plan,before,anchors);
        for(std::size_t i=0;i<before.size();++i){plan.cursors[i]=SmartEditing::map(begins[i],plan.changes,false)+inside[i];plan.anchors[i].reset();}
        return plan;
    });
}
