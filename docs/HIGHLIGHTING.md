# Highlighting

Turtle Code uses the same two-layer approach commonly used by LazyVim: incremental Tree-sitter queries provide syntax highlighting, and LSP semantic tokens refine symbols. The renderer composes both layers for visible rows. Neither Neovim nor a JavaScript runtime is required.

## C and C++

`queries/c/highlights.scm` and `queries/cpp/highlights.scm` contain unmodified queries from nvim-treesitter. C++ inherits C rules. Syntax captures (including their dotted names and explicit priorities) survive to the theme renderer. Query predicates filter matches; hidden helper captures are not painted. Nested pointer/array/function parameter declarators have one additional structural predicate, defined in `HighlightQueries.cpp`, which recognizes the declared name and excludes array bounds and default expressions. This is not semantic name inference: parameter references are supplied by LSP.

Each query pattern is checked against the installed grammar. Unsupported anonymous alternatives in bracket lists are removed individually; unsupported patterns are skipped and counted in the log. This allows the queries to work with grammars accepted by the installed Tree-sitter runtime. Other languages retain their installed queries; Neovim query inheritance/injections for every language are not implemented.

Tree-sitter parsing runs in the existing background worker with a 90 ms typing debounce and incremental edits. The former 8 MiB syntax file cutoff is removed; processing and working-copy memory budgets apply instead. Full-document or viewport semantic tokens are requested for the active file after an adaptive 200–4000 ms pause and decoded in a background worker. UTF-16 boundaries, token legends, modifiers and non-overlap are checked before use. Version/generation checks discard late responses. Changed rows lose old semantic tokens immediately; unaffected rows move with inserted/deleted lines. No server request is issued on repaint. LSP follows the separate adaptive policy in [LSP.md](LSP.md).

## Viewport priority

The editor supplies its visible rows to `SyntaxHighlighter`. A load or edit first builds/updates the complete syntax tree in the background, then queries only the visible range plus 80 rows on either side. This preserves context for multiline comments, raw strings, ancestor predicates and declarations beginning outside the viewport. It does not parse a detached text fragment.

Missing rows in the current working region are queried in batches of at most 256 rows. Small files fitting the row budget can be prepared entirely; large files prefetch at most 2048 nearby rows, while retaining recently visited rows within the LRU budgets. Each batch is delivered independently to the UI; it no longer waits for a document-wide highlight cache snapshot. Scrolling replaces the pending viewport request and interrupts the previous query cooperatively; the next job prepares the newest viewport. Already prepared rows reuse their cache. Edits shift row caches and invalidate affected syntax ranges; late results do not overwrite newer document revisions, and undelivered batches are retried in the correct pre-edit coordinates.

`highlightingReady` means a batch is available; `highlightingComplete()` reports that the current working region is prepared (or has reached its fallback), not that a large file is cached in its entirety. Tests verify the first batch and margins, scrolling priority, offscreen multiline context, edits during progressive processing, and exact agreement of final spans/captures with a complete query.

This is viewport-prioritized **highlight querying and delivery**, not streaming parsing: the initial snapshot and syntax tree still cover the entire file. Obsolete parsing/querying is interrupted cooperatively; background batches also yield between jobs. Sparse cache eviction and processing budgets apply below. Partial text loading remains a separate step; the old syntax file cutoff is removed and LSP has its own adaptive transfer policy.

## Cancellation and edits

Shared atomic document/viewport epochs signal obsolete work without accessing the editor from a worker thread. New text cancels old parsing and querying; scrolling cancels querying while allowing useful syntax-tree construction to finish. Buffer replacement, language changes, crossing the current file-size limit, and destruction also signal cancellation. The UI permits one active job until its completion has been consumed, including the interval after a future finishes but before its queued notification arrives.

Parsing uses resumable 5 ms slices and checks the document epoch between slices. CMake detects Tree-sitter's progress-callback API (`ts_parser_parse_with_options`) and uses it when available, falling back to the timeout API on older runtimes. An interrupted slice resumes the same parse; cancellation resets unfinished parser state. An already edited incremental tree is retained with dirty rows invalidated for the next revision. Querying checks cancellation between matches and during multiline span expansion. This is cooperative cancellation, not a hard wall-clock guarantee: grammar/query initialization, individual predicates and a single native query step cannot be forcibly interrupted.

Interrupted queries do not publish partial valid rows. Pending full-cache resets survive a canceled viewport query until a valid batch is committed. Changes to the source continue to be replayed serially, and undelivered results are invalidated in their old coordinates before new edits are applied.

Multi-cursor edits, completion/workspace edit batches, and Undo/Redo now report individual changes with pre-edit UTF-8 byte positions. They update the persistent tree incrementally instead of requesting a fresh document snapshot. Earlier dirty intervals shift as later batch edits insert or remove rows; unaffected syntax/semantic rows remain cached. A full rebuild remains a fallback for buffer replacement, missing trees, or a mismatched edit stream.

Regression checks cover cancellation during initial parsing and querying, reuse of an interrupted incremental tree, exact recovery against a fresh parse, rapid asynchronous edits and language changes, and UTF-8/multiline Undo/Redo notifications.

## Cache and resource budgets

`HighlightLimits` sets per-document defaults (the settings dialog does not expose these yet):

| Resource | Default | On reaching the limit |
| --- | --- | --- |
| Each syntax span cache (UI and worker) | 4096 rows and 8 MiB charged memory | Evict the least recently used rows, preferring to protect the viewport |
| Estimated source copies and row offsets | 128 MiB | Pause before allocating worker snapshots if the estimated working set does not fit |
| Native syntax parse | 1500 ms, checked between 5 ms resumable slices | Release the tree/cache/parser scratch allocations; pause syntax highlighting for this revision |
| One highlight query | 50 ms cooperatively | Keep previously cached rows; cache plain text for the unfinished requested rows |
| Temporary span payload per query | 2 MiB conservative estimate | Fall back for the requested uncached rows |
| Spans on a single row | 4096 | Keep that row plain |
| Highlighted UTF-8 line length | 256 KiB | Avoid large UTF-16 highlighting allocations; keep that row plain |
| Pending edit stream | Cache byte budget (8 MiB by default), at most 4096 edits | Replace queued deltas with one fresh snapshot on the next parse |
| Concurrent syntax jobs across tabs | 2 | Queue work in a dedicated pool |

The caches are sparse maps: they no longer allocate one span-vector/validity entry per document line. Charges include container overhead, vector capacity and capture string capacity, conservatively counting implicitly shared payloads in each cache. UI and worker each have their own allowance (up to 16 MiB of charged cached spans per document in total). Results and temporary work are separately bounded by requested row counts and span budgets. Cached empty fallback rows are valid entries, so limits do not create an automatic retry loop.

Only the active working region is prefetched on large files; background scanning does not endlessly refill evicted rows. Scrolling into an evicted area schedules a new query against the existing complete syntax tree, preserving offscreen multiline context. Inserting/deleting rows shifts the sparse entries; syntax changes invalidate intersecting cached rows. A bounded forward scan tracks work independently from eviction.

A parse deadline blocks retries on scroll for the same text revision. Editing/replacing the text or changing limits allows another attempt. Query/density limits simplify only affected uncached rows. The status bar explains a limit when the active editor reaches it; messages are deduplicated per reason/revision. These fallbacks do not alter file contents. The separate LSP semantic layer adapts its workload and transfer budget as documented in [LSP.md](LSP.md).

These are cache and cooperative processing budgets, not a hard cap on process RSS or every native allocation. The complete source snapshots, line-offset index and Tree-sitter tree remain outside the span-cache charge; snapshot creation is guarded by `3 × text bytes + line count × sizeof(size_t)` against the source-working budget. This estimate covers current full text copies and row offsets, not every native tree allocation. Native grammar/query initialization, one predicate or a native query-cursor call may overrun a cooperative deadline before control returns. The editor's text storage, long-line layout, project search and LSP have separate resource costs.

Tests cover hard charged-byte/row limits, LRU protection/recency, sparse row shifts, eviction and rehydration across a large file, timeout/density/long-line fallback, retry suppression and recovery with a larger budget. Full-query equivalence tests use explicit larger test budgets rather than requiring the default cache to hold every row.

## Semantic tokens

Enabled by default. Use **Settings → Language server → Semantic highlighting (LSP)** to switch this layer off independently of completion. The server must advertise `semanticTokensProvider.full`; `workspace/semanticTokens/refresh` is supported. Superseded requests are canceled. Servers without this capability retain syntax highlighting.

The protocol client uses each server's advertised legend; it does not hard-code clangd's numeric token IDs. Standard types and relevant clangd types/modifiers map to theme roles. Unknown types preserve syntax styles. A future language server can use the same transport/decoder/renderer. Currently the settings profile exposes C/C++ with clangd.

For correct C/C++ symbols, configure `compile_commands.json`, especially for embedded projects with target-specific flags, include paths and defines. Tree-sitter alone cannot identify all parameter uses, overloaded calls or macro expansions.

## Tokyo Night

The built-in Tokyo Night adapts colors from **Tokyo Night for VS Code**, while keeping the Tree-sitter/LSP processing and existing font styles. Parameters remain yellow, user types are light, built-in types/keywords purple, fields light blue, and functions blue. Semantic type/modifier rules refine syntax; ordinary LSP variables preserve their Tree-sitter style. Custom themes can override the same color roles and font style flags.

This combines an adapted VS Code palette with the existing Neovim-style C/C++ capture/semantic mappings; it does not embed TextMate or reproduce every VS Code extension or LazyVim plugin. LazyVim can select other Tokyo Night variants, fonts, injected-language queries and user overrides. Identical appearance across all languages/configurations is not promised.

## Sources and licenses

Pinned source revisions are in `third_party/highlighting-sources.json`:

- nvim-treesitter/nvim-treesitter: `cf12346a3414fa1b06af75c79faebe7f76df080a` (C/C++ queries).
- folke/tokyonight.nvim: `cdc07ac78467a233fd62c493de29a17e0cf2b2b6` (reference for Tree-sitter and semantic highlight groups/styles).

- tokyo-night/tokyo-night-vscode-theme: `7c0f11eaef322f293621ca7befe462214b7ea468` (VS Code Night palette).

MIT licenses are retained under `third_party/nvim-treesitter`, `third_party/tokyonight`, and `third_party/tokyo-night-vscode`. To update, refresh the pinned queries/palette together with source revisions and run the syntax, theme, editor, protocol and real-clangd checks.

A regression fixture larger than 9 MiB verifies that removing the old cutoff actually produces viewport comment highlighting with bounded caches. A deliberately reduced source-working budget verifies pausing before snapshot allocation; this is a resource fallback, not the former fixed 8 MiB gate. Full-file snapshots/parsing and long-line editor layout still have costs; this stage does not implement streaming/gigabyte-file loading.

## Stable colors while typing

The UI projects its last displayed syntax spans and semantic tokens through UTF-16 edits while a fresh worker/server response is pending. Surviving prefixes/suffixes and unaffected rows retain their colors; inserted characters strictly inside a token inherit its provisional style. Deleted text drops its spans. This display cache is separate from the worker's invalidated query cache, so fresh parsing still determines the final classification. Editing code into a comment, for example, changes the colors when the new result arrives without an intermediate plain-text frame.

Normal saves and viewport range requests retain displayed colors until replacement. Loading a different buffer, disabling semantics, server/profile changes and explicit resource fallbacks still clear them. Version/generation guards continue to reject stale responses. Regression tests check styles immediately after insertion/deletion (before debounce), shifted newline rows, semantics through Undo, saving, and final replacement when syntax changes.
