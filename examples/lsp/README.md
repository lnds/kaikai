# LSP smoke fixtures

These fixtures drive `kai lsp` through scripted JSON-RPC sessions. Each
`.lsp.py` driver opens a tiny program in `textDocument/didOpen` (inline
in the script, or `hover_basic.kai` for the hover driver), issues the
request sequence and asserts on the responses.

Run with:

    python3 examples/lsp/hover_basic.lsp.py

Exit code 0 = pass; exit 2 = `kai-lsp` binary missing (build it
first); any other non-zero = failure with a diff against the
expected response. The drivers need `python3` (stdlib only) plus a
built `tools/kai-lsp/kai-lsp`. `make test-lsp` builds the binary and
runs all drivers; it is part of tier1.
