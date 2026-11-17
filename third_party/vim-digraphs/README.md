# Vim digraphs

The built-in digraph table was exported from unmodified Neovim v0.9.4 using
`vim.fn.digraph_getlist(true)`. Neovim inherits this table from Vim.
Upstream: https://github.com/neovim/neovim/blob/v0.9.4/src/nvim/digraph.c
The Vim license is included in COPYING and installed with the application.
Neovim is not a runtime or build dependency.

`digraphs.json` is the source data. Run `python3 third_party/vim-digraphs/generate.py`
from the repository root to regenerate `src/editor/VimDigraphs.hpp`.
