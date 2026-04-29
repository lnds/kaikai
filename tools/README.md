# tools/

Editor support files for kaikai. None of these are wired into the
build; they are static assets you install into your editor of choice.

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

```
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
- The keyword list tracks `stage2/compiler.kai`. If the lexer's
  keyword set drifts, this file drifts with it; bump in lockstep.

Real scope-aware highlighting requires the kaikai LSP, which is
scheduled for milestone m17. Tree-sitter and `kai fmt` are also
out of scope here — see the project roadmap in `docs/design.md`.
