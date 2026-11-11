# Themes

Open **Settings → Theme / Тема оформления…**. Selecting a theme applies it immediately and remembers it for the next launch. Double-click a color to preview changes. Use **Save custom copy…** to give your theme a name and keep it. Unsaved color previews revert when the window closes. Export includes the currently previewed colors.

Ten built-in palettes are adapted for Turtle Code from the theme families listed at https://htmlacademy.ru/blog/soft/vs-code-themes : C/C++, One Dark Pro, GitHub, Dracula, Winter is Coming, Monokai Pro, Night Owl, Ayu, Shades of Purple, and Noctis. These are independent color adaptations, not bundled VS Code extensions or complete reproductions of their variants, token rules, or typography. Monokai is explicitly labeled “inspired”.

Custom themes are stored under `QStandardPaths::AppConfigLocation/themes` (normally `~/.config/Turtle Code/Turtle Code/themes` on Linux). The selected ID is stored in QSettings under `appearance/theme`. Import makes a separate local copy with a unique ID, so duplicate names never overwrite files.

**Import JSON…** accepts Turtle Code theme JSON (not arbitrary VS Code theme files). **Export JSON…** writes a standalone theme with all colors. Format:

```json
{
  "formatVersion": 1,
  "name": "My theme",
  "colors": {
    "editor.background": "#202020",
    "text.primary": "#eeeeee",
    "syntax.keyword": "#c586c0"
  }
}
```

Colors use `#RRGGBB`. Missing roles inherit the built-in C/C++ Dark defaults, unknown roles are ignored, unsupported format versions and invalid colors are rejected. Input size is limited to 1 MiB. Export a built-in theme to get the complete list of supported color roles. User files are written atomically.

Tokyo Night uses a palette adapted from [Tokyo Night for VS Code](https://github.com/tokyo-night/tokyo-night-vscode-theme): light user types, purple built-in types/keywords, blue fields, and VS Code editor/selection colors. Translucent colors are flattened onto the editor background. Its existing Neovim-style Tree-sitter/LSP classification and font styles are retained. The theme ID remains `tokyo-night`; existing custom copies keep their saved colors. See [HIGHLIGHTING.md](HIGHLIGHTING.md).

Select a custom theme and click **Delete theme…** to remove its saved JSON file after confirmation. Built-in themes cannot be deleted. Deleting the selected custom theme switches to the built-in C/C++ Dark theme and persists that selection. Canceling or a file removal error leaves the theme intact.

Optional `fontStyles` maps color roles to integer flags: bold=1, italic=2, underline=4, strike-through=8 (combine with addition). For example, `"fontStyles": {"syntax.comment": 2}` makes comments italic. Old files without this field remain valid. Export/import preserves these styles; the color dialog edits colors.


## Workspace background image

Use **Settings → Editor… → Workspace background** to choose an image, set its opacity (0–100%, default 15%), or remove it. Save applies the changes to all open windows; Cancel leaves them untouched. The file path and opacity persist in QSettings as `appearance/backgroundImage` and `appearance/backgroundOpacity`. The image is referenced in place, so keep the source file available. An unavailable image on startup falls back to the regular theme.

The image fills the entire editor window behind the project tree, search panel, tab header, line numbers, editor text, menu bar and status bar. It is centered, preserves its aspect ratio and crops overflow. All surfaces share workspace coordinates, including after resizing or moving the sidebar divider; document scrolling does not move the image. Current-line highlighting becomes translucent while the image is enabled. Popup menus, dialogs, selections and input fields retain readable backgrounds. Image formats follow the installed Qt image plugins.
