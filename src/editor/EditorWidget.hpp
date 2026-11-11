#pragma once

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QPoint>
#include <QString>
#include <QVector>
#include <QHash>
#include <QMap>
#include <QJsonArray>
#include <QJsonObject>
#include <optional>
#include <atomic>
#include <vector>

#include "core/text/PieceTreeBuffer.hpp"
#include "core/text/EditHistory.hpp"
#include "SyntaxHighlighter.hpp"
#include "SmartEditing.hpp"
#include "language/SemanticHighlighting.hpp"
#include "language/LspAdaptivePolicy.hpp"
#include "document/Document.hpp"

class QInputMethodEvent;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QContextMenuEvent;
class QPaintEvent;
class QResizeEvent;
class QRect;
class QVariant;
class QFrame;
class QLineEdit;
class QLabel;
class QToolButton;
class QTimer;
class QCompleter;
class ChunkedFile;
struct HighlightEdit;

class EditorMiniMap;

class EditorWidget final : public QAbstractScrollArea {
    Q_OBJECT
public:
    ~EditorWidget() override;
    explicit EditorWidget(QWidget* parent = nullptr, DocumentPtr document = std::make_shared<Document>());
    [[nodiscard]] DocumentPtr document() const { return document_; }
    void loadBuffer(PieceTreeBuffer buffer, const QString& path);
    void restoreBuffer(PieceTreeBuffer buffer, const QString& path);
    void reloadBuffer(PieceTreeBuffer buffer, const QString& path);
    void applyHistorySettings();
    void applyFontSettings();
    void setMiniMapEnabled(bool enabled);
    bool miniMapEnabled() const { return miniMapEnabled_; }
    [[nodiscard]] int lineNumberAreaWidth() const { return gutterWidth(); }
    void setDebugBreakpoints(const QSet<int>& lines,const QSet<int>& disabled={}) { debugDisabledBreakpoints_=disabled; debugBreakpoints_=lines; viewport()->update(); }
    void setDebugLine(int line) { debugLine_=line; viewport()->update(); }
    struct DiffHighlight { int line, start, length; };
    void setDiffHighlights(const QVector<DiffHighlight>& highlights) {
        diffHighlights_.clear(); for(const auto& h:highlights) diffHighlights_.insert(h.line,{h.start,h.length}); viewport()->update();
    }
    // Visual-only alignment rows: -1 is a spacer, other values are document lines.
    void setDiffAlignment(const QVector<int>& rows);
    int visualRowForLine(int line) const;
    int documentLineForVisualRow(int row) const;
    QPoint selectedLineRange() const {
        const auto [begin,end]=selectionRange();
        return {int(buffer_.lineIndexAt(hasSelection()?begin:cursorPosition_)),
                int(buffer_.lineIndexAt(hasSelection() && end>begin?end-1:cursorPosition_))};
    }
    void pasteClipboard();
    void documentUndo(bool redoEdit = false) { if (!busy_) { if (redoEdit) redo(); else undo(); } }
    void toggleLineComment();
    void toggleBlockComment();
    void indentSelection(bool unindent = false);
    void copyLines(int direction);
    void moveLines(int direction);
    void copySelection(bool cut = false);
    void completeAsyncSave(const QString& path);
    void setBusy(bool busy) noexcept { busy_ = busy; if (busy) hideCompletions(); }
    [[nodiscard]] bool isBusy() const noexcept { return busy_; }
    bool findText(const QString& query, bool forward = true,
                  Qt::CaseSensitivity sensitivity = Qt::CaseSensitive);
    int replaceAllText(const QString& query, const QString& replacement,
                       Qt::CaseSensitivity sensitivity = Qt::CaseSensitive);
    [[nodiscard]] PieceTreeBuffer snapshotBuffer() const { return buffer_.snapshot(); }
    bool loadFile(const QString& path);
    bool saveFile(const QString& path);
    void updateFilePathAfterRename(const QString& path);
    void detachDeletedFile();
    void refreshSyntaxHighlighting();
    void goToLine(int oneBasedLine);
    [[nodiscard]] QPoint navigationPosition() const;
    void setNavigationPosition(int oneBasedLine, int oneBasedColumn);
    void revealMatch(int oneBasedLine, int oneBasedColumn, int utf16Length, bool centerVertically = false);
    void showFindBar(bool replace = false);
    void findNext(bool backward = false);
    void selectNextOccurrence();
    [[nodiscard]] QString filePath() const { return filePath_; }
    [[nodiscard]] bool isModified() const noexcept { return modified_; }
    [[nodiscard]] std::size_t cursorCount() const noexcept { return cursors_.size(); }
    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] QString lspText() const;
    [[nodiscard]] bool lspEligible() const;
    bool lspAutomaticEligible() const { return lspEligible() && LspAdaptivePolicy::automaticDocument(buffer_.size()); }
    std::size_t byteSize() const { return buffer_.size(); }
    LspAdaptivePolicy& lspPolicy() { return lspPolicy_; }
    QJsonObject visibleLspRange() const;
    QString lspRangeText(int first,int last) const;
    [[nodiscard]] QPoint lspPosition() const;
    [[nodiscard]] QPoint cursorScreenPosition() const;
    void applyCompletion(const QJsonObject& item);
    void showCompletions(const QJsonArray& items);
    void hideCompletions();
    quint64 lspRevision() const { return lspRevision_; }
    bool shouldAutoComplete() const;
    bool applyingCompletion() const { return applyingCompletion_; }
    void setDiagnostics(const QJsonArray& items);
    void setSemanticHighlighting(SemanticHighlightResult result,quint64 revision,quint64 generation);
    void clearSemanticHighlighting();
    quint64 semanticGeneration() const { return semanticGeneration_; }
    int semanticTokenCount() const;
    QHash<int,QVector<SemanticToken>> semanticSnapshot() const { return semanticLines_; }
    bool validateWorkspaceEdits(const QByteArray& expected, const std::vector<EditChange>& changes) const;
    // Caller must validate the whole workspace transaction before applying any file.
    void applyWorkspaceEdits(std::vector<EditChange> changes);

signals:
    void debugAnchorsMoved(const QMap<int,int>& anchors);
    void debugLinesEdited(int row,int column,int removed,int inserted);
    void breakpointToggled(int oneBasedLine);
    void gutterWidthChanged();
    void semanticViewportChanged();
    void highlightingStatusChanged(const QString& message);
    void modifiedChanged();
    void contentChanged();
    void codeContextMenuRequested(const QPoint& globalPosition);
    void hoverRequested(int line, int utf16Column, const QPoint& globalPosition);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    friend class EditorMiniMap;
    void layoutMiniMap();
    EditorMiniMap* miniMap_{nullptr};
    bool miniMapEnabled_{false};
    using BulkStep=std::function<void(int)>;
    using BulkTask=std::function<HistoryResult(PieceTreeBuffer&,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep&)>;
    void replaceSelectionsInBackground(const QString& text,std::shared_ptr<ChunkedFile> source={});
    bool selectionsNeedBackground() const;
    void runBulkOperation(const QString& label,BulkTask task,bool modifies=true, std::function<void()> committed = {});
    void editLines(int direction, bool copy);
    void applySmartPlan(SmartEditing::Plan plan);
    using SmartBuilder=std::function<SmartEditing::Plan(const PieceTreeBuffer&,const std::function<void()>&)>;
    void runSmartCommand(SmartBuilder builder);
    bool smartCharacter(char character);
    bool smartBackspace();
    void smartEnter();
    CodeContext::Rules editingRules() const;
    void updateBracketMatch();
    CodeContext::Cache codeContext_;
    CodeContext::Match bracketMatch_;
    QTimer* bracketTimer_{nullptr};
    quint64 bracketRevision_{~quint64(0)};
    std::size_t bracketCursor_{static_cast<std::size_t>(-1)};
    QString bracketPath_;

    void replaceAllInBackground(const QString& replacement);
    LspAdaptivePolicy lspPolicy_;
    QHash<int,QVector<SemanticToken>> semanticLines_;
    quint64 semanticGeneration_{0};
    bool semanticAdjusted_{false};
    void trackHighlightEdit(std::size_t offset,std::size_t row,std::size_t column,
                            std::string_view removed,std::string_view inserted);
    void invalidateSemanticLines(std::size_t firstLine,std::size_t byteColumn,std::string_view removed,std::string_view inserted);
    QCompleter* completer_{nullptr};
    quint64 lspRevision_{0};
    quint64 completionRevision_{0};
    QPoint completionPosition_;
    bool applyingCompletion_{false};
    void insertText(const std::string& text, EditKind kind = EditKind::Insert);
    void deleteBackward(bool byWord = false);
    void deleteForward(bool byWord = false);
    void undo();
    void redo();
    void replaceSelection(const std::string& text);
    void replaceRange(std::size_t begin, std::size_t length, const std::string& text);
    void applyBatchEdits(std::vector<EditChange> changes,
                         const std::vector<std::size_t>& cursorsBefore,
                         const std::vector<std::size_t>& cursorsAfter);
    [[nodiscard]] std::size_t previousGraphemePosition() const;
    [[nodiscard]] std::size_t nextGraphemePosition() const;
    [[nodiscard]] std::size_t previousWordPosition() const;
    [[nodiscard]] std::size_t nextWordPosition() const;
    [[nodiscard]] QRect inputCursorRect() const;
    void adjustDiffHighlights(const HighlightEdit& edit);
    void adjustDiffAlignment(std::size_t firstLine, int removedLines, int insertedLines);
    void updateScrollBars();
    void ensureCursorVisible();
    [[nodiscard]] int textAreaWidth() const;
    [[nodiscard]] int cursorPixelColumn() const;
    void setModified(bool modified);
    int lineHeight() const;
    int gutterWidth() const;
    int lineAtY(int y) const;
    std::size_t byteColumnAtX(int x, int line) const;
    std::size_t currentLine() const;
    void moveCursor(std::size_t position, bool keepAnchor);
    void selectLineRange(std::size_t begin, std::size_t end);
    [[nodiscard]] std::size_t lineSelectionEnd(std::size_t line) const;
    const QString& cachedLine(std::size_t line) const;
    void invalidateLineCache(std::size_t changedFrom = 0);
    void rebuildHitTestCache(int line) const;
    void positionFindBar();
    void updateFindMatches();
    void navigateFind(int direction);
    void replaceFindMatch(bool all);
    void clearSelection();
    void clearAllSelections();
    void syncPrimaryCursor();
    void normalizeCursors();
    void addCursorVertically(int direction);
    [[nodiscard]] bool cursorHasSelection(std::size_t index) const;
    [[nodiscard]] std::pair<std::size_t, std::size_t> cursorSelectionRange(std::size_t index) const;
    [[nodiscard]] bool hasSelection() const { return anchor_.has_value() && *anchor_ != cursorPosition_; }
    [[nodiscard]] std::pair<std::size_t, std::size_t> selectionRange() const;

    QVector<int> diffRows_, diffLineRows_;
    QHash<int,QPair<int,int>> diffHighlights_;
    DocumentPtr document_;
    PieceTreeBuffer& buffer_;
    QString& filePath_;
    QSet<int> debugBreakpoints_,debugDisabledBreakpoints_;
    int debugLine_{0};
    std::size_t cursorPosition_{0}; // UTF-8 byte offset
    std::vector<std::size_t> cursors_{0};
    std::vector<std::optional<std::size_t>> cursorAnchors_{std::nullopt};
    std::optional<std::size_t> anchor_;
    std::size_t preferredColumn_{static_cast<std::size_t>(-1)};
    bool modified_{false};
    int tabWidth_{4};
    EditHistory& history_;
    SyntaxHighlighter highlighter_;
    QElapsedTimer editClock_;
    QString preeditText_;
    int preeditCursor_{0};
    bool draggingSelection_{false};
    bool draggingWords_{false};
    bool draggingLines_{false};
    std::size_t wordSelectionBegin_{0};
    std::size_t wordSelectionEnd_{0};
    std::size_t lineSelectionBegin_{0};
    std::size_t lineSelectionEnd_{0};
    QElapsedTimer tripleClickClock_;
    QPoint lastDoubleClickPoint_;
    bool busy_{false};
    bool horizontalExtentDirty_{true};
    std::size_t cachedHorizontalLine_{static_cast<std::size_t>(-1)};
    int cachedHorizontalWidth_{0};
    int cachedGutterWidth_{-1};
    mutable QHash<qulonglong, QString> lineCache_;
    mutable int hitTestLine_{-1};
    mutable QVector<std::size_t> hitTestOffsets_;
    mutable QVector<int> hitTestColumns_;
    struct Diagnostic { int start; int end; int severity; QString message; };
    QHash<int, QVector<Diagnostic>> diagnostics_;
    struct FindMatch { std::size_t begin; std::size_t end; };
    std::vector<FindMatch> findMatches_;
    int activeFindMatch_{-1};
    QFrame* findBar_{nullptr};
    QToolButton* findReplaceToggle_{nullptr};
    QWidget* findReplaceRow_{nullptr};
    QLineEdit* findInput_{nullptr};
    QLineEdit* findReplaceInput_{nullptr};
    QLabel* findCount_{nullptr};
    QToolButton* findCase_{nullptr};
    QToolButton* findWord_{nullptr};
    QToolButton* findRegex_{nullptr};
    QToolButton* findReplaceAll_{nullptr};
    QTimer* findTimer_{nullptr};
    quint64 findGeneration_{0};
    std::shared_ptr<std::atomic<bool>> findCancelled_;
};
