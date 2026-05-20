// Minimal VS Code extension wiring `kai lsp` as a language server.
// Activates on `.kai` files and forwards LSP traffic over stdio.
//
// Issue #447, v1: hover only. Future: goto-def + diagnostics push
// follow as the LSP server grows; no extension changes are required
// — capabilities are negotiated through `initialize`.

const { workspace } = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client;

function activate(_context) {
  const config = workspace.getConfiguration("kaikai");
  const command = config.get("lspPath") || "kai";

  const serverOptions = {
    run:   { command, args: ["lsp"], transport: TransportKind.stdio },
    debug: { command, args: ["lsp"], transport: TransportKind.stdio },
  };

  const clientOptions = {
    documentSelector: [{ scheme: "file", language: "kaikai" }],
  };

  client = new LanguageClient("kaikai", "kaikai", serverOptions, clientOptions);
  client.start();
}

function deactivate() {
  if (client) {
    return client.stop();
  }
}

module.exports = { activate, deactivate };
