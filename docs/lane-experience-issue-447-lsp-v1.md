# Lane experience — issue #447: `kai lsp` v1 (hover-only)

**Lane window:** 2026-05-19 night, single session.
**Branch:** main (direct commits — no integrator gate on this lane
because it lands a new `kai` subcommand and a tools/ binary, neither
in any tier1 critical path).
**Closing PR:** N/A (direct push; see commit log under
`feat(lsp): hover-only Language Server v1`).

## Scope as planned

Brief in `/tmp/kaikai-lsp-mission.md` enumerated Phases 0–9: env
prep, `tools/kai-lsp/` scaffold, JSON-RPC framing, lifecycle, hover,
`bin/kai lsp` dispatch, smoke fixture, docs + VSCode extension,
ship. Goal: shippable v1 hover; goto-def and diagnostics push left
for next lanes.

## Scope as shipped

Everything from Phases 0–9 landed. Phase 10+ (goto-def, diagnostics
push, diag-migration) deferred per brief.

New surface:

* `prelude write_stdout(s: String) : Unit / Stdout` — new builtin.
  Writes a String verbatim with no trailing newline, then flushes.
  Required by LSP JSON-RPC framing: `Content-Length: N\r\n\r\n<body>`
  cannot tolerate the `\n` that `print` appends.
* `bin/kai lsp` subcommand.
* `tools/kai-lsp/main.kai` — ~600-line single-file LSP server in
  kaikai.
* `examples/lsp/hover_basic.{kai,lsp.py}` — smoke harness.
* `docs/lsp.md` — user-facing reference.
* `extensions/vscode/` — minimal VS Code extension.

## Design decisions and alternatives considered

### Subprocess vs. in-process compiler

**Chose:** spawn `kaic2 --library-mode <tmp>` per hover.
**Considered:** linking stage 2 (~60 kLOC) into the LSP binary.
**Why subprocess:**
1. Decoupling. LSP can ride newer compilers without rebuild.
2. Binary size. LSP stays ~110 KB; stage 2 link would double it.
3. Crash isolation. A typer bug at the cursor does not kill the
   LSP session.

Cost: ~5–15 ms per hover on macOS. Acceptable for v1; revisit when
goto-def + diagnostics push raise the QPS budget.

### Raw stdout write: new builtin vs. workaround

**Chose:** new `write_stdout` builtin.
**Considered:** (a) emit `\n` after body and trust permissive
clients; (b) shell-out via `process.start("/bin/sh", ["-c", "printf ..."])`.

(a) breaks `tower-lsp` and other strict framing parsers — they
expect the next byte after the body to be either CR or the start
of the next `Content-Length` header. (b) is awkward and forks a
shell per response. The builtin is six lines of C plus the usual
six registration sites in `stage2/compiler.kai`. Right call.

### Subprocess output capture: shell redirection vs. new prelude

**Chose:** `sh -c "<kaic2> --library-mode <tmp> > <out>"` plus
`File.read_file(out)`.
**Considered:** adding a `process_capture(cmd, args)` prelude that
wraps `popen(3)`.

Shell redirection is ugly but ships. A proper `popen` wrapper is
its own design exercise (signal handling, decoder, EAGAIN policy)
and was not in scope. Deferred follow-up.

### Single file vs. multi-module

**Chose:** one `main.kai` with section dividers.
**Considered:** sibling modules per phase.

A multi-module layout was sketched in the brief but rolled back.
Reasons: (1) per-call name qualification adds noise to the hot loop;
(2) the file is short enough to navigate by section.

### Position encoding: bytes vs. UTF-16

**Chose:** bytes, with a documented caveat.
**Considered:** UTF-16 conversion.

LSP mandates UTF-16 code units. kaikai uses byte offsets and has
no UTF-16 indexing primitive. Caveat documented under "Known v1
limitations" in `docs/lsp.md`. ASCII-only fixtures pass; non-ASCII
content misaligns. Tracked in #447 step 4.

### Tempfile lifetime

**Chose:** `/tmp/kai-lsp-src-L_C.kai` per hover, overwritten in
place each request.
**Considered:** in-memory pipe; `mkstemp(3)`.

Pipes require `popen` (see above). `mkstemp(3)` is correct but
asks for a new prelude. The L_C naming gives uniqueness within a
single hover and naturally collides across hovers — irrelevant
because the file is rewritten each time. Tradeoff: an attacker
with `/tmp` write access could substitute the source between
write and exec. Same trust model as every other kaikai tool that
writes `/tmp`.

## Structural surprises

* **`cons` is a builtin C symbol.** Naming a top-level helper `cons`
  produced `redefinition of 'kai_cons'` at the C-codegen stage.
  Renamed to `lcons`. Standing trap; worth a stdlib symbol check.
* **`extract_string_at` originally treated `\` + next byte as
  literal pass-through.** That worked for `\\` and `\"` but mangled
  `\n` (turned into the literal letter `n` in source). Found via
  inspecting the tempfile; symptom downstream was a parse error
  on the trailing letter the LSP did not expect. Fixed by adding
  `json_decode_escape` for the standard JSON simple-escape set.
* **`read_bytes` is ambiguous.** Bare `read_bytes(n)` resolves
  to `file_read_bytes(path)` when `import fs.file` is in scope
  (matches the prelude name despite the type mismatch). Must use
  `Stdin.read_bytes(n)` qualified to disambiguate. Worth a typer
  hint.
* **No `Ref[T]` prelude.** The brief assumed `Mutable + ref_*`
  was available bare; not exposed. Rewrote the tempfile counter
  as a stateless `tag-from-line-col` scheme.
* **`process.wait` does not capture stdout.** Confirmed at line 4
  of `stdlib/os/process.kai` — wait returns `Result[String, Exit]`
  where the String is `strerror` for EAGAIN, not the child's
  output. Drove the shell-redirection workaround above.

## Fixtures added

* `examples/lsp/hover_basic.kai` — minimal source with type-able
  positions.
* `examples/lsp/hover_basic.lsp.py` — JSON-RPC driver. Asserts
  hover at position 0:31 (variable `x`) returns `Int` and at 3:10
  (call to `add`) returns the function type. Not wired into tier1
  (Python dep + LSP runtime test, both outside the Tier 1
  contract).

## Coverage gaps

* `didChange` (Full sync) is implemented but not exercised by the
  smoke harness — only `didOpen` followed by hover. A regression
  there would not be caught. Adding a `hover_didChange.lsp.py`
  fixture is the obvious next step.
* `shutdown` followed by stray traffic is untested.
* Non-ASCII inputs are not tested.

## Real cost vs. estimate

Brief estimated ~6 hours across phases. Actual: ~2 hours including
the runtime change. Faster than expected because:

1. The compiler already had `--library-mode` and `read_bytes` — no
   compiler work beyond the `write_stdout` builtin.
2. The `kai-pkg` pattern in `bin/kai` was a complete template; the
   LSP wiring is a clone.
3. The single-file approach skipped the module-import friction.

The brief's slowest predicted phase (Phase 5, hover) actually took
the longest debug session because of the JSON-escape bug.

## Follow-ups for next lanes

In rough priority order:

1. **Goto-definition** (`textDocument/definition`). Probe shape is
   already wired in `--library-mode` (`# @probe def L:C`). Handler
   in `tools/kai-lsp/main.kai` follows the same pattern as
   `handle_hover`. Estimated: 2 hours.
2. **Diagnostics push** (`textDocument/publishDiagnostics`). Wire
   `--diags-json` (issue #487) on `didOpen` + `didSave`. Blocked
   until at least 50% of the 228 legacy stderr diag sites migrate
   to `mk_diag`; #487 is the umbrella.
3. **`didChange` smoke coverage** in `examples/lsp/`.
4. **UTF-16 position conversion.** Needs a byte↔UTF-16 helper. Lift
   from a small `count_utf16_units(s, byte_off)` prelude.
5. **In-process daemon mode.** Long-term; replaces subprocess hot
   path with a long-running typer process. Will need a richer IPC
   than tempfiles.
6. **`process_capture(cmd, args)`** prelude. Cleans up the
   shell-redirect hack.
7. **Symbol-collision typer hint.** When a user defines a top-level
   fn with the same name as a builtin C symbol (`cons`, `print`,
   `map`, …), error early rather than at codegen.
8. **`read_bytes` overload disambiguation hint.** When `import
   fs.file` shadows the Stdin op, suggest the qualified form.

## What I would do differently next time

* Inspect tempfiles earlier when the symptom is "compiler returns
  empty/error." The JSON-escape bug would have shown up in 30
  seconds via `cat /tmp/kai-lsp-src-*`. Spent ~10 minutes hunting
  through the JSON-RPC framing layer first.
* Check stdlib for ref/Mutable surface before designing around it.
  The brief assumed availability that does not exist; the rewrite
  was cheap but predictable.

## Conclusion

Hover-only LSP is shipped, smoke-tested, documented, and wired
into the editor surface that matters most (VS Code). The
subprocess architecture turned out to be the right call for v1
and is not the bottleneck. Goto-def is the obvious next step;
diagnostics push is the bigger one and is gated on the legacy
diagnostic migration.
