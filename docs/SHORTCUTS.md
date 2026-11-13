# Keyboard shortcuts

Open **Settings → Keyboard shortcuts / Горячие клавиши…**. Search by command name, category or ID. Select a row, focus the shortcut field and press the desired combination (up to four strokes), then click **Assign**. **Clear** disables the command shortcut. **Reset selected** and **Reset all** restore defaults. Click **Save** to apply all changes immediately; **Cancel** discards the draft.

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

The editor header shows only the current document, without a close button. The real tab starts at the right edge of the line-number gutter, with equal 12-pixel text padding on each side. A separate header gutter continues the line-number background up to the menu. The boundary follows changes to font size or the number of line-number digits. Right-click its title for **Close** or **Close All**. Unsaved documents require confirmation; cancelling Close All leaves every document open.

**Ctrl+Tab** opens a list of open files above the center of the window in most-recently-used order. Keep Ctrl held and press Tab again to advance, or Shift+Tab to go backwards. Release Ctrl, press Enter, or click a row to open the selection; Escape or a click outside cancels. Arrow keys also select a row. The compact list displays filenames only, without a heading, paths or keyboard hints. The next/previous recent-tab shortcuts remain configurable.

**Ctrl+D** adds the next occurrence to the selection. A complete identifier
(selected explicitly or expanded from the caret) matches whole identifiers:
`adc` skips `adc_state`, `my_adc` and `adc2`. Letters, digits, underscores and
Unicode combining marks belong to identifiers. An explicitly selected fragment
inside an identifier keeps substring matching. Matching is case-sensitive.

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

Document formatting: **Ctrl+Shift+I**, command ID `code.formatDocument`, menu Code → Format document. The shortcut can be changed in Settings → Keyboard shortcuts.

Smart editing: **Ctrl+/** toggles line comments, **Ctrl+Shift+A** toggles block
comments. Command IDs: `editor.toggleLineComment`, `editor.toggleBlockComment`.
Tab/Shift+Tab indent/unindent selected lines and remain fixed editing keys.
See `SMART_EDITING.md` for auto-pairs, surrounding selections, Enter and matching brackets.
