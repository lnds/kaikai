# LSP smoke fixtures

These fixtures drive `kai lsp` (issue #447) through scripted JSON-RPC
sessions. Each `.kai` file is a tiny program the LSP would see in
`textDocument/didOpen`; the matching `.lsp.py` driver script issues
the request sequence and asserts on the responses.

Run with:

    python3 examples/lsp/hover_basic.lsp.py

Exit code 0 = pass; exit 2 = `kai-lsp` binary missing (build it
first); any other non-zero = failure with a diff against the
expected response. The drivers need `python3` (stdlib only) plus a
built `tools/kai-lsp/kai-lsp`. `make test-lsp` builds the binary and
runs all drivers; it is part of tier1.
