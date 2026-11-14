# История реализованных этапов

Открытые задачи и планы удалены по указанию пользователя 14 ноября 2026 года.
Ниже сохранены записи о реализованных этапах и технические сведения.

- [x] v0.1 — Qt desktop shell and basic editor
- [x] v0.2 — Piece Tree text buffer and core tests
- [x] v0.3 — editor text/cursor geometry correction
- [x] v0.4 — coalesced Undo/Redo and save-point tracking
- [x] v0.5 — Unicode grapheme navigation, selection, and IME foundation
- [x] v0.5.1 — Ctrl+Backspace / Ctrl+Delete
- [x] v0.6.x — Tree-sitter-based syntax highlighting
- [x] v0.7.x — language registry, grammar installation and enable/disable controls
- [x] v0.8.0 — editor viewport and scrolling: visible-line rendering, horizontal scrolling, cursor visibility, selection auto-scroll, redraw correctness
- [x] v0.8.1 — debounced background Tree-sitter parsing with immutable snapshots and stale-result rejection
- [x] v0.8.2 — persistent worker-side Tree-sitter tree, ordered TSInputEdit application, changed-range query refresh and stale-result handling
- [x] v0.8.3 — preserve committed syntax spans during edits; atomically swap in matching parse results to prevent full-document highlight flashing
- [x] ordinary edits replay on a worker-owned source instead of snapshotting the live document for every parse
- [x] v0.9.0 — async lazy project Explorer, multi-root folders, `.gitignore`, directory watcher, project search, CLI paths and local IPC
- [x] v0.10 — fast project-wide search and replace
  - [x] Quick Open (`Ctrl+P`) with asynchronous workspace indexing, fuzzy path matching and recent files
  - [x] Go to Line (`Ctrl+G`) using the existing editor navigation API
  - [x] compact find/replace bar in current file with match highlighting, navigation, case/word/regex and single/batch replace
  - [x] VS Code-style sidebar search, grouped results, case/word/regex options, include/exclude filters, hidden-file toggle and cancellation
  - [x] replace preview for visible results, progressive result loading, selected matches, protection for modified buffers, file-change checks and streaming project-wide replace
- [x] initial clangd integration for C/C++: diagnostics, Ctrl+Space completion and F12 definition

## Current editor performance notes

- Painting is viewport-bounded: only visible rows plus a small overscan are drawn. The Piece Tree remains the source of document text; the editor does not create widgets per line.
- The vertical scrollbar represents document lines. The horizontal scrollbar tracks the active line's pixel width, avoiding an O(document-lines) width scan on every keystroke. Moving to another line recalculates its horizontal extent.
- The gutter remains fixed while the text area scrolls horizontally.
- Cursor movement and edits keep the cursor visible. Dragging a selection beyond the viewport auto-scrolls vertically and horizontally.
- Tree-sitter parses run in the Qt worker pool. Ordinary edits replay on a worker-owned UTF-8 source; loads, undo and redo still take a full snapshot. The worker retains its parser/tree, applies ordered `TSInputEdit` operations, and refreshes captures in changed ranges. Edits are debounced and obsolete results are discarded. Active parsing is not interrupted yet.
- C/C++ LSP uses an installed `clangd`. Document sync is currently full-text and limited to files up to 2 MiB to keep larger files responsive.
- The optional `TURTLE_CODE_SYNTAX_BENCH=1 ./build/turtle-code-syntax-tests` probe measures an initial parse and an edit in a 1.6 MiB C++ buffer. One local Debug run took 904 ms initially, under 1 ms to queue the edit, and 550 ms for the worker to return updated spans; timings are machine-dependent.
- File loading and saving run in background workers. Project-wide replacement writes files atomically and reloads clean open buffers.
- Project search has no fixed result or file-size cap. The sidebar creates match rows in batches, and typing replacement text repaints only visible rows. Replacement streams each file through an atomic temporary file instead of materializing a second full-file copy.


### v0.9.8 Explorer rename stability
- Rename files and directories from the Explorer context menu.
- Delete files and directories with confirmation.
- Keep modified editor buffers recoverable when their backing file is deleted.
- Removing a workspace root is non-destructive.

- [x] v0.9.8 — preserve expanded nested directories during refresh and rename; limit empty-space context menu to creation actions


### v0.9.8 Explorer descendant reload fix
- [x] Invalidate descendant lazy-load state when replacing a directory's child tree, then restore expanded paths progressively.


### v0.9.8 Explorer state preservation
- [x] Reconcile directory contents in place to retain expansion, selection, scroll position and loaded descendants during filesystem refreshes.
