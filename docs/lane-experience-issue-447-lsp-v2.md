# Lane experience — issue #447: `kai lsp` v2 (goto-def + diagnostics + symbols)

**Lane window:** 2026-05-20, single session.
**Branch:** main (direct commits; same standing exception as v1).
**Builds on:** `lane-experience-issue-447-lsp-v1.md`.

## Scope as planned

Continue the LSP roadmap from #447 step 1 (hover-only, shipped in
v0.75.1). Four lanes proposed in the v1 retro: goto-def,
diagnostics push, document symbols, smoke + ship.

## Scope as shipped

All three feature lanes plus a compiler fix that surfaced while
building goto-def. New LSP capabilities:

* `textDocument/definition` — points at the source decl of the
  identifier under the cursor.
* `textDocument/publishDiagnostics` — fires on `didOpen` and
  `didChange`; clears on a clean buffer.
* `textDocument/documentSymbol` — outline panel content
  (Function, Method, Interface, Constant, Variable kinds).

New compiler surface:

* `# @probe symbols` — new library-mode probe. Whole-file query
  that dumps every top-level decl authored in the root file.
  `lib_run_probe` now also receives `raw_decls` so symbols walks
  the pre-elaboration AST (DType + DConst keep their parser
  identity).
* `find_decl_def` two-pass scan — prefers user-authored decls
  over preloaded prelude / stdlib decls. Fixes a long-standing
  goto-def bug where any name that collided with a stdlib `impl`
  (e.g. `add` from `Add for Int`) resolved to the wrong file.
* `is_root_origin(o)` and `is_user_pos(line, col)` helpers exposed
  alongside.

## Design decisions and alternatives considered

### Definition resolution precedence

**Chose:** prefer `module_origin = None` matches over module-owned
decls in `find_decl_def`.
**Considered:** thread the user's file path through the call and
filter by it.

The file-path approach would handle multi-module projects more
honestly, but the module-origin flag is already present on every
DFn / DType and is exactly the bit the codegen uses to namespace
symbol names. The two-pass scan is six lines per pass; the file-
path approach would require an extra parameter through several
recursive helpers.

The price: a user's `fn print` overrides the prelude `print` for
goto-def, but stdlib internal references to `print` still resolve
to the prelude. That matches user expectation in every case I
walked through.

### Returning publishDiagnostics from notification handlers

**Chose:** notifications return `(state, Option[String])` like
request handlers. The dispatch loop writes the body just as it does
for responses.
**Considered:** opening a side-channel for proactive emissions.

A side channel would let the server emit multiple messages per
incoming notification (e.g. didOpen → publish + workDoneProgress
+ ...). v2 needs only one outgoing message per incoming, so the
simpler return-tuple shape is enough. When v3 adds progress
indicators a second outlet will land.

### Symbols probe at typed-module vs. parser-level

**Chose:** parser-level decls (`raw_decls` threaded through
`lib_run_probe`).
**Considered:** walking the typed-module decls.

The typer drops `DType` entirely and reshapes `DConst` into
`DAttribPure(DFn arity-0)`. Walking the typed shape loses the
declarations the editor's outline panel cares about. The cost: an
extra parameter through `lib_run_probes`. Not load-bearing for any
other probe — the existing `type` / `def` / `enc` probes ignore it.

DType still does not appear in v2 — the parser produces it, but
the type elaborator drops it before any later pass sees it.
Probably needs a `lib_collect_symbols` arm that reads from a
separate `[DTypeDecl]` channel the elaborator preserves. Out of
scope for this lane.

### Symbol kind mapping

**Chose:** kaic2 surface emits LSP SymbolKind names (`Function`,
`Class`, `Interface`, …); the LSP shim decodes them to wire
integers.
**Considered:** emit integers directly from the compiler.

Names are debuggable. The wire encoding is an LSP detail that
doesn't belong in the compiler's library-mode JSON — kaic2 stays
ignorant of LSP, the shim handles the protocol.

## Structural surprises

* **`string_slice(s, from, len)` not `(s, from, end)`.** Six
  places in the LSP file had `string_slice(blob, i, close)` when
  they should have been `string_slice(blob, i, close - i)`. The
  symptom was an empty / truncated entry that downstream parsers
  silently dropped. Standing trap for anyone who reflexes from
  Python's `s[i:j]` semantics.
* **Nested `kind` fields confuse the regex-style decoder.** The
  outer `{"kind": "symbols", …}` probe envelope matched my
  per-symbol decoder regex first, swallowing all subsequent
  symbols inside its braces. Had to anchor the decoder to
  `"symbols": [` and only scan after it.
* **Compact vs. pretty JSON.** kaic2 emits `"kind": "Function"`
  with a space; my first cut searched for `"kind":"Function"`
  without one. Added a tiny `find_*_entry` that tries both
  spacings. Honest follow-up: a real JSON parser in `tools/`
  shared across all LSP shims.
* **publishDiagnostics breaks the existing hover smoke.** The v1
  smoke read one response per request; v2 didOpen now fires two
  outbound messages (publishDiagnostics + the eventual hover
  response). Old fixture had to consume the extra frame.
* **goto-def for stdlib names was completely broken before this
  lane.** Every user-authored fn named `add`, `print`, `map`,
  `filter`, … was sending the cursor into `stdlib/protocols.kai`.
  Caught by the goto-def smoke; the fix was small but the bug had
  been latent since `def_at` shipped.

## Fixtures added

* `examples/lsp/goto_def.lsp.py` — drives the `definition`
  request and asserts the location matches the source decl.
* `examples/lsp/diagnostics_push.lsp.py` — sends a bad buffer,
  asserts severity-1 diagnostic; sends a clean buffer, asserts
  empty diagnostics.
* `examples/lsp/document_symbols.lsp.py` — opens a 3-function
  source, asserts `add`, `double`, `main` all return as
  Function-kind (12).
* `examples/lsp/hover_basic.lsp.py` — updated to consume the new
  publishDiagnostics notification.

## Coverage gaps

* `DType` does not appear in documentSymbol. Variants surface as
  Functions (their constructors lower that way), but the type head
  itself is invisible to outline panels.
* Cross-file goto-def still untested. Imports parse, but
  `find_decl_def` walks the current file's decls — a goto-def on
  an imported symbol may or may not resolve depending on whether
  the typer pulled the foreign decl into the typed module.
* The 228 unmigrated diag sites still print to stderr — they will
  not surface as LSP diagnostics until the migration completes.
  Tracked in #487.
* No `selectionRange` precision: symbols emit identical `range`
  and `selectionRange` (both span just the name). LSP spec asks
  for `range` to span the whole declaration body; we'd need a new
  `lib_decl_end_pos` helper.

## Real cost vs. estimate

Total ~3 hours including a compiler rebuild for the def_at fix
and the symbols probe extension. v1 retro estimated ~6 hours for
this batch; faster because:

1. The `probe_run` refactor in v1 paid off — goto-def reused the
   subprocess plumbing zero-cost.
2. publishDiagnostics is a notification, no id round-trip
   accounting.
3. The compiler had `def_at` and `--diags-json` already wired —
   only `lib_collect_symbols` was new.

The two slow spots: hunting the `string_slice` off-by-one (15
min) and the nested-kind decoder bug (10 min).

## Follow-ups for next lanes

In rough priority order:

1. **DType in documentSymbol.** Preserve DType in a sidecar list
   through the elaborator. Estimated: 2 hours.
2. **Cross-file goto-def.** Walk imported module decls when the
   current-file scan misses. Needs module-resolution context the
   library-mode entry point doesn't yet carry.
3. **completion** (`textDocument/completion`). Mash up symbols
   query + scope walk + keyword list. Estimated: 4 hours.
4. **signatureHelp** (`textDocument/signatureHelp`). Needs a new
   probe for the enclosing call's param types. ~3 hours.
5. **Real JSON parser** in `tools/json/` shared across `kai-pkg`
   and `kai-lsp`. Today both projects hand-roll regex-style
   decoders. Standing tech debt.
6. **`process_capture`** prelude — replace the shell-redirect
   `sh -c "... > tmp"` workaround. Still tracked from v1 retro.
7. **Diagnostic migration**. 228 stderr sites. Mechanical but
   tedious; one lane per ~50 sites is the right granularity.

## Conclusion

LSP v2 turns kai-lsp from a curiosity into a genuinely useful
language-aware editor backend. Hover + goto-def + diagnostics +
outline is the table-stakes set for any modern LSP — they cover
the 80% of editor interactions a user has with their code in any
session. Completion + signature help bring the remaining 20% and
will land as separate lanes.
