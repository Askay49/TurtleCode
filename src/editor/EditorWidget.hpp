#pragma once

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QPoint>
#include <QString>
#include <QVector>
#include <QHash>
#include <QMap>
#include <QTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <optional>
#include <atomic>
#include <vector>
#include <map>
#include <set>

#include "core/text/PieceTreeBuffer.hpp"
#include "core/text/EditHistory.hpp"
#include "core/text/LineAlignment.hpp"
#include "SyntaxHighlighter.hpp"
#include "SmartEditing.hpp"
#include "EditorConfig.hpp"
#include "VimController.hpp"
#include "VimMotions.hpp"
#include "VimState.hpp"
#include "VimBlock.hpp"
#include "VimEx.hpp"
#include <deque>
#include "language/SemanticHighlighting.hpp"
#include "language/LspAdaptivePolicy.hpp"
#include "document/Document.hpp"

class QInputMethodEvent;
class QPlainTextEdit;
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
    void initializeSharedView(const EditorWidget& source);
    void copyViewPosition(const EditorWidget& source);
    void loadBuffer(PieceTreeBuffer buffer, const QString& path);
    void restoreBuffer(PieceTreeBuffer buffer, const QString& path);
    void reloadBuffer(PieceTreeBuffer buffer, const QString& path);
    void setVimEnabled(bool enabled);
    bool vimEnabled() const { return vim_.enabled(); }
    VimController::Mode vimMode() const { return vim_.mode(); }
    QString vimModeName() const;
    bool vimCommandLineActive() const {return vimExActive_;}
    void reloadVimMappings();
    void queueVimAutocmd(const QString& event, const QString& path = {});
    bool afterVimAutocmds(std::function<void()> completed);
    void setVimCommandHandlers(VimKeymaps::CommandValidator validator, VimKeymaps::CommandValidator executor);
    bool vimCommandExecuting() const {return vimExExecuting_;}
    bool vimSearchRunning() const {return vimSearchRunning_;}
    bool vimSubstitutionConfirming() const {return vimSubConfirm_;}
    void stopVimExecution() {vimExRemaining_.clear();cancelVimExFrames();cancelVimReplay();}
    bool isVimReplaying() const {return !vimReplay_.empty() || vimReplayScheduled_;}
    void recordVimJump() {pushVimJump();}
    void discardVimBufferMarks();
    void resetVimMode() {if(vim_.enabled()){flushVimMapping();switchVimMode(VimController::Mode::Normal);finishVimChange();}}
    void transferVimReplay(EditorWidget* target);
    void transferVimEx(EditorWidget* target);
    void reportVimExMessage(const QString& message){vimExMessage(message);}
    void applyHistorySettings();
    void applyFontSettings();
    void refreshEditorConfig();
    int indentationSize() const { return editorConfig_.indentation(); }
    int tabSize() const { return tabWidth_; }
    bool insertsSpaces() const { return !editorConfig_.tabs.value_or(false); }
    void prepareEditorConfigSave(const QString& path,std::function<void(bool)> completed);
    void setMiniMapEnabled(bool enabled);
    void setMiniMapSuppressed(bool suppressed);
    bool miniMapEnabled() const { return miniMapEnabled_; }
    [[nodiscard]] int lineNumberAreaWidth() const { return gutterWidth(); }
    void setDebugBreakpoints(const QSet<int>& lines,const QSet<int>& disabled={}) { debugDisabledBreakpoints_=disabled; debugBreakpoints_=lines; viewport()->update(); }
    void setDebugLine(int line) { debugLine_=line; viewport()->update(); }
    struct DiffHighlight { int line, start, length; };
    void setDiffHighlights(const QVector<DiffHighlight>& highlights) {
        diffHighlights_.clear(); for(const auto& h:highlights) diffHighlights_.insert(h.line,{h.start,h.length}); viewport()->update();
    }
    // Visual-only alignment rows: -1 is a spacer, other values are document lines.
    void setDiffAlignment(const LineAlignment& alignment);
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
    void setBusy(bool busy);
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
    void markFileMissing();
    void markDiskDiverged();
    void refreshSyntaxHighlighting();
    void goToLine(int oneBasedLine);
    [[nodiscard]] QPoint navigationPosition() const;
    void setNavigationPosition(int oneBasedLine, int oneBasedColumn);
    void revealMatch(int oneBasedLine, int oneBasedColumn, int utf16Length, bool centerVertically = true);
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
    void vimExInterruptRequested();
    void vimExRequested(const VimEx::Invocation& command, std::function<void(QString)> completed);
    void vimNavigateRequested(const QString& path,int line,int column);
    void vimBufferNavigateRequested(Document* document,int line,int column);
    void vimWorkspaceRequested(const QString& command,int count,bool explicitCount);
    void viewFocused();
    void vimModeChanged();
    void vimCompletionRequested();
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
    struct ExScope {
        bool silent{},ignoreErrors{},keepJumps{},keepPatterns{};
        QString pattern,offset,subPattern;QStringList searchHistory;bool direction{};
    };
    struct ExFrame {
        enum Kind {Global,Normal,Register,ResumeNormal,Commands} kind{Global};
        std::shared_ptr<VimEx::LineMarks> marks;
        QString body,after;QStringList lines;int index{};bool preserveColumn{}, remap{};
        std::optional<ExScope> scope;
        std::shared_ptr<int> autocmdGuard;
    };
    std::vector<ExFrame> vimExFrames_;
    std::optional<ExScope> vimExScope_;
    std::optional<ExScope> vimExParseState_;
    bool vimExNormal_{false};int vimExSteps_{};
    bool vimExSilent() const;
    bool vimExIgnoreErrors() const;
    void restoreVimExScope(std::optional<ExScope>& scope);
    void vimExMessage(const QString& message);
    void beginVimGlobal(const VimEx::Invocation& command);
    void beginVimNormal(const VimEx::Invocation& command);
    bool nextVimExFrame();
    VimEx::Registry vimExRegistry() const;
    QString executeVimUserCommand(const VimEx::Invocation&);
    QString executeVimAutocmd(const VimEx::Invocation&);
    bool vimExHasAutocmd() const;
    void beginVimExProgram(QStringList lines, std::shared_ptr<int> autocmdGuard = {});
    void scheduleVimAutocmd();
    bool startPendingVimAutocmd();
    std::function<void()> vimAfterAutocmds_;
    QStringList vimPendingAutocmds_;
    bool vimAutocmdScheduled_{};
    void editVimExLines(const VimEx::Invocation& command,bool normal=false);
    void readVimExFile(const VimEx::Invocation& command);
    void executeVimExRegister(const VimEx::Invocation& command);
    void projectVimExLines(std::size_t row,std::size_t column,std::size_t removed,std::size_t inserted);
    void cancelVimExFrames();
    void renameVimAnchors(const QString& path);
    QFrame* vimExBar_{nullptr};
    QLineEdit* vimExInput_{nullptr};
    QPlainTextEdit* vimExHistoryWindow_{nullptr};
    VimController vimHistoryMode_;
    bool filterVimCommandHistory(QEvent*);
    void showVimCommandHistory();
    void submitVimCommandLine(const QString&);
    QLabel* vimExHints_{nullptr};
    QLabel* vimExPrompt_{nullptr};
    QChar vimCommandPrompt_{':'};
    VimController::Action vimSearchAction_;
    std::size_t vimSearchOrigin_{},vimLastSearchCursor_{},vimLastSearchPosition_{};
    std::optional<std::size_t> vimSearchOriginAnchor_;
    int vimSearchOriginTop_{},vimSearchOriginLeft_{};
    std::optional<VimSearch::Match> vimLastSearchMatch_;
    std::shared_ptr<VimSearch::Subject> vimSearchSubject_;
    quint64 vimSearchSubjectRevision_{},vimSearchGeneration_{};
    std::shared_ptr<std::atomic<bool>> vimSearchCancelled_;
    QTimer* vimSearchTimer_{nullptr};
    struct VimSearchHighlight {std::size_t begin,end;};
    std::vector<VimSearchHighlight> vimSearchHighlights_;
    bool vimSearchRunning_{false},vimHighlightHidden_{false};
    std::shared_ptr<VimSearch::Plan> vimSubPlan_;
    std::optional<std::size_t> vimSubGlobalLast_;
    bool vimSubConfirm_{false};
    std::size_t vimSubIndex_{},vimSubAccepted_{};
    std::ptrdiff_t vimSubDelta_{};
    quint64 vimSubRevision_{};
    QString vimSubPrintFlags_;
    void startVimSearch(const VimController::Action& action);
    void previewVimSearch();
    void commitVimSearch(const QString& input);
    void navigateVimSearch(const QString& pattern,bool forward,const VimController::Action& action,const QString& offset={},bool repeat=false,std::optional<std::size_t> anchor={});
    void updateVimSearchHighlights();
    void invalidateVimSearch();
    void applyVimSearchMatch(const VimSearch::Match&,const QString& offset,const VimController::Action&,bool forward);
    void beginVimSubstitute(const VimEx::Invocation& command,std::shared_ptr<VimEx::LineMarks> selected={});
    void applyVimSubstitutionPlan(std::size_t from=0,std::ptrdiff_t delta=0);
    void nextVimSubstitution();
    bool answerVimSubstitution(QKeyEvent*);
    void finishVimSubstitution(bool interrupted=false);
    bool vimExActive_{false},vimExExecuting_{false},vimExRegisterPending_{false};
    quint64 vimExGeneration_{};
    QString vimExRemaining_,vimExDraft_,vimExHistoryPrefix_,vimExCompletionBase_;
    QStringList vimExCompletions_;
    int vimExHistoryIndex_{},vimExCompletionIndex_{-1},vimExCompletionSuffix_{};
    void showVimCommandLine(int count=0,QChar prompt=':');
    void closeVimCommandLine();
    void positionVimCommandLine();
    bool filterVimCommandLine(QEvent* event);
    void executeVimCommandLine(const QString& line);
    void continueVimEx();
    void executeVimExParsed(const VimEx::Parsed&);
    void scheduleVimEx(int delay=0);
    void finishVimEx(const QString& error={},bool fatal=false);
    void completeVimEx(int direction);
    void updateVimExHints();
    bool vimOwnsFocus() const;
    VimController vim_;
    QChar vimRegister_;
    bool vimForceNumbered_{false};
    std::optional<VimState::Register> vimPutValue_;
    bool vimVisualBlock_{false},vimBlockInsert_{false},vimBlockChange_{false};
    int vimBlockAnchorColumn_{},vimBlockVirtualColumn_{-1};
    int vimBlockAnchorEnd_{};
    std::size_t vimBlockExitPosition_{};
    bool vimBlockToEnd_{false};
    bool vimCaptureInsertEdit_{false},vimHandlingKey_{false},vimSendingReplay_{false};
    bool vimRepeatingDot_{false};
    std::optional<VimKeymaps::Mode> vimMappingMode() const;
    QString executeVimMappingCommand(const VimEx::Invocation&, const VimKeymaps::Command&);
    bool expandVimAbbreviation(QKeyEvent&);
    void sendVimMappingKey(const VimState::Input&, bool remap);
    void runVimMappingCommand(const QString&, bool record);
    VimKeymaps::CommandValidator vimValidCommand_, vimRunCommand_;
    bool vimBypassMappings_{}, vimRemapReplay_{};
    int vimMappingDepth_{};
    int vimMappingSteps_{};
    int vimMappingTimeout() const;
    void initializeVimOptions();
    void applyVimIndentOptions();
    QVariant vimBufferOption(const QString& name) const;
    QString executeVimSet(const VimEx::Invocation&);
    static QStringList completeVimOptions(const QString& prefix);
    bool vimViewOptionsInitialized_{};
    bool reserveVimMapping(const QKeyEvent&);
    bool handleVimMapping(QKeyEvent&);
    void resolveVimMapping(bool waitForMore);
    void flushVimMapping();
    void processEditorKey(QKeyEvent*);
    QVector<VimState::Input> vimMappingPending_;
    QTimer vimMappingTimer_;
    bool handleVimInsertKey(QKeyEvent*);
    void resumeVimInsertNormal();
    void completeVimInsert(int direction,QChar source={});
    bool vimInsertNormalEnd_{false},vimInsertNormal_{false},vimInsertNormalArmed_{false},vimInsertCtrlX_{false};
    std::size_t vimInsertNormalReplayDepth_{};
    VimController::Mode vimInsertReturn_{VimController::Mode::Insert};
    QString vimLiteral_,vimDigraph_,vimInsertSavedIndent_;
    bool vimInsertJoinMovement_{false},vimInsertJoinedMovement_{false};
    bool vimInsertCtrlG_{false},vimInsertExitAfterPut_{false};
    std::size_t vimPreviousVisualRows_{1},vimPreviousVisualWidth_{1};
    int vimLiteralBase_{0},vimLiteralLimit_{0};
    QChar vimInsertCompletionSource_;
    QStringList vimInsertCandidates_;
    int vimCompletionChangeIndex_{-1};
    int vimInsertCandidate_{-1};std::size_t vimCompletionStart_{},vimCompletionEnd_{};
    QString vimCompletionOriginal_;
    quint64 vimCompletionTextRevision_{};
    VimState::Anchor vimPreviousVisualAnchor_,vimPreviousVisualHead_;
    bool vimPreviousVisualLine_{false},vimPreviousVisualBlock_{false},vimPreviousVisualToEnd_{false};
    int vimPreviousBlockAnchor_{},vimPreviousBlockAnchorEnd_{},vimPreviousBlockHead_{};
    QVector<VimState::Input> vimCommandKeys_,vimChangeKeys_;
    std::shared_ptr<ChunkedFile> vimEditPayload_;
    bool vimChangeActive_{false};std::uint64_t vimChangeBefore_{};
    bool vimChangeTooLarge_{false};
    bool vimChangeInserted_{false};std::string vimInsertedText_;
    struct ReplayFrame {QVector<VimState::Input> inputs;qsizetype index{};int remaining{1};bool exNormal{false};bool insertRepeat{false};bool remap{false};};
    std::vector<ReplayFrame> vimReplay_;
    int vimReplaySteps_{};bool vimReplayCancel_{false},vimReplayScheduled_{false},vimReplayGroupActive_{false};
    void endVimReplayGroup();
    QChar vimReplayRegister_;
    struct ReplaceStep {std::size_t begin;std::string before,after;};
    std::vector<ReplaceStep> vimReplaced_;
    bool beginVimKey(QKeyEvent*);
    void endVimKey();
    void finishVimChange();
    void editVimNormal(const VimController::Action&);
    void transformVimRange(std::size_t begin,std::size_t end,VimController::Command,bool linewise=false);
    void noteVimEdit(std::size_t begin,std::size_t length);
    void setVimEditMarks(std::size_t begin,std::size_t end);
    void commitVimChangeLocation();
    void jumpVimChange(int direction,int count);
    QVector<VimState::Anchor> vimChanges_;int vimChangeIndex_{};
    VimState::Anchor vimPendingChange_;
    bool vimApplyingHistory_{false};
    void beginVimChange();
    void captureVimEdit(std::size_t at,std::string_view removed,std::string_view inserted);
    void replayVim(QVector<VimState::Input> inputs,int count=1,bool insertRepeat=false);
    void continueVimReplay();
    void cancelVimReplay();
    void playVimMacro(QChar name,int count);
    void toggleVimRecording(QChar name={});
    void repeatVimChange(int count,bool explicitCount);
    void applyVimInput(const VimState::Input&);
    void putVimRegister(QChar name,bool before,int count,bool insert=false);
    void storeVimRegister(QChar name,const VimState::Register&,bool yank,bool smallDelete,bool forceNumbered=false,const VimState::Register* appended=nullptr);
    void replaceVimText(const QString& text,bool single=false,int count=1);
    void replaceVimPayload(std::shared_ptr<ChunkedFile> source,int count=1,bool leaveReplace=false);
    void backspaceVimReplace();
    QString vimMarkKey() const;
    VimState::Anchor vimLocation();
    void attachVimAnchors();
    static void projectVimAnchors(Document& document,std::size_t at,std::size_t removed,std::size_t inserted);
    void saveVimMark(QChar name);
    void jumpVimMark(QChar name,bool linewise,VimController::Command operation=VimController::Command::Consume,bool keepJumps=false);
    void applyVimMark(const VimState::Anchor&,bool linewise,VimController::Command,bool keepJumps);
    void pushVimJump();
    void jumpVimHistory(int direction,int count);
    void restoreVimLocation(const VimState::Anchor&,bool linewise=false);
    void persistVimAnchors();
    std::vector<std::pair<std::size_t,std::size_t>> vimBlockRanges() const;
    std::pair<int,int> vimBlockColumns() const;
    void editVimBlock(VimController::Command command,int count=1,const QString& text={});
    void putVimBlock(const VimState::Register&,bool before,int count);
    void drawVimBlock(QPainter&) const;
    std::uint64_t vimHistoryState_{0};
    int vimPreferredX_{-1};
    bool vimVisualLine_{false};
    // A file terminator and an empty logical last line share the same bytes.
    // Keep the loaded Vim EOL convention while edits add/remove blank lines.
    VimController::Command vimLastFind_{VimController::Command::Consume};
    QChar vimFindCharacter_;
    int vimInsertCount_{1};
    bool vimInsertModeToggled_{false},vimSendingInsertRepeat_{false};
    QVector<VimState::Input> vimCountedInsertKeys_;
    std::size_t vimInsertStart_{};
    std::string vimInsertPrefix_;
    bool repeatVimInsert();
    void replayVimHistory(bool redoEdit,int count);
    void resolveVimAction(VimController::Action action);
    void applyVimAction(const VimController::Action&,const VimMotions::Range&);
    void editVimRange(std::size_t begin,std::size_t end,VimController::Command operation,bool linewise);
    void pasteVim(bool before,int count,bool ex=false);
    void indentVim(std::size_t first,std::size_t last,bool unindent,int count=1);
    bool handleVimKey(QKeyEvent* event);
    void switchVimMode(VimController::Mode mode,bool fromEscape=false);
    void normalizeVimCursor();
    void refreshVimPresentation();
    int vimCaretWidth() const;
    std::size_t vimLineStart(std::size_t line) const;
    std::size_t lineContentEnd(std::size_t line) const;
    std::size_t graphemeBoundaryAt(std::size_t position,bool forward) const;
    std::size_t vimSelectionEnd(std::size_t position) const;
    void layoutMiniMap();
    EditorMiniMap* miniMap_{nullptr};
    bool miniMapEnabled_{false};
    bool miniMapSuppressed_{false};
    using BulkStep=std::function<void(int)>;
    using BulkTask=std::function<HistoryResult(PieceTreeBuffer&,EditHistory&,std::shared_ptr<ChunkedFile>&,const BulkStep&)>;
    void replaceSelectionsInBackground(const QString& text,std::shared_ptr<ChunkedFile> source={});
    bool selectionsNeedBackground() const;
    void runBulkOperation(const QString& label,BulkTask task,bool modifies=true, std::function<void()> committed = {},
                          std::function<void(bool)> completed = {});
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
    struct VimFold {
        VimState::Anchor first, last;
        bool closed{true};
    };
    struct WrapBreak { std::size_t byte; int pixel; };
    struct ScreenSpan {
        int firstLine, lastLine, firstRow, lastRow;
        QVector<WrapBreak> breaks;
    };
    struct ScreenSlice {
        int line{};
        std::size_t begin{}, end{};
        int left{}, right{-1};
    };
    std::vector<VimFold> vimFolds_;
    std::vector<std::size_t> previewOpenedFolds_;
    mutable std::vector<ScreenSpan> screenSpans_;
    mutable std::map<int,QVector<WrapBreak>> screenWraps_;
    mutable std::set<int> screenDirtyLines_;
    mutable bool screenWrapFull_{true};
    mutable bool screenLayoutDirty_{true};
    mutable int screenLayoutWidth_{}, screenRowCount_{};
    bool vimWrap_{false}, vimFoldEnabled_{true};
    int vimHalfPage_{}, vimScrollPadding_{}, vimScreenPreferredX_{-1};

    void rebuildScreenLayout() const;
    void rebuildScreenWraps(int width) const;
    void adjustScreenWraps(std::size_t row, std::size_t removed, std::size_t inserted);
    int screenRowCount() const;
    int screenRowForPosition(std::size_t position) const;
    int screenOriginX(std::size_t position) const;
    bool screenLayoutActive() const;
    ScreenSlice screenSlice(int row) const;
    std::size_t positionAtPoint(int x, int y, int line) const;
    std::size_t screenPositionAtX(int row, int x, bool insert = false) const;
    std::pair<int,int> foldLines(const VimFold& fold) const;
    int closedFoldEnd(int line) const;
    bool revealVimFolds(int line);
    void restoreVimPreviewFolds();
    void refreshScreenLayout(int topLine);
    void invalidateScreenMinimap();
    bool handleVimViewport(const VimController::Action& action);
    void createVimFold(int first, int last);
    void editVimFolds(QChar command, int count);
    void moveToVimFold(QChar command, int count);

    void updateScrollBars();
    void ensureCursorVisible(bool centerVertically = false);
    [[nodiscard]] int textAreaWidth() const;
    [[nodiscard]] int cursorPixelColumn() const;
    void setModified(bool modified, bool replaced = false);
    void notifyDocumentChanged(bool modified);
    void refreshSharedView(bool modified, bool replaced, const std::vector<Document::Edit>& edits);
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
    std::pair<QString,bool> occurrenceQuery() const;
    void replaceFindMatch(bool all);
    void clearSelection();
    void clearAllSelections();
    void syncPrimaryCursor();
    void normalizeCursors();
    void addCursorVertically(int direction);
    [[nodiscard]] bool cursorHasSelection(std::size_t index) const;
    [[nodiscard]] std::pair<std::size_t, std::size_t> cursorSelectionRange(std::size_t index) const;
    [[nodiscard]] bool hasSelection() const { return anchor_.has_value() && (*anchor_ != cursorPosition_ || (vim_.enabled() && vim_.mode()==VimController::Mode::Visual && cursorPosition_<buffer_.size())); }
    [[nodiscard]] std::pair<std::size_t, std::size_t> selectionRange() const;

    LineAlignment diffAlignment_;
    QHash<int,QPair<int,int>> diffHighlights_;
    DocumentPtr document_;
    PieceTreeBuffer& buffer_;
    QString& filePath_;
    bool& vimFinalNewline_;
    QSet<int> debugBreakpoints_,debugDisabledBreakpoints_;
    int debugLine_{0};
    std::size_t cursorPosition_{0}; // UTF-8 byte offset
    std::vector<std::size_t> cursors_{0};
    std::vector<std::optional<std::size_t>> cursorAnchors_{std::nullopt};
    std::optional<std::size_t> anchor_;
    std::size_t preferredColumn_{static_cast<std::size_t>(-1)};
    bool modified_{false};
    int tabWidth_{4};
    EditorConfig::Settings editorConfig_;
    QString expandedTabs(const QString& text,int& column) const;
    int textWidth(const QString& text) const;
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
    bool& busy_;
    QString observedPath_;
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
