"""Generate the Qt lookup table from the vendored Vim digraph data."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
data = json.loads((Path(__file__).parent / "digraphs.json").read_text())
rows = []
for pair, value in data:
    if len(pair) == 2 and len(value) == 1:
        rows.append(f"{{{ord(pair[0]) * 65536 + ord(pair[1])},{ord(value)}}},")
header = """#pragma once
// Generated from third_party/vim-digraphs/digraphs.json. See its COPYING.
#include <QHash>
inline const QHash<unsigned int,char32_t>& vimDigraphs(){static const QHash<unsigned int,char32_t> data={
""" + "\n".join(rows) + "\n};return data;}\n"
(root / "src/editor/VimDigraphs.hpp").write_text(header)
