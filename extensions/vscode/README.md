# kaikai VS Code extension

Minimal extension that wires the `kai lsp` language server (issue
#447) into VS Code. v1 surfaces hover; goto-def, diagnostics, and
completion ride newer LSP releases without extension changes.

## Install (dev)

```bash
cd extensions/vscode
npm install
npx vsce package                     # produces kaikai-lsp-0.1.0.vsix
code --install-extension kaikai-lsp-0.1.0.vsix
```

The extension requires `kai` on your `PATH`. Override the binary
path through the `kaikai.lspPath` setting:

```json
"kaikai.lspPath": "/usr/local/bin/kai"
```

See `docs/lsp.md` in the kaikai repository for the LSP server
documentation, supported methods, and known limitations.
