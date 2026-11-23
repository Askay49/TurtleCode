# Keyboard shortcuts

The optional [Vim mode](VIM.md) changes editor typing, selection and Escape
handling. Dialogs, search fields and terminal retain their ordinary shortcuts.

Open **Settings… → Keyboard shortcuts / Настройки… → Горячие клавиши**. Search by command name, category or ID. Select a row, focus the shortcut field and press the desired combination (up to four strokes), then click **Assign**. **Clear** disables the command shortcut. **Reset selected** and **Reset all** restore defaults. Click **Save** to apply all changes immediately; **Cancel** discards the draft.

Conflicting shortcuts and shared chord prefixes are rejected. Window commands and editor commands overlap while the editor is focused, so they cannot reuse a shortcut. Text input, clipboard, undo/redo, normal cursor movement, selection and Ctrl+L line selection are fixed. Escape/Enter and navigation inside dialogs remain fixed. Advanced editor commands (extra cursors, select next occurrence, line copying and line movement) are configurable.

Only overrides are stored in QSettings under `shortcuts/<command-id>`, using portable Qt key sequences. An explicitly empty value disables a shortcut. Default assignments are not saved, so newly added commands receive their defaults. If saved settings become conflicting after an update, the application uses defaults.

Import/export use a Turtle Code JSON file, for example:

```json
{
  "formatVersion": 1,
  "shortcuts": {
    "file.open": "Ctrl+Alt+O",
    "search.replace": "Ctrl+R",
    "editor.nextOccurrence": ""
  }
}
```

Import merges the listed commands into the current draft and validates before accepting it. Export writes the complete draft. Unknown commands, reserved keys and conflicts are rejected. Imported settings take effect after **Save**.

**Ctrl+Shift+O** opens symbol search for the current file. **Ctrl+Alt+P** opens
the same dialog for project symbols. Both commands are available from the
**Go** menu and the command palette. Switch scope in the dialog, select a result
with the arrow keys, press Enter to jump, or press Escape to close.
An existing shortcut assignment takes priority over either new default.

Each editor pane shows its current document in a header. The active pane has
an accent line. Click the title to choose another open buffer for that pane.
The header provides split and close actions, and a top tab bar appears when
multiple Vim tab pages are open. Unsaved documents require confirmation before
closing. Cancelling Close All leaves every document open. See [VIM.md](VIM.md#editor-panes-vim-tab-pages-and-diff)
for pane and tab-page controls.

**Ctrl+Tab** opens an opaque panel over the editor with file icons and filenames in most-recently-used order. The editor keeps its size and scroll position while the panel is open. Keep Ctrl held and press Tab again to advance, or Shift+Tab to go backwards. Release Ctrl, press Enter, or click a row to open the selection. Escape or a click outside cancels. Arrow keys also select a row. The next/previous recent-tab shortcuts remain configurable.

**Ctrl+D** adds the next occurrence to the selection. A complete identifier
(selected explicitly or expanded from the caret) matches whole identifiers:
`adc` skips `adc_state`, `my_adc` and `adc2`. Letters, digits, underscores and
Unicode combining marks belong to identifiers. An explicitly selected fragment
inside an identifier keeps substring matching. Matching is case-sensitive.

**Escape** clears selections and cancels multiple cursors in the editor, keeping
the primary cursor at its current position. Document text and Undo history
are preserved. Completion popups and search fields retain their own Escape action.

The editor also highlights matching text automatically, using the word at the
caret or the current single-line selection and the same identifier boundaries.
This is textual highlighting and does not require a language server. Search
matches and selections are drawn above it. The theme role is `editor.occurrence`.
Only visible lines are scanned; lines over 64 KiB, queries over 4096 UTF-16 units,
multiline/whitespace-only selections and IME preedit are skipped. At most 256
visible matches per line are drawn. Highlighting never changes text or history.

**Ctrl+B** hides or restores the current left sidebar page, retaining the chosen
Explorer/Search/Git/Debug page. The activity icons remain visible. Clicking the
active icon hides its panel; clicking it again reopens it. Clicking another icon
switches to that panel. Hiding the sidebar returns focus to the editor.

**F6** focuses the active editor pane (`view.focusEditor`). **Ctrl+Shift+T**
switches focus between the editor and terminal (`view.focusTerminal`). It opens
the terminal if needed and leaves it visible when returning to the editor.

Press **Ctrl+'**, then **h** for the project tree, **j** for the terminal, or
**k/l** for the active editor. Each letter selects the same destination from
any pane. **Esc** cancels the pending transition. Change the prefix in
**Settings → Keyboard shortcuts → General → Focus navigation prefix**.

**Ctrl+Shift+E** shows and focuses the project tree. Press it again from the tree
to return to the active editor pane while keeping the tree visible
(`view.focusProjectTree`). Use Up/Down to select a row, Enter to open a file in
a regular tab and focus the editor, expand or collapse a folder, or activate
a paging row. F2 renames the selected item in the tree. In the editor, F2 invokes
symbol rename.

With Vim mode enabled and the tree focused:

| Keys | Action |
| --- | --- |
| `j` / `k` | Select the next / previous visible row. |
| `gg` / `G` | Select the first / last visible tree row on the loaded pages. |
| `h` | Collapse an expanded folder or select the parent. |
| `l` | Expand a folder, select its first visible child if already expanded, or open the selected file or paging row. |
| `a` / `A` | Create a file / folder inside the selected folder, beside the selected file, or in the project root if nothing is selected. The tree keeps focus and selects the new item if it passes the tree filters. |
| `r` | Rename the selected item. For a file with an extension, the name before the extension is selected for editing. |
| `d` / `D` | Move to Trash / permanently delete. Permanent deletion always asks for confirmation and defaults to **No**. Both commands apply to items inside the project. |
| `Esc` | Cancel a pending `g` or return focus to the editor. |

After deletion the tree selects the next sibling, the previous sibling, or the
parent if the folder is empty. Use **Delete** or the context menu to remove the
root folder from the workspace.

With the tree focused, **Ctrl+C**, **Ctrl+X** and **Ctrl+V** copy, cut and
paste files or folders. Paste uses the selected folder, the selected file's
parent, or the project root. The context menu also offers **Duplicate**.

With the tree focused, **Delete** moves the selected file or folder to the
system Trash. The confirmation opens with **Yes** focused. Press **Enter** to
continue or **Esc** to cancel. You can turn off the prompt under **Settings →
Files and search → Confirm before moving items to Trash**. On the project root,
**Delete** asks to remove the folder from the workspace and keep its contents
on disk. **Delete Permanently** is available from the context menu for files
and folders inside the project.

Line operations are available in **Code / Код**:

| Command | Default shortcut |
| --- | --- |
| Copy line up / Скопировать строку вверх | Shift+Alt+Up |
| Copy line down / Скопировать строку вниз | Shift+Alt+Down |
| Move line up / Переместить строку вверх | Alt+Up |
| Move line down / Переместить строку вниз | Alt+Down |

Without a selection, the current line is used. A selection uses complete touched
lines; ending at the next line's column zero excludes that next line. Adjacent or
overlapping ranges from multiple cursors are processed as one block. Selection
orientation and cursor columns follow the copied/moved text. Each command is one
Undo step. Moves beyond the document boundary do nothing. BOM stays at the start,
CRLF is preserved, and a missing final newline is handled when copying/moving the
last line. Affected ranges of 1 MiB or more use the existing cancellable background
editing path. Undo restores text and cursors; as with other commands, the existing
history does not store selection anchors.

**Ctrl+Shift+I** formats the selected block or the whole document when there is
no selection. The command is under **Code → Format document or selection** and
keeps the ID `code.formatDocument`. Change the shortcut in **Settings… → Keyboard shortcuts**.

Smart editing: **Ctrl+/** toggles line comments, **Ctrl+Shift+A** toggles block
comments. Command IDs: `editor.toggleLineComment`, `editor.toggleBlockComment`.
Tab/Shift+Tab indent/unindent selected lines and remain fixed editing keys.
See `SMART_EDITING.md` for auto-pairs, surrounding selections, Enter and matching brackets.

## Command palette

**Ctrl+Shift+P** or **Go → Command palette** opens a compact command search above
the editor. Search by translated name, English name or command ID; multiple words
and abbreviated matches are supported. Up/Down selects a result, Enter runs it,
Escape cancels, and clicking a result runs it. Focus returns before execution;
editor commands use the current document and cursor. Disabled commands show a
reason and cannot run. Toggle commands display their current state. The list
displays current user-assigned shortcuts, including commands with no shortcut.

An empty query puts recently invoked commands first. Up to 20 command IDs are
stored locally in QSettings under `commands/recent`. The palette shortcut is
configurable under Settings… → Keyboard shortcuts. Ctrl+P still searches files.
