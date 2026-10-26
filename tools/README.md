# tools/

Build, test and measurement scripts for the repository, the sources of
the `kai` tool itself (`tools/kai/`) and its helpers, and editor
support files. This page covers the editor support files: static
assets, not wired into the build, that you install into your editor
of choice. For the language server, see `extensions/vscode/` and
`kai lsp --help`.

## Vim / Neovim

`kaikai.vim` is a traditional Vim 8+ / Neovim syntax file. To install
for the current user:

```sh
mkdir -p ~/.vim/syntax ~/.vim/ftdetect
cp tools/kaikai.vim ~/.vim/syntax/kaikai.vim
cat > ~/.vim/ftdetect/kaikai.vim <<'EOF'
autocmd BufNewFile,BufRead *.kai set filetype=kaikai
EOF
```

Neovim users can substitute `~/.config/nvim/syntax` and
`~/.config/nvim/ftdetect`. After reloading Vim, `:set ft?` in a
`.kai` buffer should report `filetype=kaikai`.

To verify the file loads cleanly without installing:

```sh
vim -u NONE -c "set ft=kaikai" -c "syntax on" \
    -c "source tools/kaikai.vim" examples/minimal/hello.kai
```

## VSCode / Sublime Text / TextMate

`kaikai-syntax.json` is a TextMate-format grammar (the same format
GitHub uses for web rendering). It maps tokens to the standard
TextMate scope names so any colour theme picks them up.

For a one-off VSCode install, create a minimal extension layout:

```text
~/.vscode/extensions/kaikai-syntax/
├── package.json
├── language-configuration.json
└── kaikai-syntax.json
```

`package.json` minimum:

```json
{
  "name": "kaikai-syntax",
  "version": "0.1.0",
  "engines": { "vscode": "^1.60.0" },
  "contributes": {
    "languages": [{
      "id": "kaikai",
      "aliases": ["kaikai", "kai"],
      "extensions": [".kai"]
    }],
    "grammars": [{
      "language": "kaikai",
      "scopeName": "source.kaikai",
      "path": "./kaikai-syntax.json"
    }]
  }
}
```

Reload VSCode (`Developer: Reload Window`) and open a `.kai` file.

Sublime Text 4 reads TextMate grammars under `Packages/User/`; copy
the JSON there and rename the extension to `.sublime-syntax` if
needed.

## Limitations

These files are **purely heuristic**. They categorise tokens by shape
(capitalisation, operator characters, keyword tables) without any
type or scope information:

- A capitalised identifier highlights as a constructor whether it is
  one or not (e.g. inside an `import` path).
- Effect names in the catalog list always highlight as effects, even
  when the same identifier shadows a user-defined value.
- The keyword list is a copy of the reserved words `kai info syntax`
  lists; when that list changes, update both files.

Type- and scope-aware information comes from the language server
(`kai lsp`), not from these files.
