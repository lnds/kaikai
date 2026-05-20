# LSP smoke fixtures

These fixtures drive `kai lsp` (issue #447) through scripted JSON-RPC
sessions. Each `.kai` file is a tiny program the LSP would see in
`textDocument/didOpen`; the matching `.lsp.py` driver script issues
the request sequence and asserts on the responses.

Run with:

    python3 examples/lsp/hover_basic.lsp.py

Exit code 0 = pass; non-zero = failure with a diff against the
expected response. These tests are **not** wired into tier1 — they
require `python3` plus a running build of `kai-lsp`. Run them
locally when touching `tools/kai-lsp/` or the `--library-mode`
surface in `stage2/compiler.kai`.
