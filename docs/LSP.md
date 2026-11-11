# Language server support

The initial server profile is **clangd for C/C++**. Tree-sitter grammars remain independent: they provide coloring, while clangd provides completion, diagnostics and code navigation.

## Settings

Open **Settings → Language server / Языковой сервер…**. The dialog lists every
language in `LanguageRegistry`, including languages without a configured server.
Select a language and assign a server profile, or choose **No server**. A profile
can serve several languages (for example, C and C++ share clangd).

- **Add profile** creates an empty profile; set its name, executable and arguments.
  Servers are installed by the user. Turtle Code does not download/install them.
- Enable/disable servers globally or individual profiles. The executable may be
  an absolute path or a command resolved through PATH. Arguments use Qt command
  splitting; quote paths containing spaces. Runtime launchers are supported by
  selecting the runtime executable and placing the server entry point in arguments.
- Existing clangd settings are loaded automatically until profiles are first
  changed/saved. The clangd profile preserves the compilation database directory
  through its `--compile-commands-dir=` argument. Its special directory field is
  shown only for the profile with ID `clangd`; arbitrary profiles can supply the
  same argument themselves.
- **Advanced** provides an optional LSP language ID override for the selected
  language, `initializationOptions`, and server settings as JSON objects. Settings
  are sent through `workspace/didChangeConfiguration` after initialize and returned
  by section for `workspace/configuration` requests. Settings are uniform within
  a profile; scope-specific overrides are not implemented.
- **Check connection** tests the current draft in an isolated temporary client
  using the selected workspace directory. It reports initialization and advertised
  completion/hover/definition/signature/rename/formatting/semantic capabilities.
  It does not open a document, prove editor compatibility, or save the draft.
  Test processes end on profile/language change or when the dialog closes.
- **Restart server** restarts the selected profile's live instances and an active
  test instance. **Clear log** clears that profile's live/test buffers.
- Save applies changes; changes to profiles/bindings restart the configured clients.
  Changing only log levels updates buffers without restarting servers.
  Cancel discards the draft. Check/restart/clear are explicit
  runtime actions and are not undone by Cancel.
- Auto completion, semantic highlighting and completion delay remain independent
  global options. Delay is 50–2000 ms (default 150 ms).

### Routing and workspace roots

`LspManager` owns clients by profile and workspace root. It routes open/change/
save/close, requests, capabilities and document versions to the corresponding
client, including background timers. Profiles for different languages run
concurrently; switching tabs does not restart or replace a server. Diagnostics
and server restart/semantic refresh affect only the documents of that instance.
Rename checks document versions in the originating server, never another server's
version counter.

Language detection uses `LanguageRegistry`, including `CMakeLists.txt`, Makefile,
Dockerfile variants, `meson.build`, `nginx.conf` and `.editorconfig`. JSX/TSX use
`javascriptreact`/`typescriptreact`; Bash uses `shellscript`, assembly `asm`, and
`.editorconfig` uses `editorconfig`. Language ID overrides are per language,
not per individual file. Tree-sitter enablement is independent of LSP routing.

New documents inside the selected project use that root. For standalone or
outside-project files, the manager searches ancestors for common project markers
and otherwise uses the containing directory. Existing open documents retain their
server/root when a different project is selected. Idle instances can be reused
for another root. No universal language-specific root detection is promised.

Profiles and bindings are stored as JSON in QSettings `lsp/profiles`; earlier
`lsp/clangd/*` values remain as migration inputs. No server is auto-installed and
unconfigured languages remain usable with their existing syntax highlighting.

### Bounded in-memory logging

Logging is configurable per profile:

| Level | Retained data per server/root instance |
| --- | --- |
| Off | Nothing; existing buffers are cleared |
| Normal (default) | At most **16 KiB UTF-8**: 8 KiB errors + 8 KiB events/stderr |
| Detailed | At most **1 MiB UTF-8** total, including the normal buffers |

Logs are **never automatically written to disk** and disappear when the client
is destroyed/application exits. Old entries are evicted. Normal event traffic
cannot evict the separate error buffer; new errors can replace old errors once
that buffer fills. Each entry has a local date/time. stderr error/fatal/exception
lines and clangd `E[` lines are classified as errors on a best-effort basis.
Individual normal entries are truncated. Unsupported client requests are recorded
as errors so missing compatibility is visible without a protocol trace.

Detailed mode includes incoming/outgoing JSON, cancellations, ignored replies and
response durations. A JSON entry is truncated at about 64 KiB. It may contain
source text and settings. Returning to Normal immediately clears the trace buffer.
The limits are per instance, not per project aggregate. The log tab displays each
instance's root; an isolated connection check has its own bounded log.

## Editor actions

These defaults can be changed under **Settings → Keyboard shortcuts**:

| Action | Default |
| --- | --- |
| Complete manually | Ctrl+Space |
| Go to definition | F12 |
| Rename symbol | F2 |
| Symbol information | F1 |
| Function parameters | Ctrl+Shift+Space |
| Errors and warnings | Ctrl+Shift+M |

Completions appear beside the cursor. Choose with arrow keys and Enter, dismiss with Escape, or click an item. The server's replacement range and additional text edits are applied together as one Undo operation. Overlapping or invalid UTF-16 edits are rejected. Snippet placeholders are not supported and are explicitly disabled in client capabilities. Symbol information also appears on hover; diagnostic messages take precedence. Double-click an entry in Problems to navigate to it.

## Protocol and architecture

`LspClient` handles a single configurable server/workspace instance. `LspServerConfig` contains executable, arguments, language IDs by file extension and initialization options. The transport and synchronization contain no clangd-specific protocol logic. This separates the existing C/C++ settings profile from the reusable protocol client. The `LspManager` router owns multiple instances and the settings dialog exposes user-defined profiles for every registered language. Actual compatibility must be checked per server; only supported protocol features are advertised.

Initialization waits for the process to start and checks the initialize response. UTF-16 positions are negotiated explicitly. Incremental synchronization is used when the server requests it, including surrogate-pair-safe changes; full synchronization is also supported. Requests are canceled when superseded or when the document changes/closes. Versioned responses and diagnostics from earlier document versions are discarded, and the UI additionally checks the editor revision and cursor location before showing completion/navigation results. Requests have timeouts. Unexpected server termination retries at most twice; manual restart resets that limit. Shutdown waits briefly for the server acknowledgment before sending exit, with a bounded termination fallback.

The client supports completion, definition, hover, signature help, prepareRename/rename, semanticTokens/full, semanticTokens/range and publishDiagnostics, as well as didOpen/didChange/didSave/didClose. Returned rename workspace edits support both `changes` and versioned `documentChanges`. Snippets, file creation/deletion/renaming, annotated edits and unsolicited `workspace/applyEdit` requests are not advertised. Code actions are a future feature. Document formatting uses `textDocument/formatting` when the server advertises `documentFormattingProvider`.

## Rename (F2)

Place the cursor on a symbol, press **F2**, enter its new name, and review the changes grouped by file. **Apply** changes all references returned by the language server. Canceling either dialog leaves the text unchanged. This is semantic rename, so unrelated symbols with the same spelling are not intentionally replaced.

All affected files become open, modified tabs, including files that were previously closed. **The operation does not write files to disk automatically.** Save the changed files normally, or use **File → Save All / Сохранить всё** (**Ctrl+Alt+S**) to save all modified tabs. Each file's entire rename is one Undo step; Ctrl+Z is per file, not a workspace-wide undo command. Existing unsaved edits are preserved and are synchronized before requesting rename.

Every target is validated before any edits are applied. Document versions, editor revisions and exact source bytes are checked; unopened files are checked against the symbol text and checked again against their staged bytes after preview. Invalid UTF-16 positions, overlapping edits, unsupported resource operations, read-only targets or changed files abort the operation. UTF-8 BOM and CRLF line endings are preserved. Results depend on the server's project index and compilation database, so configure clangd's `compile_commands.json` correctly for cross-file references.

## Checks

`lsp-manager` checks independent clients, roots, shared profiles, language IDs,
configuration sections, legacy settings migration, connection probes and bounded
logs under flooding. `lsp-window` checks a mixed-language window, independent
capabilities/restarts, dialog assignments, Save/Cancel and connection checks.
`lsp-client` exercises the installed clangd (when available). `lsp-protocol` uses a local Python test peer and covers custom language mappings, fragmented messages, string request IDs, configuration requests, initialization errors, Unicode incremental edits, cancellation, stale diagnostics, save notifications and restart. Editor checks cover popup selection/cancellation, Unicode edit ranges, additional edits and Undo. No network is required for these checks.

## Semantic highlighting

The semantic layer is enabled by default and can be disabled independently in the language server settings. It refines Tree-sitter syntax using the server's token legend and works with unsaved buffers. See [HIGHLIGHTING.md](HIGHLIGHTING.md) for scheduling, validation, theme rules and limitations.

## Adaptive workload for large documents

The policy is generic and per document (`LspAdaptivePolicy`), using raw UTF-8 size and observed end-to-end semantic request/decode latency:

| Document size | Automatic behavior | Manual commands |
| --- | --- | --- |
| Up to 512 KiB | Full semantic tokens, or ranges for a range-only/slow server | Available |
| 512 KiB–2 MiB | Prefer viewport ranges; otherwise full semantic tokens | Available |
| 2–16 MiB | Range semantics only when advertised; otherwise Tree-sitter syntax without automatic LSP colors. Automatic completion/hover are suppressed; document sync waits at least 1 second of inactivity | Available |
| 16–32 MiB | Automatic server attachment is deferred | A manual command attaches/synchronizes the complete document, starts the server if needed, and retries once initialization completes |
| Above 32 MiB | No document transfer | Blocked by the transfer-copy budget with a status message |

The 32 MiB transport budget is separate from syntax highlighting and is intentionally retained while LSP synchronization sends whole-document snapshots/diffs. It is not a guarantee that every server can handle that input efficiently. Requests for a viewport still require the server to know/analyze the whole document. Server CPU/RSS cannot be capped by this client policy.

Sync delays adapt from 200 to 2000 ms; semantic request intervals from 200 to 4000 ms. Semantics use a 2500 ms request deadline checked by the existing deadline timer. Three pressured replies, two failures, a long failed request, or an excessive token count pause automatic semantics for 30 seconds. Successful cold-start latency is capped in the initial average and alone does not trigger a pause. Later successful replies lower the average, allowing automatic features to recover. A paused active document schedules a retry; editing/closing cancels obsolete semantic requests immediately. Server refresh notifications survive throttling and pending requests.

Ranges include the viewport plus 80 lines on each side. They replace the previous semantic layer on application and are re-requested after scrolling, with version/generation/window checks. Range replies use the server's legend and absolute line deltas, and are decoded against only the requested text slice. The slice is read in one operation, not by rescanning each line from the document start. Full results are limited to 50,000 tokens, range results to 20,000, with invalid/oversized results contributing to backoff. Manual completion, definition, hover/signature commands and rename bypass automatic semantic pauses. Disabled LSP retains no document copies.

Tests use full-only and range-only peers, Unicode range decoding, deterministic policy latency/failure inputs, a 3 MiB document with/without range support and viewport scrolling, and a 17 MiB document activated by a manual definition request. The real clangd check remains enabled when clangd is installed.

## Document formatting

Use **Code → Format document** or **Ctrl+Shift+I**. This formats the entire current
saved-path document using its language server (the profile assigned to the current document).
Unsaved edits in that document are sent before the request; the file is not saved
automatically. The command can initialize the server on demand, subject to the
existing 32 MiB LSP transport budget. New untitled files must first be saved with
a supported extension. A missing/disabled server or unsupported capability produces
a status message.

Formatting options request four-space indentation (`tabSize: 4`, `insertSpaces:
true`), matching editor Tab behavior. The formatter determines the code style;
clangd can use the project's `.clang-format`. No formatting on save or selection
formatting is provided by this command.

Results are checked against the original document and revision, path, current tab,
and request generation. Replies to changed/closed/switched/busy documents are
ignored. Invalid or overlapping UTF-16 edits are rejected before any change.
UTF-8 BOM and the existing line-ending convention (chosen from the first LF/CRLF)
are retained. Unchanged replacements are removed so a no-op does not create an Undo
step. All remaining edits are one Undo step; Ctrl+Z preserves earlier unsaved edits,
and Ctrl+Y restores formatting. Cursor byte positions are mapped through edits;
selections are cleared, as for other workspace edits.

The `formatting` CTest exercises the UI shortcut, delayed replies, tab switching,
closing, unsaved edits, Undo/Redo, Unicode/BOM/CRLF, no-op/null replies, malformed
edits, unsupported servers and protocol errors. `lsp-protocol` checks the wire
request and cancellation; `lsp-client` also requests formatting from an installed
clangd when available.
