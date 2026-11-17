#include "VimController.hpp"
#include <QKeyEvent>
QString VimController::modeName() const {
    QString name;
    switch(mode_) {
    case Mode::Normal: name=QStringLiteral("NORMAL");break;
    case Mode::Insert: name=QStringLiteral("INSERT");break;
    case Mode::Replace: name=QStringLiteral("REPLACE");break;
    case Mode::Visual: name=QStringLiteral("VISUAL");break;
    }
    return pending_.isEmpty()?name:name+QStringLiteral(" ")+pending_;
}
VimController::Command VimController::command(const QKeyEvent& event) const {
    if(!enabled_)return Command::Pass;
    const auto modifiers=event.modifiers();
    if((event.key()==Qt::Key_Escape && modifiers==Qt::NoModifier) ||
       (event.key()==Qt::Key_BracketLeft && modifiers==Qt::ControlModifier))return Command::Normal;
    if(acceptsText()) {
        if(event.key()==Qt::Key_C && modifiers==Qt::ControlModifier)return Command::Normal;
        if((event.key()==Qt::Key_At || event.key()==Qt::Key_Space) && modifiers==Qt::ControlModifier)return Command::Key;
        if(modifiers==Qt::ControlModifier && event.key()>=Qt::Key_A && event.key()<=Qt::Key_Z && QStringLiteral("GHIJKMWUDTONPYEVQXLA").contains(QChar(event.key())))return Command::Key;
        if(event.key()==Qt::Key_R && modifiers==Qt::ControlModifier)return Command::InsertRegister;
        if(event.key()==Qt::Key_Insert && modifiers==Qt::NoModifier)return mode_==Mode::Replace?Command::Insert:Command::ReplaceMode;
        return Command::Pass;
    }
    if(event.key()==Qt::Key_V && modifiers==Qt::ControlModifier)return Command::VisualBlock;
    if(event.key()==Qt::Key_O && modifiers==Qt::ControlModifier)return Command::JumpBack;
    if((event.key()==Qt::Key_I && modifiers==Qt::ControlModifier) || (event.key()==Qt::Key_Tab && modifiers==Qt::NoModifier))return Command::JumpForward;
    if(event.key()==Qt::Key_R && modifiers==Qt::ControlModifier)return Command::Redo;
    if(event.key()==Qt::Key_A && modifiers==Qt::ControlModifier)return Command::Increment;
    if(event.key()==Qt::Key_X && modifiers==Qt::ControlModifier)return Command::Decrement;
    // Reserve native text-mutating shortcuts for future Vim bindings. Native
    // application shortcuts (save, palette, search, etc.) retain their scope.
    if(modifiers==Qt::ControlModifier && (event.key()==Qt::Key_V || event.key()==Qt::Key_X))return Command::Consume;
    if(modifiers & (Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))return Command::Pass;
    switch(event.key()) {
    case Qt::Key_Return:case Qt::Key_Enter:return Command::Down;
    case Qt::Key_Left: return Command::Left;
    case Qt::Key_Right: return Command::Right;
    case Qt::Key_Up: return Command::Up;
    case Qt::Key_Down: return Command::Down;
    case Qt::Key_Home: return Command::Start;
    case Qt::Key_End: return Command::End;
    default: break;
    }
    if(!(modifiers & Qt::ShiftModifier))switch(event.key()) {
    case Qt::Key_H: return Command::Left;
    case Qt::Key_L: return Command::Right;
    case Qt::Key_K: return Command::Up;
    case Qt::Key_J: return Command::Down;
    case Qt::Key_V: return prefixG_?Command::Key:mode_==Mode::Visual?Command::Normal:Command::Visual;
    case Qt::Key_I: return mode_==Mode::Normal?Command::Insert:Command::Consume;
    case Qt::Key_A: return mode_==Mode::Normal?Command::Append:Command::Consume;
    default: break;
    }
    if(event.text().size()==1 && event.text().at(0).unicode()<128 && event.text().at(0).isPrint())return Command::Key;
    // Unknown Normal/Visual commands must never fall through to text input.
    if(!event.text().isEmpty() || event.key()==Qt::Key_Tab || event.key()==Qt::Key_Backtab ||
       event.key()==Qt::Key_Return || event.key()==Qt::Key_Enter || event.key()==Qt::Key_Backspace || event.key()==Qt::Key_Delete)
        return Command::Consume;
    return Command::Pass;
}

void VimController::resetPending() {
    count_=0;operatorCount_=1;operation_=waiting_=Command::Consume;
    around_=prefixG_=explicitCount_=false;bracketPrefix_={};
    pending_.clear();selectedRegister_={};
}
VimController::Action VimController::feed(const QKeyEvent& event) {
    using C=Command;
    const auto key=command(event);
    if(acceptsText() && waiting_==C::InsertRegister && key==C::Normal){resetPending();return {C::Consume};}
    if(acceptsText() && waiting_==C::InsertRegister && key!=C::Normal && !event.text().isEmpty()) {
        Action action;action.command=C::InsertRegister;action.character=event.text().at(0);resetPending();return action;
    }
    // Qt delivers modifier presses separately from the following shortcut.
    // Keep a count alive through Ctrl so 2<Ctrl+R> reaches the redo command.
    if(key==C::Pass && (event.key()==Qt::Key_Control || event.key()==Qt::Key_Shift || event.key()==Qt::Key_Alt || event.key()==Qt::Key_Meta))return {C::Pass};
    if(key==C::Pass || key==C::Normal) {resetPending();return {key};}
    const auto character=event.text().isEmpty()?QChar{}:event.text().at(0);
    auto finish=[&](C cmd,QChar arg=QChar{}) {
        Action action{cmd,operation_,std::min(10000,std::max(1,count_)*operatorCount_),explicitCount_,around_,arg};
        action.registerName=selectedRegister_;action.text=event.text();resetPending();return action;
    };
    if(waiting_!=C::Consume) {
        pending_+=event.text();
        if(waiting_==C::SelectRegister){
            if(!QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\"-_*+.%#/: ").contains(character) || character==' '){resetPending();return {C::Consume};}
            selectedRegister_=character;waiting_=C::Consume;return {C::Pending};
        }
        const auto cmd=waiting_;const bool keep=prefixG_ && (cmd==C::JumpMark || cmd==C::JumpMarkLine);auto action=finish(cmd,character);if(cmd==C::JumpMark || cmd==C::JumpMarkLine)action.around=keep;return action;
    }
    if(key==C::Increment || key==C::Decrement){const bool progressive=prefixG_;auto action=finish(key);action.around=progressive;return action;}
    if(key==C::Redo || key==C::VisualBlock || key==C::JumpBack || key==C::JumpForward || key==C::ReplaceMode || (key==C::Insert && mode_!=Mode::Normal))return finish(key);
    if(key==C::InsertRegister){waiting_=C::InsertRegister;pending_+=QStringLiteral("^R");return {C::Pending};}
    pending_+=event.text();
    if(character=='"'){waiting_=C::SelectRegister;return {C::Pending};}
    if((character=='q' && !prefixG_ && operation_!=C::Format) || character=='@' || character=='m' || character=='\'' || character=='`' || character=='r') {
        waiting_=character=='q'?C::RecordMacro:character=='@'?C::PlayMacro:character=='m'?C::SetMark:character=='\''?C::JumpMarkLine:character=='`'?C::JumpMark:C::ReplaceCharacter;return {C::Pending};
    }
    if(character>='0' && character<='9' && (character!='0' || count_)) {
        count_=std::min(10000,count_*10+character.digitValue());explicitCount_=true;return {C::Pending};
    }
    const auto operatorKey=[&](C op){
        if(mode_==Mode::Visual)return finish(op);
        if(operation_==op)return finish(C::Line);
        if(operation_!=C::Consume)return finish(C::Consume);
        operation_=op;operatorCount_=std::max(1,count_);count_=0;return Action{C::Pending};
    };
    if(!bracketPrefix_.isNull()){
        const bool forward=bracketPrefix_==']';
        if(character=='[' || character==']')return finish(forward?C::SectionNext:C::SectionBack,character);
        if((forward && (character==')' || character=='}')) || (!forward && (character=='(' || character=='{')))return finish(forward?C::UnclosedNext:C::UnclosedBack,character);
        return finish(C::Consume);
    }
    if(prefixG_){
        prefixG_=false;
        if(character=='v')return finish(C::RestoreVisual);
        if(character=='i')return finish(C::InsertPrevious);
        if(character=='I')return finish(C::InsertZero);
        if(character=='n' || character=='N'){auto a=finish(C::SelectMatch);a.around=character=='N';return a;}
        if(character=='u' || character=='U' || character=='~' || character=='q')return operatorKey(character=='u'?C::LowerCase:character=='U'?C::UpperCase:character=='~'?C::ToggleCase:C::Format);
        if(character=='J'){auto action=finish(C::Join);action.around=true;return action;}
        auto action=finish(character=='g'?C::FirstLine:character=='e'?C::WordEndBack:character=='E'?C::BigWordEndBack:character==';'?C::ChangeBack:character==','?C::ChangeForward:character=='_'?C::LastText:character=='*'?C::SearchWord:character=='#'?C::SearchWordBack:character=='&'?C::RepeatSubstitute:C::Consume);action.around=true;return action;
    }
    const auto op=[&](QChar c) {
        switch(c.unicode()){case 'd':return C::Delete;case 'c':return C::Change;case 'y':return C::Yank;case '>':return C::Indent;case '<':return C::Unindent;case '=':return C::AutoIndent;default:return C::Consume;}
    }(character);
    if(op!=C::Consume)return operatorKey(op);
    if((operation_==C::LowerCase && character=='u') || (operation_==C::UpperCase && character=='U') || (operation_==C::ToggleCase && character=='~') || (operation_==C::Format && character=='q'))return finish(C::Line);
    if((operation_!=C::Consume || mode_==Mode::Visual) && (character=='i' || character=='a')) {
        waiting_=C::Object;around_=character=='a';return {C::Pending};
    }
    if(character=='g'){prefixG_=true;return {C::Pending};}
    if(character=='[' || character==']'){bracketPrefix_=character;return {C::Pending};}
    if(character=='f' || character=='F' || character=='t' || character=='T') {
        waiting_=character=='f'?C::Find:character=='F'?C::FindBack:character=='t'?C::Till:C::TillBack;return {C::Pending};
    }
    C cmd=key==C::Key?C::Consume:key;
    switch(character.unicode()) {
    case 'h':cmd=C::Left;break;case 'j':cmd=C::Down;break;case 'k':cmd=C::Up;break;case 'l':cmd=C::Right;break;
    case 'w':cmd=C::Word;break;case 'b':cmd=C::WordBack;break;case 'e':cmd=C::WordEnd;break;
    case 'W':cmd=C::BigWord;break;case 'B':cmd=C::BigWordBack;break;case 'E':cmd=C::BigWordEnd;break;
    case '(':cmd=C::SentenceBack;break;case ')':cmd=C::SentenceNext;break;
    case '+':cmd=C::LineNext;break;case '-':cmd=C::LineBack;break;case '_':cmd=C::LineOffset;break;case '|':cmd=C::Column;break;
    case '0':cmd=C::Start;break;case '^':cmd=C::FirstText;break;case '$':cmd=C::End;break;
    case 'G':cmd=C::LastLine;break;case '{':cmd=C::ParagraphBack;break;case '}':cmd=C::ParagraphNext;break;
    case ';':cmd=C::RepeatFind;break;case ',':cmd=C::ReverseFind;break;case '%':cmd=C::Match;break;
    case '/':cmd=C::SearchForward;break;case '?':cmd=C::SearchBackward;break;
    case 'n':cmd=C::SearchNext;break;case 'N':cmd=C::SearchPrevious;break;
    case '*':cmd=C::SearchWord;break;case '#':cmd=C::SearchWordBack;break;
    default:break;
    }
    if(operation_!=C::Consume)return finish(cmd);
    switch(character.unicode()) {
    case 'U':if(mode_==Mode::Visual)cmd=C::UpperCase;break;
    case 'J':cmd=C::Join;break;case '~':cmd=C::ToggleCase;break;
    case '&':cmd=C::RepeatSubstitute;break;
    case ':':cmd=C::CommandLine;break;
    case 'i':cmd=mode_==Mode::Normal?C::Insert:C::Consume;break;
    case 'a':cmd=mode_==Mode::Normal?C::Append:C::Consume;break;
    case 'I':cmd=C::InsertFirst;break;case 'A':cmd=C::InsertLast;break;
    case 'o':cmd=C::OpenBelow;break;case 'O':cmd=C::OpenAbove;break;
    case 'v':cmd=mode_==Mode::Visual?C::Normal:C::Visual;break;case 'V':cmd=C::VisualLine;break;
    case 'x':cmd=C::DeleteChar;break;case 'X':cmd=C::DeleteBefore;break;
    case 's':cmd=C::Substitute;break;case 'S':cmd=C::ChangeLine;break;
    case 'D':cmd=C::DeleteEnd;break;case 'C':cmd=C::ChangeEnd;break;case 'Y':cmd=C::YankLine;break;
    case 'p':cmd=C::PasteAfter;break;case 'P':cmd=C::PasteBefore;break;
    case 'u':cmd=mode_==Mode::Visual?C::LowerCase:C::Undo;break;case '.':cmd=C::RepeatChange;break;case 'R':cmd=C::ReplaceMode;break;
    default:break;
    }
    return finish(cmd);
}
