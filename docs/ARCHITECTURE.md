# Architecture notes — v0.10.6

## Text core

`PieceTreeBuffer` is a Qt-independent UTF-8 byte buffer. Its implicit treap stores
pieces that refer either to the immutable original string or to an append-only add
string. Each node aggregates subtree byte length and newline count. Initial and
inserted content is split into 4 KiB pieces so line queries do not scan a single
very large source piece.

The core API uses byte offsets. Callers must keep mutation offsets on UTF-8 code
point boundaries. `utf8PreviousBoundary` and `utf8NextBoundary` provide low-level
code-point helpers.

## Editor adapter and Unicode

`Document` owns the `PieceTreeBuffer` and its edit history. `EditorWidget`
translates Qt input into UTF-8 edits and renders visible lines. Split views of the
same document share that state and keep independent cursor and scroll positions.
`UnicodeNavigation` is a QtCore adapter: it uses
`QTextBoundaryFinder` to map extended grapheme-cluster boundaries and basic word
movement back to UTF-8 byte offsets. The Piece Tree remains independent of Qt.

Qt input-method events are handled by `EditorWidget`. Preedit text is rendered as a
composition overlay, committed text is inserted as a single standalone edit, and
Qt's UTF-16 replacement ranges are translated to Piece Tree byte offsets.

Selection, clipboard and edit-history integration remain in the widget layer for
now. The renderer creates temporary `QString` snapshots for visible lines; a later
layout/cache layer will remove remaining avoidable work. Bidirectional visual
navigation and full word-boundary semantics are not implemented.

## Testing

`turtle-code-text-tests` and `turtle-code-history-tests` are dependency-free core test
executables. They cover Piece Tree edits, line indexing, UTF-8 boundaries, randomized
edits, history coalescing and saved-state tracking.

When the Qt application is enabled, `turtle-code-unicode-tests` checks grapheme
navigation for combining marks, ZWJ emoji, regional-indicator flags and emoji skin
tone modifiers, plus basic word movement. IME still needs manual verification in a
real desktop session with the user's input method framework.

`TURTLE_CODE_BUILD_BENCHMARKS=ON` enables a reproducible microbenchmark. Treat its
output as a local comparison tool, not as a performance guarantee.

## Edit history

`EditHistory` is a Qt-independent command history layered over `PieceTreeBuffer`. It
stores replaced and inserted byte sequences, cursor positions before/after each
command, and logical state identifiers. Adjacent character insertions and adjacent
deletions can be coalesced within a 750 ms window. Save operations mark the current
state identifier; `isModified()` compares the current identifier to that save point.
Cursor navigation, standalone edits, save, undo and redo form coalescing barriers.

## Syntax highlighting (v0.6.1)

`SyntaxHighlighter` is a QPainter-oriented adapter: it provides UTF-16 spans by
document line, without creating a `QTextDocument` or per-line widgets. It caches
Tree-sitter capture spans and contains no lexical tokenizer. The renderer paints only
visible lines.

The Tree-sitter backend loads the grammar module and highlight query for the active
language. Each editor session owns a worker-side parser and persistent syntax tree.
The UI records ordered edits as UTF-8 byte offsets plus byte-based row/column points,
removed bytes and inserted bytes. The worker applies each edit with `ts_tree_edit`,
then calls `ts_parser_parse_string` with the edited previous tree so unchanged
subtrees can be reused. `ts_tree_get_changed_ranges` is combined with explicit edit
ranges to invalidate affected captures. Captures are queried for the visible rows
and nearby uncached rows. The worker updates its per-line cache as edits insert
or delete lines.

Parsing runs asynchronously on Qt's worker pool. The worker owns its source text,
parser and tree; ordinary edits replay on that private source without copying the
live document again. A 90 ms debounce coalesces rapid edits. Results are displayed
only when the document revision, session generation and language still match.
Initial loads, language changes, and detected edit-stream mismatches can trigger
a full snapshot and parse. Undo/Redo sends incremental edits. Outdated parse or
query jobs stop cooperatively. Unsupported or uninstalled file types remain
plain text. See [HIGHLIGHTING.md](HIGHLIGHTING.md) for budgets and scheduling.

## Language servers

`LspClient` communicates with an installed server through framed JSON-RPC over
`QProcess`. `LspManager` routes documents to server profiles by language and
workspace. Diagnostics paint on visible rows, `Ctrl+Space` requests completion,
and `F12` opens a definition. `LspAdaptivePolicy` controls synchronization and
automatic work based on document size and response latency. See [LSP.md](LSP.md)
for size thresholds and transport limits.

## Language registry (v0.7.0)

`LanguageRegistry` is the central catalogue for syntax support. Each entry has a
stable ID, display name, file extensions, grammar repository, parser symbol and
optional source subdirectory. User enablement is stored under
`languages/<id>/enabled` in `QSettings`. `Settings… → Languages and tools → Syntax highlighting` installs missing
grammars on demand and refreshes open tabs after installation or enablement.

`GrammarManager` downloads source archives asynchronously, compiles a shared
parser module, installs the upstream highlight query, and validates the module
against the linked Tree-sitter runtime. The editor dynamically loads each module
and query from the per-user application-data directory. Disabled, uninstalled, or
incompatible grammars leave the file in plain-text mode.


## Runtime grammar modules (v0.7.1)

The Tree-sitter runtime is linked by the application, but language grammar parsers are not compiled into the executable. `GrammarManager` downloads public grammar source archives over HTTPS, builds a small shared parser module, and stores it under the user's Turtle Code application-data directory. The generic Tree-sitter backend loads the module with `QLibrary`; missing or incompatible grammars are a supported state and files remain plain text.


## Viewport and scrolling (v0.8.0)

`EditorWidget` remains a single `QAbstractScrollArea`; it does not create one widget
per document line. `paintEvent` visits only the visible line range plus a two-line
overscan. The line-number gutter is fixed, while horizontal scrolling is applied to
the text, selection, preedit and caret geometry. Text drawing is clipped to the
text viewport so scrolled glyphs cannot paint over the gutter.

The vertical scrollbar uses visible layout rows, including wrapping and folds.
To avoid scanning every line on each edit,
the horizontal extent is recalculated from the active line only. Cursor movement and
edits update the extent and keep the caret visible. Drag-selection auto-scrolls at
both viewport edges. `scrollContentsBy` schedules a repaint instead of attempting
to pixel-scroll stale custom-rendered content.

Loading and saving run in background workers. The chunked file loading section
below describes backing files, preview and cancellation.


## Project workspace and process entry point (v0.9.0)

`ProjectExplorer` enumerates expanded directories in the background and watches
them for changes. `WorkspaceFiles` shares ignore rules between the tree, Quick Open
and project search. Hidden files have a separate toggle, `Ctrl+H`. See
[LARGE_PROJECTS.md](LARGE_PROJECTS.md) for filtering, indexing and pagination.

The command-line entry point resolves positional paths relative to the launching
process. One directory becomes the project root and files open as tabs. A per-user
`QLocalServer`/`QLocalSocket` endpoint forwards requests to the primary window.
`--new-window` opens a separate process, while the default and `--reuse-window`
target the primary endpoint.

## Project search (v0.10)

`SearchPanel` is a persistent sidebar surface rather than a modal dialog. It debounces
queries and delegates filesystem work to `ProjectSearchService`. A new query cancels
the previous worker through a shared atomic token, and stale results are discarded by
generation. Search supports case sensitivity, whole-word and regular-expression
modes, include/exclude globs, a shared hidden-file toggle and binary-file detection.
Matches are stored on disk and the panel displays them by file on pages. The selected
match is revealed after its document has loaded. See [LARGE_PROJECTS.md](LARGE_PROJECTS.md)
for regex budgets and long-line handling.


## Project Explorer refresh and rename behavior

Directory refreshes retain the set of expanded descendant paths and restore them progressively as lazy-loaded levels become available. Renaming a nested item remaps its visible tree paths, loaded-directory state, pending expansion paths, and filesystem watcher registrations before refreshing the parent. Context actions that operate on an existing item are omitted when the pointer is over empty tree space.


## v0.9.8 — Explorer reconciliation
Directory refreshes now reconcile children by normalized path. Existing tree items are reused, new entries are inserted, and only removed entries are destroyed. This avoids resetting expanded state, selection and viewport position during rename/delete and watcher-driven refreshes.

## Text storage retention

Single history commands own one removed/inserted payload; multi-change commands own only their change vector. `retainedTextBytes()` reports payload sizes across Undo and Redo without clearing either stack. Buffer compaction does not invalidate history because commands use text/offsets rather than references to the add arena.

After erasing, the piece buffer rebases only when retained backing capacity exceeds three times the remaining live bytes plus 16 MiB. Rebuilding is amortized and retains the current text/line positions; `clear()` releases backing capacity. A snapshot materializes only live pieces rather than copying the entire append arena. Syntax parsing obtains one locked live-text copy directly, avoiding an intermediate buffer snapshot. These changes reduce duplication; they do not promise an immediate RSS reduction from the allocator, nor imply an immediate reduction in process RSS. History now has a configurable payload budget as described below.

## Chunked file loading and saving

Files below 4 MiB use resident original text. Larger regular files are read in 64 KiB blocks into a private, immediately unlinked temporary backing file. The initial pass also builds the piece/newline index in 4 KiB pieces. Text content is then fetched on demand with `pread` and a shared LRU cache of at most 32 × 64 KiB pages (2 MiB). Piece metadata remains resident; it scales with file size. This stage removes full-file heap copies from loading, but still scans/copies the complete source before the tab becomes editable. A bounded initial preview is published after the first read, while the remaining scan/indexing continues in a worker. The preview is read-only until indexing completes.

The original backing is immutable and independent from the user's pathname, so external truncation or an atomic save cannot invalidate live text. Source size/mtime/ctime are checked around the initial pass; concurrent modifications abort the load. Loading errors and cancellation leave existing buffer content intact. Closing a loading tab signals cancellation without accessing GUI objects in the worker.

File-backed snapshots clone the piece tree and share the backing file/cache; they do not materialize original text. Insertions of 1 MiB or more use temporary disk storage; accumulated smaller insertions spill their append arena at 4 MiB. Undo/Redo payloads can also live on disk with configurable retention limits. Saves iterate live pieces into QSaveFile and atomically commit, including Save As and Save All. The prior backing remains usable by editor and search snapshots after commit. Its file descriptor/storage is released when the last owner disappears; snapshots may outlive a tab.

The backing copy requires temporary filesystem space approximately equal to the source size, using TMPDIR when set or /tmp. Failure to create/write it is reported as an open failure. OS filesystem cache, resident piece metadata, edited text and history are separate from the 2 MiB original-page cache. The editor's decoded viewport-line cache is bounded to 512 entries. Lines over 64 KiB use a bounded, simplified viewport rendering path. Full-text APIs still explicitly materialize text. LSP, formatting/rename previews and syntax parsing retain their separate copy/time budgets.

Search can iterate lines from a shared file-backed snapshot, and random line navigation uses subtree newline counts instead of reading all intervening pages. UTF-8 bytes, BOMs, CRLF, no-final-newline files and edits spanning page boundaries are preserved. Tests cover page eviction, snapshot ownership after external replacement/deletion and document close, load cancellation/temp storage failure, streaming failures, Undo/Redo and atomic save over the input file. No multi-million-line memory benchmark was rerun for this change.

## Bounded editing operations and history

Font preferences use `app/FontSettings.hpp`. QSettings stores independent
families and point sizes (6–96) under `fonts/tree`, `fonts/editor` and
`fonts/terminal`. Saving the settings updates existing explorers, editors and
terminals across application windows. The editor recomputes scroll geometry
and the terminal recomputes its cell grid.

See [TEXT_PROCESSING.md](TEXT_PROCESSING.md) for user-facing behaviour and limitations. The editor settings store `editor/historyMemoryMiB` (default 64), `editor/historySpillToDisk` (default true), and `editor/historyDiskMiB` (default 0/unlimited) in QSettings. Limits apply per document to retained payload bytes, not whole-process RSS or tree/container metadata. Oldest commands are removed when spill is disabled and RAM is exceeded, or the disk history limit is exceeded. Save-point identifiers survive pruning; storage errors keep recoverable history and report a notice, making the RAM target soft in that error case.

Large paste, replacement, cut/copy and stored Undo/Redo prepare a buffer/history snapshot in a worker. Progress checkpoints observe cancellation; the UI commits a successful result only for the original revision. The document remains read-only during preparation. Cancelling discards staged document/history changes. A stable per-buffer mutex protects replacement from concurrent syntax/search readers. Saves stream into QSaveFile and commit atomically, with cancellation before commit.

Nodes aggregate code-point and UTF-16 counts as well as bytes/newlines. Position lookup reads at most a piece after tree descent, including for supplementary characters spanning pieces. Lines over 64 KiB render only the horizontal viewport in fixed cells, without syntax/diagnostic styling or full shaping. Word/grapheme navigation uses an 8 KiB context on either side for these exceptional lines; ordinary lines preserve complete Qt Unicode boundary handling.

Literal find retains only a chunk and query-sized overlap, with Unicode boundary context. Searches at 2 MiB and above run in a worker; stale searches are cancelled. Regex uses bounded per-line scans and skips lines over 256 KiB, documented in the search tooltip. Results are capped at 10,000 and Replace All is disabled at that cap. The internal clipboard can carry a disk-backed payload; platform clipboard reads and external applications requesting text can still materialize the full clipboard string.

## Smart editing

`SmartEditing.hpp` prepares byte-based edit batches for indentation and comment
commands, mapping cursor and selection endpoints without modifying the source.
`EditorWidget` applies a batch as one history action; large line operations use
the existing cancellable staged-buffer worker. `CodeContext.hpp` maintains bounded
lexical checkpoints for typing and bracket matching without requiring installed
Tree-sitter grammars. Concrete edit offsets invalidate its suffix; whole-buffer
replacement clears it. See `SMART_EDITING.md` for semantics and limits.
