# Vim mode — stages 1–10

Enable **Settings… → Editor → Enable Vim mode / Настройки… → Редактор → Включить режим Vim**, or search for `Vim`
in the command palette (**Ctrl+Shift+P**, command ID `settings.vimMode`).
The same option is available in Editor settings. It is off by default and saved
under `editor/vimMode` in QSettings. Changing it applies to all open windows;
each editor keeps its own current mode. Newly opened documents start in Normal.

The status bar shows **NORMAL**, **INSERT**, **VISUAL**, **V-LINE**, **V-BLOCK**
or **REPLACE** for the current editor. Normal and Visual use a block cursor;
Insert uses a thin cursor and Replace uses an underline.
The indicator dims when keyboard focus leaves the editor. Pending counts and
operators are shown beside the mode, for example `NORMAL 2d`.

| Key | Action |
| --- | --- |
| `i` in Normal | Enter Insert before the current character |
| `a` in Normal | Enter Insert after the current character |
| `Esc` or `Ctrl+[` | Return to Normal; leaving Insert moves one character left within the line |
| `v` in Normal | Start characterwise Visual selection, including the current character |
| `v` in Visual | Return to Normal and clear the selection |
| `V` | Select complete lines; `v` returns to characterwise Visual |
| `o` / `O` in Visual | Swap the active end of the selection |
| `h`, `j`, `k`, `l` or arrows in Normal/Visual | Move left, down, up, right |
| `Home`, `End` in Normal/Visual | Move to the first/last character of the line |

Horizontal modal movement stops at line boundaries. Movement and selection use
grapheme boundaries, including emoji and combining marks; CRLF is preserved.
Visual selection includes both endpoints and can be copied with **Ctrl+C**.
Disabling Vim while a Visual selection is active preserves the selected text
as an ordinary editor selection.

Insert retains the editor's typing, smart editing, completion, clipboard and
IME behavior. Escape closes an active completion popup first; a second Escape
leaves Insert. Search inputs, the command palette, dialogs and terminal keep
their normal keyboard handling. Returning focus to an editor preserves its mode.
Native save, undo/redo, search and other application commands remain available.
Native edits end Visual mode; saving alone preserves it. Undo uses the existing
editor history and does not restore Vim mode or Visual anchors.

## Motions and operators

| Keys | Action |
| --- | --- |
| `w`, `b`, `e` | Next word, previous word, end of word; punctuation forms separate words |
| `W`, `B`, `E` | The same motions for whitespace-separated WORDs |
| `ge`, `gE` | Previous word/WORD end; can follow an operator |
| `0`, `^`, `$` | Line start, first nonblank character, line end |
| `gg`, `G`, `42G`, `42gg` | First line, last line, a numbered line |
| `{`, `}` | Previous/next paragraph boundary |
| `(`, `)` | Previous/next sentence boundary |
| `[[`, `]]`, `[]`, `][` | Section starts/ends, including column-one braces and default nroff section macros |
| `[(`, `[{`, `])`, `]}` | Previous/next unmatched parenthesis or brace |
| `+`, `-`, `_` | First nonblank on a following/preceding line; `_` uses count minus one |
| `42\|`, `g_`, `50%` | Screen column, last nonblank, percentage through the buffer |
| `f<char>`, `F<char>` | Find a character forwards/backwards on the current line |
| `t<char>`, `T<char>` | Stop before/after a character on the current line |
| `;`, `,` | Repeat/reverse the last character search |
| `%` | Jump between matching `()`, `[]`, `{}`; quoted text is skipped while matching |

Combine **`d`** (delete), **`c`** (change and enter Insert), **`y`** (copy),
**`>`** (indent) or **`<`** (unindent) with a motion or text object.
Examples: `dw`, `cw`, `y$`, `dG`, `>j`, `diw`, `ci"`, `dap`.
Double the operator to act on complete lines: `dd`, `cc`, `yy`, `>>`, `<<`.
These operators also act on Visual selections. Indentation follows EditorConfig.

Counts work before motions and operators, and multiply when supplied in both
positions: `3w`, `2dd`, `2d3w`. `3>>` indents three lines once; `3>` in Visual
indents the selection three times. Counts are bounded at 10,000.
Escape, an invalid sequence or moving focus to another input cancels a pending
command without modifying the document.

Supported text objects use `i` (inside) or `a` (around):

| Objects | Meaning |
| --- | --- |
| `iw` / `aw`, `iW` / `aW` | Word/WORD, with surrounding whitespace for `a` |
| `i"` / `a"`, `i'` / `a'`, ``i` `` / ``a` `` | Quoted text on the current line; escaped quotes are respected |
| `i(` / `a(`, `i[` / `a[`, `i{` / `a{`, `i<` / `a<` | Nested paired delimiters; closing-delimiter aliases also work |
| `ib` / `ab`, `iB` / `aB` | Parentheses/braces |
| `ip` / `ap` | Paragraph, including adjacent empty lines for `a` |

Objects can be used in Visual mode too, for example `viw`. Delimiter objects are
textual and do not depend on an installed language grammar. Tags and sentence
objects are not implemented.

## Editing and Undo

| Keys | Action |
| --- | --- |
| `I`, `A` | Insert at first nonblank character / end of line |
| `o`, `O` in Normal | Open an indented line below / above and enter Insert |
| `x`, `X` | Delete characters after / before the cursor |
| `s`, `S` | Change characters / complete lines |
| `D`, `C`, `Y` | Delete to end of line / change to end of line / copy lines |
| `p`, `P` | Put copied text after / before the cursor; linewise copies insert whole lines |
| `u`, `Ctrl+R` | Undo / Redo; native Ctrl+Z / Ctrl+Y are also available |

Counts also work for character deletion, puts, Undo/Redo and Insert commands:
`3x`, `2p`, `2u`, `3i-<Esc>`, `3ohello<Esc>`.
Copy/delete/change and `p`/`P` use Vim registers. The system clipboard is accessed
explicitly through `"+` or `"*`; native Ctrl+C remains available.

An Insert session, including a preceding change such as `ci"`, is one Undo block,
even across pauses, newlines and Backspace. Cursor movement, leaving the editor,
saving or leaving Insert ends that block. Large scans, range edits, repeated
puts and disk-backed history use cancellable background work; cancellation
discards staged changes. Navigation reads bounded text windows.

## Additional Normal commands

| Keys | Action |
| --- | --- |
| `J`, `gJ` | Join at least two lines; `gJ` preserves whitespace, `J` strips following indentation and supplies spacing |
| `~` | Toggle the case of count graphemes, then advance within the line |
| `gu`, `gU`, `g~` + motion/object | Lowercase, uppercase or toggle case; examples `guiw`, `gUap`, `g~$` |
| `guu`, `gUU`, `g~~` | Apply the case operator to count lines; `gugu`, `gUgU`, `g~g~` also work |
| `Ctrl+A`, `Ctrl+X` | Add/subtract count from the number under or after the cursor on this line |
| Visual `g Ctrl+A`, `g Ctrl+X` | Increase the amount for each selected line containing a number |
| `=` + motion/object, `==` | Recompute line indentation using the internal C-style formatter and EditorConfig indentation settings |
| `gq` + motion/object, `gqq` | Reflow text; examples `gqap`, `gqj`, `gqG` |

Case operations, joins, numbers and formatting support counts, Visual selections,
dot-repeat, macros and grouped Undo. Visual Block case/number changes respect
the rectangle. Case conversion retains local marks and the pending global line
selection, including when UTF-8 character lengths change. Explicit uppercasing
converts `ß` to `SS`; toggling case leaves `ß` unchanged, matching Vim. `ẞ`
lowercases to `ß`. Edit records and Undo payloads are batched on disk. Large
operations are cancellable; Escape discards the current staged operation.

`Ctrl+A/X` use 64-bit arithmetic, decimal signs and leading-zero padding.
`:set nrformats=bin,hex` is the default; `octal`, `alpha` and `unsigned` are also
supported. Partial Visual selections constrain the digits affected.
`:set joinspaces` enables sentence spacing for joins; `:set nojoinspaces` is
the default. `:set textwidth=40` / `:set tw=40` selects a reflow width; zero
uses the editor's fallback width of 79 columns. These settings currently apply
to the application session and are not persisted.

`gq` preserves paragraph boundaries and indentation, and continues common
comment leaders (`//`, `#`, `%`, `>`, `*`). It does not run an LSP formatter or
external program. The internal indentation/reflow implementations cover common
C-style constructs and ordinary text; the full `cinoptions`, `indentexpr`,
`formatexpr`, `formatprg`, custom `comments` and `formatoptions` configurations
are not implemented. They must not be interpreted as supported Vim options.
Reflow rejects indentation over 1 MiB to bound repeated-prefix allocation.

## Screen navigation and folds

| Keys | Action |
| --- | --- |
| `H`, `M`, `L` | Move to the top, middle or bottom of the viewport. Counts on `H/L` count inward from that edge. |
| `gj/gk` | Move by displayed rows, including wrapped continuations. Ordinary `j/k` move by document lines and skip closed folds. |
| `g0/g^/g$`, `gm` | Move to the start, first nonblank, end or middle of the visible row. |
| `Ctrl+D/U` | Scroll down/up by half a page. A count sets the size for following half-page commands. |
| `Ctrl+F/B`, `Ctrl+E/Y` | Scroll by a page or a single row. |
| `zt/zz/zb` | Put the cursor row at the top, middle or bottom. A count selects the destination document line. |
| `zh/zl`, `zH/zL`, `zs/ze` | Scroll horizontally by character widths or half a viewport, or align the viewport to the cursor. |
| `zf{motion}`, Visual `zf`, `[count]zF`, `:[range]fold` | Create a closed manual fold. |
| `za/zo/zc`, `zA/zO/zC` | Toggle, open or close a fold. Uppercase variants also affect children. |
| `zM/zR`, `zn/zN/zi` | Close or open all folds, or control whether folding is displayed. |
| `zd/zD/zE` | Delete the current fold, its subtree, or all folds. |
| `zj/zk`, `[z/]z` | Jump between folds or to the boundaries of an open fold. Closed folds are represented by their header. |

Use `:set wrap` to enable soft wrapping in the current editor and
`:set nowrap` to turn it off. Wrapping starts off to match the editor's usual
horizontal scrolling. Wraps use grapheme boundaries and account for tabs.
Lines larger than 64 KiB retain the editor's horizontal scrolling path.
The first wrap layout and changes to available width remeasure the document.
While editing, unchanged wrap boundaries are reused. Screen motions respect
the editor's measured viewport, so pixel widths and line breaks can differ
from a terminal with fixed cell widths.

Manual folds are local to the open editor and nest up to the total limit of
2,048 configured ranges. Double-click a closed header to open it. The buffer,
line numbers and saved bytes retain their original document positions. Fold
boundaries follow edits and text Undo/Redo. Creating or removing a fold does
not add a text Undo step. Accepted search and navigation reveal hidden
destinations, while cancelling an incremental search restores its temporary
fold openings. Built-in comparison views keep their alignment and temporarily
suspend wrapping and folding. Automatic syntax/indent/marker folds and
persistence across reloads are outside this manual-fold implementation.

## Registers

Registers and macros are shared by all open editor windows for the current
application session. Their contents are not saved across application restarts.

| Register | Use |
| --- | --- |
| `"` | Unnamed register, used by default |
| `0` | Most recent unnamed yank; deletes preserve it |
| `1`–`9` | Numbered deletion history |
| `-` | Small deletions within a line |
| `a`–`z` | Named text or macro registers |
| `A`–`Z` | Append to the corresponding lowercase register |
| `_` | Black hole: discard text without changing other registers |
| `+`, `*` | Clipboard / primary selection, when supported by the platform |
| `.` | Last inserted text |
| `%` | Current file path |

Prefix an operation with `"<register>`: `"ayy`, `"ap`, `"0P`, `"_diw`, `"+y`.
In Insert or Replace, **Ctrl+R** followed by a register inserts its contents.
Characterwise, linewise and blockwise copies retain their shape. Uppercase
append follows the register type, including vertical append for block registers.
Register line separators are normalized to LF; puts use the receiving file's
EditorConfig/EOL convention. Visual `p` saves the replaced text in the unnamed
register; Visual `P` preserves the source register.

Register payloads use shared temporary files, so rotating numbered registers
does not copy their full contents into memory. Large puts and register appends
run in the existing cancellable worker path.

## Dot-repeat and macros

**`.`** repeats the last Vim change, including an Insert session or a change
such as `ci"new<Esc>`. **`3.`** replaces a leading count for a counted change.
Visual changes retain their recorded selection shape. Committed IME text and
accepted completion edits can be replayed as edits rather than re-running the
input method or language server. Native menu actions are not dot commands.

**`qa`** starts recording into register `a`; **`q`** in Normal stops recording.
**`qA`** appends. The mode indicator displays `recording @a`.
**`@a`** plays the macro, **`3@a`** repeats it, and **`@@`** plays the last macro
again. A text register can also be executed as editor keys.

Playback yields to the UI, waits for background edits and follows mark jumps
between tabs, including files opened during playback. One invocation is an
Undo block in each edited buffer. Escape or other editor input interrupts
playback; losing editor focus stops it. Already completed edits are retained
and can be undone; cancellation discards an in-progress staged operation.
Dialogs and terminal input are not recorded. Completion requests are suppressed
during playback.

Dot recording and macro recording/playback are bounded at 100,000 input steps; macro nesting is limited
to 100 levels. Macro recording retains up to 1 MiB of serialized input. Literal text registers over 1 MiB are not expanded into key
sequences. These bounds prevent accidental recursion from locking the UI.

## Marks and jumps

**`ma`** stores a local file mark, **`` `a ``** jumps to its exact position,
and **`'a`** jumps to the first nonblank character on its line. Uppercase marks
(`mA`, `` `A ``, `'A`) work across files and reopen a closed file when needed.
Local mark motions can follow operators, for example **`d'a`** or **`` y`a ``**.
**`''`** / **\`\`** return to the previous jump context. The last Visual
selection endpoints are available as **`` `< ``** and **`` `> ``**.

**Ctrl+O** moves back through jumps; **Ctrl+I** or **Tab** in Normal moves forward.
Counts work in both directions. File opening, go-to-line, marks and Vim jump
motions share the window's Vim jump list. Ordinary `hjkl` movement does not
create a jump entry. Positions follow foreground and background edits and Undo.
The list retains 500 locations; local marks cache 256 files, with up to 256
destructive history states retained per mark for deleted-position recovery.

These modal bindings take precedence while the editor has focus. Application
commands remain available through their menus and the command palette.

**`g;` / `g,`** navigate the buffer-local change list, with counts. Nearby
changes on the same line coalesce; up to 100 locations are retained for the
current document. Undo/Redo do not add new change-list entries. This list is
independent of Ctrl+O/I jumps and is not persisted across restarts.

Special marks **`'[` / `']`** describe the last changed region, **`'.`** the
last change, **`'^`** the last Insert exit and **`'"`** the last known file
position. Backtick variants jump to the exact column. Sentence/paragraph marks
**`'(` / `')` / `'{` / `'}`** compute their boundary at the current position.
**`g'a` / ``g`a``** jump to a mark without adding a same-buffer jump-list entry.
The stored special marks are also usable as Ex range addresses.

## Visual Block and Replace

**Ctrl+V** selects a rectangle. Move with the ordinary Vim motions, use **`o`**
to swap ends or **`O`** to swap horizontal corners, and **`$`** to extend to each
line's end. **`d/x`**, **`c/s`**, **`y`**, **`r<char>`**, **`>`/`<`**, **`p/P`**
operate on the rectangle. **`D`/`C`** act from its left edge to line ends.
**`I`/`A`** insert before/after it. Block Insert/Change uses the editor's multiple
cursors internally, so text appears on the affected rows as it is typed.
Insert at a block's left edge skips shorter lines; append pads them as needed.
Tabs and Unicode graphemes are kept intact, with spaces used for partial tabs.

**`r<char>`** replaces characters in Normal or Visual. **`R`** enters Replace:
typing overwrites graphemes and extends the line at EOL. Backspace restores
characters overwritten by direct typing. Large register inserts are atomic
Undo operations. Escape returns to Normal. Replace uses the same grouped
history as Insert; it does not trigger completion popups.

Leader bindings belong to later stages. Unknown
plain keys in Normal/Visual do not insert text. Paste/cut typing shortcuts are
blocked there; use Insert for text input. Multiple cursors are disabled while
Vim is enabled outside block editing. Modal keys currently use the Latin layout.

## Additional Insert and Visual commands

**`is/as`** select a sentence or its surrounding whitespace. Counts, operators
and repeated Visual objects work across lines and paragraphs. **`it/at`** select
inside/around matching HTML/XML tag blocks; repeated Visual objects expand to
tags and then enclosing blocks. Tag names ignore case, self-closing tags are
skipped, and unmatched tags are tolerated, following Vim's text-object rules.
This is a text scanner rather than an HTML parser. Nesting is bounded to 100,000
pending tags. Visual word/sentence objects extend at the active end; paragraph
objects remain linewise.

**`gv`** restores the previous Visual selection, including line/block mode and
orientation. In Visual it exchanges the current and previous selection. Saved
endpoints follow edits and Undo. **`gn/gN`** select the current or next/previous
match of the last search pattern, independently of its search direction; repeat
in Visual to extend the selection. Counts and operators work, for example
`cgn`, `2dgn`, then `.`. Search offsets do not apply to these match objects.

**`gi`** starts Insert at the last Insert-exit mark; **`gI`** starts at column 1.
Additional Insert/Replace bindings:

| Keys | Action |
| --- | --- |
| `Ctrl+H`, `Ctrl+W`, `Ctrl+U` | Backspace, delete a word, delete entered text to the start of the line; repeat `Ctrl+U` to remove earlier text |
| `Ctrl+T/D` | Increase/decrease indentation to the next/previous indentation boundary, using EditorConfig |
| `0 Ctrl+D`, `^ Ctrl+D` | Remove indentation; `^` restores it on the next entered line |
| `Ctrl+O` | Execute one Normal command, then return to Insert/Replace; operators, searches, Ex and macros can complete asynchronously |
| `Ctrl+G u`, `Ctrl+G U` | Break the Undo group, or keep the next same-line left/right movement in it |
| `Ctrl+Y/E` | Insert the character at the same logical column from the preceding/following line |
| `Ctrl+A`, `Ctrl+@` / `Ctrl+Space` | Insert the last inserted text; the latter also leaves Insert |
| `Ctrl+I`, `Ctrl+J/M` | Tab, newline with the editor's smart indentation |
| `Ctrl+V/Q` | Literal input: next character, decimal (3 digits), `o` octal (3), `x` hex (2), `u` Unicode (4), `U` Unicode (8) |
| `Ctrl+K {two characters}` | Built-in Vim digraph, e.g. `a:` → ä, `Eu` → € |
| `Ctrl+C` | Leave Insert/Replace |

Escape cancels a partial digraph. Literal Escape/Tab/Return are stored as
characters when entered after `Ctrl+V`. A non-digit terminates a numeric literal
and is then processed normally. IME commits retain the editor's normal Unicode
input and clear an unfinished literal/digraph prefix. Replace backspacing
restores directly overwritten text. These commands use the shared registers,
history, dot-repeat and macro machinery.

## Insert completion

**`Ctrl+N/P`** cycle forward/backward through matching keywords in the current
buffer. **`Ctrl+X`** selects a source:

| Following key | Source |
| --- | --- |
| `Ctrl+N/P` | Current-buffer keywords |
| `Ctrl+L` | Whole lines, initially searching backward; indentation is ignored |
| `Ctrl+F` | Files/directories relative to the current file |
| `Ctrl+K` | Files configured with `:set dictionary=words.txt,other.txt` (`dict`) |
| `Ctrl+T` | Synonym lines in `:set thesaurus=synonyms.txt` (`tsr`) |
| `Ctrl+I` | Current buffer and recursively included local files |
| `Ctrl+D` | `#define` names in the buffer and those included files |
| `Ctrl+]` | Names in ctags files configured with `:set tags=./tags,tags` (`tag`) |
| `Ctrl+V` | Supported Ex command names |
| `Ctrl+O/U` | Turtle Code's existing language-server completion |
| `Ctrl+Z` | Stop the completion prefix |
| `Ctrl+E/Y` | Scroll one screen row down/up |

`Ctrl+N/P` cycle a source; repeating its source key also cycles it (`Ctrl+L`
backward, others forward). The original prefix participates in the cycle.
`Ctrl+Y` accepts, `Ctrl+E` restores the original prefix. Typing continues with the
selected text. Built-in completion does not open a second popup; the existing
language-server popup retains its Enter/Tab/Escape behavior. Dot repeats the
accepted text; macros replay their completion keys. One Insert session remains
one Undo group unless explicitly broken. Matching honors `ignorecase/smartcase`.

Providers run in cancellable workers and never write their source files.
Each scan reads up to 16 MiB, skips lines over 64 KiB and retains at most 2,000
unique candidates. File completion visits at most 20,000 directory entries;
include scans visit at most 256 distinct files and resolve paths relative to
the including file. Dictionary/thesaurus/tags accept explicit comma-separated
paths. Keywords come from the current buffer; Vim's `complete`, `path`, wildcard
option expansion, continuation completion, custom Vimscript functions and
`infercase` are not implemented. `Ctrl+X Ctrl+O/U` use the editor's language
service instead of Vimscript `omnifunc/completefunc`. Spelling completion belongs
to the later spelling stage. These settings are session-local.

## Ex command line

Press **`:`** in Normal or Visual. The command line has its own focus and IME;
typing does not modify the document. **Enter** executes, **Esc / Ctrl+C / Ctrl+[**
cancels. An empty **Backspace** returns to Normal. **Ctrl+U** clears text before
the caret, **Ctrl+W** deletes a word, and **Ctrl+R {register}** inserts a register.
**Up/Down** recall history matching the prefix before the caret; **Ctrl+P/N**
recall history without prefix filtering. History retains 200 distinct commands
in the application session. **Tab / Shift+Tab** cycle command or filename
completions. Filename completion is bounded to 200 candidates and 2,000 entries.

`VimEx::Registry` describes commands independently of widgets: names and preferred
abbreviations, ranges, bang support, argument grammar, defaults, comments and bar
handling. `VimEx::Context` supplies current/last lines and mark resolution, with a
search-address resolution through the shared search engine. A chain is parsed and executed one command at a
time, so later addresses see the preceding command's changes. Errors stop the
remaining chain; background operations complete before the next command starts.
The syntax follows the [Vim command-line reference](https://vimhelp.org/cmdline.txt.html).

Addresses include **`.`**, **`$`**, line numbers, **`'a`** marks, and **`+/-`**
offsets. **`%`** selects all lines, **`'<,'>`** the last Visual selection.
Comma evaluates the next address relative to the original current line;
semicolon uses the preceding address. Entering `:` from Visual supplies
`'<,'>` automatically; a Normal count supplies `.,.+{count-1}`.
For example, `:2,4delete a`, `:'a,'byank b`, `:2;+2print`, `:1delete | $`.
An address without a command moves to its line. `|` chains commands;
`"` starts a trailing comment for commands that allow it. Backslash quotes
filename separators, spaces and special characters, e.g. `:w my\ file.txt`.

Initial commands exercise the shared interpreter:

| Command | Behavior |
| --- | --- |
| `:w[rite][!] [file]` | Write the buffer or supplied range |
| `:up[date][!] [file]` | Write only when modified |
| `:wq[!] [file]`, `:x[it][!] [file]` | Save and close the current tab; `x` skips an unchanged write |
| `:q[uit][!]`, `:clo[se][!]` | Close the current tab; `!` discards its unsaved changes |
| `:qa[ll][!]` | Close the current application window |
| `:d[elete] [register] [count]` | Delete lines into a register |
| `:y[ank] [register] [count]` | Yank lines |
| `:pu[t][!] [register]` | Insert register contents as lines; `!` puts before |
| `:p[rint]`, `:nu[mber]`, `:l[ist]`, `:=` | Show bounded line output or a line number |
| `:u[ndo]`, `:red[o]` | Undo or redo |
| `:> [count]`, `:< [count]` | Shift lines using EditorConfig |
| `:f[ile][!]` | Show the current filename and buffer status |

Named-buffer writes to another path keep the source name and modified status;
a full write of an untitled buffer assigns its name. Writing an existing other
file, an externally changed source, or a partial range over the source requires
`!`. Writes are atomic and recheck disk state before committing. A failed write
prevents subsequent commands and prevents `wq` from closing the tab. Save and
close operations use the application's document and recovery handling.
Filenames support `%`, alternate file `#` and `:p/:h/:t/:r/:e` modifiers;
spaces must be escaped.

Ex input is recorded in macros, and playback waits for asynchronous execution.
The last command is available in register **`:`** and is repeated with **`@:`**.
An editing chain is one Undo block. Ex edits do not replace the Normal-mode dot
command. Line output is truncated to protect responsiveness. Address jumps use
Vim's `startofline` behavior (first nonblank); oracle checks enable this Vim
default explicitly because Neovim defaults differ.

Shell commands, append writes, general expansion and Vimscript are not implemented.
Unsupported forms report an error. Stage 6 extends the same interpreter as follows.

## Extended Ex commands

| Command | Behavior |
| --- | --- |
| `:[range]copy {address}`, `:t` | Copy lines after the destination; address `0` inserts before line 1 |
| `:[range]move {address}` | Move lines, including their pending global selections and Vim marks |
| `:[range]join[!] [count]` | Join lines; `!` keeps whitespace verbatim |
| `:[range]sort[!] [flags] [/pattern/]` | Sort the range (default whole file), reverse with `!`; flags `i`, `u`, `n`, `f`, `x`, `o`, `b`, `r` |
| `:[range]global[!] /pattern/ commands`, `:g` | Execute the command body on matching lines; `!` inverts selection |
| `:[range]vglobal /pattern/ commands`, `:v` | Execute on nonmatching lines |
| `:[range]normal[!] keys` | Run Normal input on each addressed line; without a range keep the current column |
| `:[address]mark a`, `:ka` | Set a mark without moving the cursor |
| `:registers [names]`, `:display [names]` | Show bounded register contents |
| `:[address]@a`, `:@@` | Execute register text as Ex command lines; repeat the last Ex register |
| `:[address]read [file]`, `:r` | Read a file after the address; `:0r` inserts before line 1 |
| `:edit[!] [file]`, `:e` | Open/select a file; no argument reloads the current document, requiring `!` for unsaved changes |
| `:enew` | Select a new untitled buffer in the current pane |
| `:file {file}` | Rename the buffer without writing it |
| `:saveas[!] {file}` | Write the complete document and assign its new path |
| `:buffers[!]`, `:ls[!]`, `:files[!]` | List buffers with session numbers and current, alternate, displayed, hidden and modified flags. Add `!` to include unlisted buffers |
| `:buffer [number/name/#]`, `:b` | Select a buffer in the current pane, reopening an unlisted buffer if needed |
| `:bnext`, `:bprevious`, `:bNext`, `:bfirst`, `:blast` | Navigate listed buffers |
| `:bdelete[!] [buffer]` | Unload the buffer, close its views and unlist it. Its number can still be used to reopen it |
| `:bwipeout[!] [buffer]` | Remove the buffer number and its marks as well as unloading it |

Sort compares UTF-8 bytes; `i` ignores ASCII case, matching Neovim's default
(non-locale) sort behavior.

Examples: `:g/TODO/d`, `:v/^#/normal I# `, `:g/error/s/error/warning/g`,
`:1,5move $`, `:%sort iu`, `:e other.txt | $`.

Global selection is made before execution, using the shared Vim pattern engine.
Edits update the selected line identities: deleted lines are skipped, inserted
copies are not selected, and moved lines retain their selection. A global body
owns its `|` chain. Nested globals without an explicit range test only the
current line; an explicit nested range fails. The whole editing invocation is
one Undo block. Escape interrupts execution; edits already applied remain
undoable. Each worker stages its own changes before committing.

The modifiers `:silent`, `:silent!`, `:keepjumps` and `:keeppatterns` apply to
the following command and its nested execution. `silent!` suppresses execution
errors; malformed command syntax stops that command line. `keeppatterns`
preserves search patterns/history. Normal input and register programs use the
existing replay queue and wait for background work. Recording another macro
inside `:normal` is disabled, as in Vim. There are no user mappings yet, so
`:normal` and `:normal!` currently use the same input.

Unsaved modifications require `!` for `:bdelete` and `:bwipeout`. A hidden
buffer keeps its edits and Undo history. Buffer changes carry the remaining Ex
chain, register program and macro into the selected editor, waiting for file
loading. Line selections stay with their original document. Undo groups belong
to each edited document. Buffer numbers belong to the current Turtle Code
window.

Global and ranged Normal execution select at most 100,000 lines, nested programs
are limited to 100 levels, and an invocation permits at most 100,000 Ex commands.
Register programs are limited to 1 MiB. Sort accepts up to 1,000,000 lines.
Selection scans and line edits run in cancellable workers; simple one-line global
substitutions share one filtered substitution plan. Other bodies execute in order
and yield to the UI between commands. Pending line marks
use copy-on-write snapshots and logarithmic row shifts. These bounds report an
error rather than silently processing a partial selection. Behavior is based on
the [Vim Ex command reference](https://vimhelp.org/change.txt.html) and
[global/normal reference](https://vimhelp.org/repeat.txt.html).

## Editor panes, Vim tab pages and diff

**Split Editor Below**, **Split Editor Right** and **Close Editor Pane** are
available in the View menu and command palette with or without Vim mode.
When two panes show the same buffer, they share text, dirty state and Undo.
Each pane has its own cursor and scroll position. Drag a divider to resize the
panes. Closing a pane leaves its buffer available.

Each pane has a header with its file name and an unsaved-change dot. The active
pane has an accent line. Click the file name to show another open buffer in that
pane. The header also offers split and close actions. With multiple tab pages,
the top bar switches between whole layouts. Hover over a tab to see the files
shown on that page.

| Command | Behavior |
| --- | --- |
| `:split [file]`, `:vsplit [file]`, `Ctrl+W s/v` | Split below or right, optionally opening a different file |
| `:new [file]`, `:vnew [file]`, `Ctrl+W n` | Open a file or new untitled buffer in a split |
| `Ctrl+W h/j/k/l`, arrows, `w/W`, `t/b`, `p` | Focus by direction, cycle, select the first or last pane, or return to the previous pane |
| `:hide`, `:close`, `Ctrl+W c/q` | Close the pane. `:hide` keeps unsaved edits in the buffer and requires another pane or tab page |
| `:only`, `Ctrl+W o` | Keep the current pane on this page |
| `Ctrl+W +/−/</>`, `_/\|`, `=` | Resize by lines or columns, maximize or set a dimension, or equalize |
| `Ctrl+W r/R`, `x`, `H/J/K/L`, `T` | Rotate or exchange panes, move a pane to an edge, or move it to a new tab page |
| `:wincmd {key}` | Run a supported `Ctrl+W` action from Ex |
| `:tabnew [file]`, `:tabedit [file]` | Open a tab page with its own arrangement of panes |
| `gt`, `gT`, `:tabnext`, `:tabprevious`, `:tabfirst`, `:tablast` | Navigate tab pages. `{count}gt` selects a page by number. Pages can also be selected in the tab bar |
| `:tabs`, `:tabmove [position]`, `:tabclose [page]`, `:tabonly` | List, reorder or close tab pages |
| `:diffsplit {file}`, `:vertical diffsplit {file}`, `:diffthis` | Compare two buffers in a tab page with aligned lines and synchronized vertical scrolling |
| `[c`, `]c` | Move to the previous or next change |
| `do`, `dp`, `:[range]diffget [buffer]`, `:[range]diffput [buffer]` | Pull changes into the current buffer or send them to the compared buffer. Edits can be undone in the destination buffer |
| `:diffupdate`, `:diffoff[!]` | Recompute the comparison or turn diff off for the current pane. `!` turns it off for both panes on the page |

Vim diff compares two buffers at a time, up to 8 MiB and 30,000 lines per
buffer. Pane layouts and Vim tab pages are local to the current session.

## Vim search and substitution

**`/pattern`** searches forward, **`?pattern`** backward. **`n`** follows the last
search direction and **`N`** reverses it. Counts work with each command. **`*`**
and **`#`** search the keyword under/after the caret as a whole word;
**`g*` / `g#`** allow partial words. Search motions work with Visual and with
operators, e.g. `d/foo`, `c?bar`, `y2n`. Motion-based changes participate in dot
repeat and use the existing register/Undo machinery.

The search field uses the command bar with a `/` or `?` prompt. Incremental
search previews the destination; **Esc** restores the cursor, Visual anchor and
scroll position. Enter accepts. **Up/Down**, **Ctrl+P/N**, and **Ctrl+R {register}**
provide history and register insertion. Empty input repeats the last pattern.
History retains 200 distinct inputs per session, separately from Ex history;
register **`/`** contains the last pattern. Macros record and replay searches.

An optional closing delimiter introduces a search offset: `/foo/e` moves to the
end of the match, `/foo/s+1` one character after its start, `/foo/+2` two lines
below it. `s` and `b` address the start, `e` the end. Offsets are bounded to
10,000 characters/lines. `n/N` reuse the last offset. Match selection accounts
for character offsets before choosing the next occurrence.

The interpreter accepts search addresses such as `:/foo/,$delete` and
`:1;/bar/print`. Empty address patterns reuse the previous pattern. Forward
addresses search after the addressed line; backward addresses search before it.
They update the shared last pattern and direction. Comma/semicolon and mark
addresses retain their Ex behavior. The final delimiter can be omitted for a
search address at the end of the command line, e.g. `:/foo`.

The shared engine translates Vim notation to PCRE2-8 and operates on UTF-8
bytes from a mapped temporary subject. It supports:

- Magic modes `\m`, `\M`, `\v`, `\V`, with Vim's default magic rules.
- Groups `\(…\)`, noncapturing `\%(…\)`, alternatives `\|`, intersection `\&`,
  backreferences, `*`, `\+`, `\?`, `\=`, counted and lazy `\{…}` repetitions.
- Word boundaries `\<` / `\>`, character collections, common POSIX classes,
  `\d/\s/\w/\k/\i/\h/\a/\l/\u/\x/\o` and their corresponding uppercase forms.
- `\c` / `\C`, `\zs` / `\ze`, lookaround `\@=`, `\@!`, `\@<=`, `\@<!`,
  atomic `\@>`, buffer anchors `\%^` / `\%$` and decimal/hex Unicode atoms.
- Multiline `\n`, `\_.` and newline-inclusive common classes; Vim's virtual
  final newline is distinct from a physical file terminator.
- Combining characters: literals distinguish an undecorated base, explicit
  combining marks are matched together, dot/classes consume the composing
  sequence, `\%C` skips composing marks and `\Z` ignores them.

This is an extensible compatibility layer, not a complete implementation of
every Vim pattern atom. Position/visual predicates, optional-sequence atoms,
file/printable classes, the previous-replacement pattern atom `~` and numbered
lookbehind limits are not implemented.
Variable-length lookbehind is limited by the installed PCRE2 version.
The [Vim pattern reference](https://vimhelp.org/pattern.txt.html) describes the
source semantics; the tests compare supported behavior against Neovim.

**`:s/pattern/replacement/flags [count]`** substitutes on the current line;
use a range or **`:%s/old/new/g`** for the whole buffer. Delimiters can be changed,
e.g. `:%s#old#new#g`. Separators and quotes inside the pattern/replacement are
parsed before the following `|` chain. Missing final delimiters are accepted.

| Flag | Behavior |
| --- | --- |
| `g` | Replace all occurrences per line; otherwise the first |
| `c` | Confirm each occurrence |
| `n` | Report the match count without editing; suppress confirmation |
| `e` | Continue when no match is found |
| `i` / `I` | Ignore/enforce case; explicit `\c/\C` in the pattern takes precedence |
| `p` / `#` / `l` | Show the resulting line, its number, or visible control characters |
| `r` | Reuse the last search pattern |
| leading `&` | Reuse previous substitute flags |

Replacement text supports **`&`**, **`\0`**, captures **`\1`–`\9`**, escaped
ampersands/backslashes, **`\r`** (line break using the document's EOL style),
**`\n`** (NUL), **`\t`**, **`\b`**, and case conversion
**`\u/\l/\U/\L/\E`**. Unescaped **`~`** reuses the previous replacement.
Expression replacements (`\=…`) require Vimscript and report an error.

**`:s`**, **`:&`** and Normal **`&`** reuse the previous substitute without its
flags; **`:&&`** preserves flags, **`:~`** uses the last search pattern, and
**`g&`** repeats the substitution across the buffer with its flags. An empty
pattern in a new substitution uses the last search pattern. `:smagic` and
`:snomagic` select the corresponding pattern mode for that command.

Confirmation is inline and shows **CONFIRM** in the mode indicator:
**`y`** replaces, **`n`** skips, **`a`** replaces the remainder,
**`l`** replaces this occurrence and stops, **`q`** stops. **Esc/Ctrl+C** also
stop the remaining command chain. Accepted replacements remain and are undone
together. **Ctrl+E/Y** scroll while confirming. A document revision change
stops confirmation. Recorded macros retain confirmation answers.

Search settings are shared for the current process through `:set`:
`ignorecase/ic`, `smartcase/scs`, `magic`, `hlsearch/hls`, `incsearch/is`,
`wrapscan/ws`, and `gdefault/gd`. `no` disables, `!` toggles, `?` reports.
`gdefault` reverses the meaning of the substitute `g` flag. Defaults are
case-sensitive, magic, incremental search, highlighting and wrapping enabled,
with smartcase and gdefault disabled. **`:nohlsearch` / `:noh`** temporarily
hides highlights; the next search restores them. `:set nohlsearch` persists
until changed explicitly. Saving these options is part of the later settings
stage.

Navigation is independent of the bounded highlight list. At most 2,000 matches
are highlighted around the visible lines; the highlight scan includes one
preceding line of context for multiline patterns. Search and replacement run
in cancellable workers. Temporary subjects, replacement text and match records
are stored on disk. Edits are applied in bounded batches with a shared Undo
payload, retaining individual mark positions. Cancelling a staged operation
discards its pending changes; accepted confirmation steps remain. A chain stays
one Undo block. If the history budget cannot retain a whole ongoing group,
it drops the whole group rather than leaving a partial Undo.

PCRE2 evaluation has a per-match time guard (2 seconds), a 5,000,000 match-step
limit, depth 1,000 and a 16 MiB matching heap limit. Excessively costly patterns
report an error. Search uses PCRE2 10.34 or newer; it is a runtime dependency,
while Neovim remains a test oracle only.

## Verification

Build `turtle-code`, `turtle-code-editor-tests` and `turtle-code-window-tests`,
then run:

```sh
ctest --test-dir build -R '^vim-(editor|viewport|insert|insert-window|normal|session|ex|ex-extended|search|search-core|window|workspace)$' --output-on-failure
```

Tests cover mode transitions, inclusive selections, Unicode/CRLF, input methods,
completion, large-line navigation, settings persistence, several windows, focus,
the command palette and terminal, operators, counts, text objects, grouped Undo,
disk history, registers, dot-repeat, macros, marks/jumps, block editing, Replace
and large range edits, Insert controls/literals/digraphs, completion providers,
Visual reselect/match objects, search/Ex addresses, regex translation, substitutions,
confirmation, mark preservation and history budgets. If `nvim` is installed, edit/motion/register
sequences are also compared with clean headless Neovim. Neovim is a test oracle,
not a runtime dependency. Window checks use Qt's offscreen platform; desktop
input methods and compositor behavior still need a real desktop smoke test.
