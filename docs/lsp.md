# Language Server (`kai lsp`)

> v3 status (2026-05-20): hover, goto-definition, publishDiagnostics,
> documentSymbol, **completion**, and **signatureHelp** all ship.
> See issue #447 for the roadmap.

`kai lsp` is the kaikai Language Server. It speaks LSP JSON-RPC
over stdio and is the integration point for every editor — VS
Code, Helix, Neovim, Emacs (eglot/lsp-mode), Sublime LSP, Zed,
etc.

```
$ kai lsp        # block on stdio; reads JSON-RPC frames
```

## What v3 does

| LSP method                        | v3 support |
| --------------------------------- | ---------- |
| `initialize` / `initialized`      | ✅          |
| `shutdown` / `exit`               | ✅          |
| `textDocument/didOpen`            | ✅          |
| `textDocument/didChange` (Full)   | ✅          |
| `textDocument/didClose`           | ✅          |
| `textDocument/hover`              | ✅          |
| `textDocument/definition`         | ✅          |
| `textDocument/publishDiagnostics` | ✅ (partial) |
| `textDocument/documentSymbol`     | ✅          |
| `textDocument/completion`         | ✅          |
| `textDocument/signatureHelp`      | ✅          |

* **Hover** returns the inferred kaikai type for the AST node under
  the cursor, formatted as a markdown fenced block. Driven by the
  `type_at` query in `--library-mode` (issue #454).
* **Goto-definition** points at the source line of the resolved
  identifier. User-authored decls always win over preloaded
  prelude / stdlib decls — `find_decl_def` prefers
  `module_origin = None` matches.
* **publishDiagnostics** fires on `didOpen` and `didChange`. Driven
  by `--diags-json` for compile errors and by `--holes-json` for
  unfilled typed holes. Each hole becomes one Warning-severity
  diagnostic carrying the inferred type — e.g.
  `unfilled hole ?conversion: expected Real<F>` — so editors can
  underline the `?` and agents can read the expected type without
  shelling out. Only the T1–T5 *error* diagnostics migrated to the
  structured collector are visible (~11 of 239 emit sites — the
  rest still print to stderr). An empty array publishes on a clean
  buffer so previous markers clear.
* **documentSymbol** walks the parser-level decls and emits the
  outline panel content. `DFn` (incl. lowered `const`),
  `DEffect`, `DProtocol`, `DUnit`, `DTest`, `DBench` are
  surfaced. `DType` is dropped by the elaborator and does not
  appear in v2 — pending follow-up.

## Architecture

```
┌──────────┐     stdio JSON-RPC    ┌────────────────┐
│ Editor   │ ◄───────────────────► │ kai-lsp        │
└──────────┘                       │ (kaikai)       │
                                   └───────┬────────┘
                                           │ fork+exec per hover
                                           ▼
                                   ┌────────────────┐
                                   │ kaic2          │
                                   │ --library-mode │
                                   └────────────────┘
```

Each hover writes the current in-memory text plus a synthetic
`# @probe type L:C` comment to a tempfile and invokes
`kaic2 --library-mode <tmp>` as a child process. The compiler
returns JSON with the AST type at that probe; the LSP wraps it in
a `Hover` response and writes it back.

Per-hover cost on macOS is roughly 5–15 ms. The subprocess model
is intentional: the alternative is linking the ~60 kLOC of stage 2
into the LSP binary, which would double its size and couple LSP
releases to compiler internals. Issue #447 sketches a long-term
in-process daemon mode; v1 ships the subprocess wrapper.

The LSP itself is written in kaikai (`tools/kai-lsp/main.kai`,
~500 lines) and uses the `Stdin`, `Stdout`, `File`, `Process`,
and `Env` capabilities.

## Editor configuration

### VS Code

A minimal extension lives at `extensions/vscode/`:

```bash
cd extensions/vscode
npm install
npx vsce package          # produces kaikai-lsp-0.1.0.vsix
code --install-extension kaikai-lsp-0.1.0.vsix
```

The extension activates on `.kai` files and spawns `kai lsp` from
your PATH. Override the binary with the `kaikai.lspPath` setting.

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

### Emacs (eglot, ≥ 29)

```elisp
(add-to-list 'eglot-server-programs '(kaikai-mode . ("kai" "lsp")))
```

## Known v3 limitations

The v1 limitations around goto-def and publishDiagnostics are
closed (see the status sidebar above). The limitations that
remain:

* **ASCII-only positions.** LSP uses UTF-16 code units for line
  characters; kaikai uses byte offsets. Files with non-ASCII
  content will misalign for multi-byte characters. Tracked in
  #447 step 4.
* **No cross-file resolution.** A hover on an imported symbol
  resolves only if its definition is part of the open document.
  Goto-definition follows user-authored decls in the current file
  (prefers `module_origin = None` matches) but cross-module
  resolution is LSP v4 work.
* **No incremental sync.** Every `didChange` ships the full text.
  Acceptable for files under a few hundred KB.
* **One probe per request.** No caching between hovers /
  completions / signature lookups; each request re-types the
  file. Acceptable while compile time stays under 20 ms for
  sub-1 kLOC files; #452 Phase B (user-file incremental cache)
  is the long-term answer.
* **Completion is top-level only.** User fns + stdlib + prelude
  builtins (~445 items by default). Locals, params, and field
  completion after `.` are not yet wired — `.` is announced as a
  trigger but the server returns the same top-level set.
* **`activeParameter` is static in signature help.** Hardcoded to
  index 0; the dynamic tracking that highlights the current
  argument is not yet implemented.
* **`DType` decls are not in the outline.** The elaborator drops
  them before `documentSymbol` walks the decl list. Pending
  follow-up on the parser-vs-elaborator split.
* **Diagnostic coverage is partial.** Only the T1–T5 *error*
  diagnostics migrated to the structured collector are visible
  (~11 of 239 emit sites); the rest still print to stderr.
* **No references, rename, workspace symbols, or inlay hints.**
  All deferred to LSP v4.

## Environment variables

| Variable        | Purpose                                                 |
| --------------- | ------------------------------------------------------- |
| `KAILSP_KAIC2`  | Override the `kaic2` binary path used for probes.       |

The `kai` wrapper sets `KAILSP_KAIC2` automatically to the
matching companion compiler. Override it only when running the
LSP outside the wrapper (CI, embedded scenarios).

## Troubleshooting

**The editor hangs on connect.** Ensure `kai lsp` is on your
`$PATH` and that `which kai` resolves to the same version as the
compiler that built your project. The LSP writes nothing to stdout
between requests; that is normal.

**Hover returns nothing.** The probe failed to type-check the
file. Check stderr for the LSP — it forwards `kaic2 --library-mode`
errors there. Common cause: an unsaved edit introduced a parse
error.

**The wrapper says `kai-lsp binary missing at .../libexec/kaikai/kai-lsp`.**
You are on an installed layout (brew, packaged release) that did
not ship the LSP. Rebuild from a dev checkout or install a newer
release.

## Roadmap

v1 (hover) → v2 (goto-def + publishDiagnostics + documentSymbol,
shipped 2026-05-09) → v3 (completion + signatureHelp + hole
warning diagnostics, shipped 2026-05-11 → 2026-05-20). Next
candidates for v4: cross-file resolution, locals/params in
completion scope, field completion after `.`, references,
rename, workspace symbols, inlay hints. Each step is a separate
PR-sized lane; tracked in #447.
