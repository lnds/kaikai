# lsp

The kaikai Language Server (`kai lsp`) for editor integration.

## Description

`kai lsp` speaks LSP JSON-RPC over stdio and is the integration
point for every editor — VS Code, Helix, Neovim, Emacs (eglot /
lsp-mode), Sublime LSP, Zed. The server itself is written in
kaikai (`tools/kai-lsp/main.kai`, ~500 lines) and shells out to
`kaic2 --library-mode` per request.

```sh
kai lsp        # block on stdio; reads JSON-RPC frames
```

End users do not usually run `kai lsp` directly — editor extensions
spawn it. See *Editor configuration* below.

## What ships today

| LSP method                        | Supported |
| --------------------------------- | --------- |
| `initialize` / `initialized`      | yes       |
| `shutdown` / `exit`               | yes       |
| `textDocument/didOpen`            | yes       |
| `textDocument/didChange` (Full)   | yes       |
| `textDocument/didClose`           | yes       |
| `textDocument/hover`              | yes       |
| `textDocument/definition`         | yes       |
| `textDocument/publishDiagnostics` | partial   |
| `textDocument/documentSymbol`     | yes       |
| `textDocument/completion`         | yes       |
| `textDocument/signatureHelp`      | yes       |

- **Hover** returns the inferred kaikai type for the AST node under
  the cursor, formatted as a fenced markdown block.
- **Goto-definition** points at the source line of the resolved
  identifier; user-authored decls win over prelude / stdlib.
- **publishDiagnostics** fires on `didOpen` and `didChange`. Driven
  by `--diags-json` for compile errors and by `--holes-json` for
  unfilled typed holes (each hole becomes one Warning-severity
  diagnostic carrying the inferred type).
- **documentSymbol** surfaces `DFn` (incl. lowered `const`),
  `DEffect`, `DProtocol`, `DUnit`, `DTest`, `DBench`.
- **completion** offers user fns + stdlib + prelude builtins
  (~445 items by default; top-level only — locals / params /
  field completion are LSP v4 work).
- **signatureHelp** highlights the callee signature; `activeParameter`
  is currently static at index 0.

## Editor configuration

### VS Code

A minimal extension lives at `extensions/vscode/`:

```sh
cd extensions/vscode
npm install
npx vsce package          # produces kaikai-lsp-0.1.0.vsix
code --install-extension kaikai-lsp-0.1.0.vsix
```

The extension activates on `.kai` files and spawns `kai lsp` from
`$PATH`. Override the binary with the `kaikai.lspPath` setting.

### Helix

Add to `~/.config/helix/languages.toml`:

```toml
[language-server.kaikai]
command = "kai"
args = ["lsp"]

[[language]]
name = "kaikai"
scope = "source.kaikai"
file-types = ["kai"]
language-servers = ["kaikai"]
```

### Neovim (nvim-lspconfig)

```lua
local lspconfig = require("lspconfig")
local configs = require("lspconfig.configs")

if not configs.kaikai then
  configs.kaikai = {
    default_config = {
      cmd = { "kai", "lsp" },
      filetypes = { "kaikai" },
      root_dir = lspconfig.util.root_pattern("kai.toml", ".git"),
      single_file_support = true,
    },
  }
end
lspconfig.kaikai.setup({})
```

### Emacs (eglot, >= 29)

```elisp
(add-to-list 'eglot-server-programs '(kaikai-mode . ("kai" "lsp")))
```

## Environment variables

| Variable       | Purpose                                            |
| -------------- | -------------------------------------------------- |
| `KAILSP_KAIC2` | Override the `kaic2` binary path used for probes. |

The `kai` wrapper sets `KAILSP_KAIC2` automatically. Override only
when running the LSP outside the wrapper.

## Known limitations

- ASCII-only positions. LSP uses UTF-16 code units; kaikai uses byte
  offsets. Non-ASCII content misaligns for multi-byte characters.
- No cross-file resolution. Hover and goto-def follow user-authored
  decls in the current file; cross-module is LSP v4.
- No incremental sync. Every `didChange` ships the full text.
- One probe per request — no caching between hovers / completions.
- Completion is top-level only (no locals, no params, no `.` field
  completion).
- `activeParameter` is static at index 0 in signature help.
- `DType` decls are dropped before `documentSymbol`.
- Diagnostic coverage is partial — only T1–T5 *error* diagnostics
  migrated to the structured collector are visible (~11 of 239 emit
  sites); the rest still print to stderr.
- No references, rename, workspace symbols, or inlay hints.

## NOT IN KAIKAI

- A long-running daemon mode. The current LSP forks `kaic2` per
  request; in-process daemon mode is sketched in #447 but not
  shipped.
- WebSocket or TCP transport. Stdio JSON-RPC only.
- Configuration via `workspace/configuration`. The LSP reads no
  workspace settings; behaviour is fixed per release.

## See also

`kai info syntax`, `kai info holes`, `docs/lsp.md` (full reference
including troubleshooting and roadmap).
