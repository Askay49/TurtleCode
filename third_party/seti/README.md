# Seti file icons

Seti UI by Jesse Weed and contributors: https://github.com/jesseweed/seti-ui
License: MIT (`LICENSE`).

The file icon theme (`vs-seti-icon-theme.json`) and original WOFF are vendored
from VS Code's built-in Seti theme. `ThirdPartyNotices.txt` and `LICENSE-VSCODE`
preserve the upstream notices. `seti.ttf` is an equivalent sfnt font unwrapped
from that exact WOFF with the included stdlib-only `extract_ttf.py`; glyph outlines
and font tables are unchanged (sfnt offsets/padding/checksum are regenerated).
The theme's `version` references Seti UI, but its checked-in upstream TTF has a
different codepoint mapping. Do not substitute that upstream TTF for this one.

`language-associations.json` is derived from `contributes.languages` in VS Code's
bundled extension manifests. Seti uses language IDs for common extensions such
as .cpp and .py, so its fileExtensions table alone is not sufficient. The derived
map supplies those associations without depending on Turtle Code grammar enablement
or VS Code being installed. It contains extension and exact filename associations;
content/shebang detection and VS Code user file association settings are not used.

`SOURCE.json` records source URLs, revisions and SHA-256 hashes of downloaded
assets and source language manifests. All runtime data is embedded as Qt resources;
no network access or font installation is needed at build/run time.

The renderer respects Seti's 150% font size and uses its light/dark colors based
on the application's current background. File matching order is exact filename,
longest matching compound extension, language association, then the default icon.
Folders and application controls continue using Codicons.
