#pragma once
#include <QString>
class QKeyEvent;

// Modal input state only. Document edits, selections and Undo remain editor-owned.
class VimController final {
public:
    enum class Mode { Normal, Insert, Visual, Replace };
    enum class Command { Pass, Consume, Normal, Insert, Append, Visual, Left, Right, Up, Down, Start, End,
        Key, Pending, FirstText, Word, WordBack, WordEnd, BigWord, BigWordBack, BigWordEnd,
        FirstLine, LastLine, ParagraphNext, ParagraphBack, Find, FindBack, Till, TillBack,
        RepeatFind, ReverseFind, Match, Line, Object, Delete, Change, Yank, Indent, Unindent,
        InsertFirst, InsertLast, OpenBelow, OpenAbove, DeleteChar, DeleteBefore, Substitute,
        DeleteEnd, ChangeEnd, ChangeLine, YankLine, PasteAfter, PasteBefore, Undo, Redo, VisualLine,
        VisualBlock, ReplaceMode, ReplaceCharacter, RepeatChange, SelectRegister, InsertRegister,
        RecordMacro, PlayMacro, SetMark, JumpMark, JumpMarkLine, JumpBack, JumpForward, CommandLine, CommandHistory,
        SearchForward,SearchBackward,SearchNext,SearchPrevious,SearchWord,SearchWordBack,RepeatSubstitute,
        Join,ToggleCase,LowerCase,UpperCase,AutoIndent,Format,Increment,Decrement,
        RestoreVisual,InsertZero,InsertPrevious,SelectMatch,WordEndBack,BigWordEndBack,SentenceNext,SentenceBack,SectionNext,SectionBack,
        UnclosedNext,UnclosedBack,ChangeBack,ChangeForward,LineNext,LineBack,LineOffset,Column,LastText, ScreenHigh, ScreenMiddle, ScreenLow, ScreenDown, ScreenUp,
        ScreenStart, ScreenFirstText, ScreenEnd, ScreenMiddleColumn, Scroll, ZCommand, Fold, FoldJump, Workspace };
    struct Action {
        Command command{Command::Consume}, operation{Command::Consume};
        int count{1};bool explicitCount{false}, around{false};QChar character;
        bool repeatFind{false};
        QChar registerName;QString text;int tabWidth{4};
    };
    bool enabled() const { return enabled_; }
    Mode mode() const { return mode_; }
    bool acceptsText() const { return !enabled_ || mode_ == Mode::Insert || mode_==Mode::Replace; }
    void setEnabled(bool enabled) { enabled_ = enabled; mode_ = Mode::Normal; resetPending(); }
    void setMode(Mode mode) { mode_ = enabled_ ? mode : Mode::Normal; resetPending(); }
    QString modeName() const;
    Command command(const QKeyEvent& event) const;
    Action feed(const QKeyEvent& event);
    void resetPending();
    bool hasPending() const {return !pending_.isEmpty() || waiting_!=Command::Consume;}
    bool allowsMapping() const { return waiting_ == Command::Consume && !prefixG_ && bracketPrefix_.isNull(); }
    bool operatorPending() const { return operation_ != Command::Consume; }
    bool reserves(const QKeyEvent& event) const { return command(event) != Command::Pass; }
private:
    bool enabled_{false};
    Mode mode_{Mode::Normal};
    int count_{0}, operatorCount_{1};
    Command operation_{Command::Consume}, waiting_{Command::Consume};
    bool around_{false}, prefixG_{false}, explicitCount_{false};
    QChar bracketPrefix_;
    QString pending_;
    QChar selectedRegister_;
};
