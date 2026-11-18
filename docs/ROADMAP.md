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

## Актуальные сведения

Текущие ограничения и устройство описаны в [архитектуре](ARCHITECTURE.md),
[подсветке](HIGHLIGHTING.md), [LSP](LSP.md) и [больших проектах](LARGE_PROJECTS.md).
Возможности модального редактирования перечислены в [VIM.md](VIM.md).


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
