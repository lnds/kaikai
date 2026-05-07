# Lane experience — issue-308-effect-shadowing

## Objective metrics

- Lane started: 2026-05-06T21:56-04:00
- Lane closed:  2026-05-06T22:27-04:00
- Wall: ~31 minutes (under the 1.5–2 h budget).
- Compiler diff: ~110 lines added in `stage2/compiler.kai` (resolver
  unchanged; the bug was a pure emit-time gating issue).
- New fixtures: 2 (`issue_308_user_log_effect.kai`,
  `issue_308_stdlib_log_unchanged.kai`), each with `.out.expected`.
- Makefile diff: ~22 lines (one new block in `test-effects`,
  exercising both fixtures on both backends).

## Diagnosis (bug location)

Issue #308 surfaces as invalid C from the user-facing program

```kai
effect Log { log(msg: String) : Unit }
fn main() { handle { ... } with Log { log(msg, resume) -> { ... } } }
```

The C compiler errors are

```
error: no member named 'debug' in 'struct EvLog'
error: no member named 'info' in 'struct EvLog'
...
```

Tracing the emit:

1. `inject_builtin_effects` already gates the builtin `Log` decl on
   "user has not declared an effect with this name" (via
   `inject_unconditional`'s `existing` check). So the user's
   `effect Log` cleanly *replaces* the builtin in the decl table —
   resolver-side shadowing already worked.

2. But `default_setups_for(main_row)` /
   `default_shims_for(main_row)` / `default_pops_for(main_row)`
   (C backend, `stage2/compiler.kai:15539+`) and the parallel
   `llvm_default_builtin_decls` / `llvm_emit_main_install_defaults`
   / `llvm_emit_main_teardown_defaults` (LLVM backend) gate purely
   on `list_has(main_row, "Log")` — not on whether the in-scope
   `Log` decl matches the runtime's expected EvLog struct shape.

3. When the user's `Log` is in scope, the user's effect ops
   (`log` only) drive `emit_effect_struct` (so `struct EvLog` has
   one field `log`), but the default-handler emit *still* writes
   `_kai_default_ev_log.debug = ...` etc. — referencing four
   nonexistent fields.

So this is **purely an emit-time bug**. The resolver was already
correct.

## Resolver change

None. `inject_unconditional`'s pre-existing gate
(`stage2/compiler.kai:44954+`) on user-declared effect names was
already sufficient.

## Default-handler emit change (the lazy gating)

Added a per-effect compatibility check used to filter `main_row`
right before the C and LLVM emit sites read it
(`stage2/compiler.kai:9750+`):

- `expected_default_op_names(name)` returns the canonical op-name
  list for each effect with a default handler. The list mirrors
  the `EvX` struct field order in `stage0/runtime.h` — Stdout
  → `[print]`, Log → `[debug, info, warn, error]`, etc.
- `lookup_effect_op_names(decls, name)` walks the post-injection
  `decls` and returns the op-name list of the in-scope decl.
- `effect_default_compatible(decls, name)` returns false iff a
  decl is in scope AND its op names diverge from the expected
  list. (Names without a default handler — Reader, Writer,
  State, Actor, Ffi — pass through.)
- `row_minus_user_effects(main_row, decls)` filters `main_row`
  through that predicate.

Two emit sites were retargeted at the filtered row:

- `stage2/compiler.kai:14796` (C backend `emit_main`)
- `stage2/compiler.kai:38958` (LLVM backend `emit_program_llvm`)

Both backends already had the per-effect `if list_has(main_row,
"X") { ... }` gating; filtering the row at the source flips them
all from "always emit if name is present" to "emit only if shape
matches the runtime contract". Symmetric C/LLVM treatment.

The `eff_op_names_eq` helper at `stage2/compiler.kai:26671`
already existed for the typer's row-comparison path and is
reused.

## Empirical verification (2 fixtures pass)

Both run on C and LLVM backends:

```
$ bin/kai run examples/effects/issue_308_user_log_effect.kai
[INFO] hello, kaikai
[INFO] hello, world

$ bin/kai run examples/effects/issue_308_stdlib_log_unchanged.kai
D:d-msg
I:i-msg
W:w-msg
E:e-msg
```

The first proves the shadowing fix; the second proves the stdlib
`import log` path still wires the default handler correctly. Both
fixtures are wired into `make test-effects` (C + LLVM gates) and
pick up Tier 1 automatically.

## Friction points

- **Initial heuristic was wrong** (line/col != 0,0). I picked it
  because builtin decls are constructed with line=col=0, so user
  decls would be the only ones with non-zero positions. But many
  stdlib effects (e.g. `Clock` in `stdlib/time.kai`) are *parsed
  from .kai source*, not built via `builtin_*_decl` — so their
  decls have non-zero positions, and the heuristic flagged them
  as user-shadowed and silently dropped the `Clock` default
  handler. Caught by `time_clock_default` segfaulting under
  `make tier1`. Pivoted to op-name shape matching, which is
  decoupled from the source-vs-injected origin.
- **Makefile fixture wiring** — initial fixtures used `println`
  (auto-loaded by `bin/kai`'s prelude wrapper but not by the bare
  `kaic2` invocation the Makefile uses). Fix was to switch to
  `Stdout.print` directly, matching `issue_141_log_default`.
- **Function-name collision** — first attempt named the helper
  `string_lists_eq`, which clashed with an existing typer helper
  having the same generated symbol name. Renamed to reuse the
  existing `eff_op_names_eq`.

## Subjective summary

The fix landed cleaner than expected because the resolver was
already correct (PR #284 / #251 era shadowing work paid off
here): only the emit gating needed updating. The compatibility
table is the conceptually correct check — "does the decl in
scope match what the runtime's default handler expects?" — and
it dovetails with the existing `EvX` struct layout invariant in
`stage0/runtime.h`. Adding a new defaulted effect now requires
adding one row to `expected_default_op_names`, which is parallel
to (and as cheap as) adding the `default_X_setup` helper itself.

## Limitations

- **No `import log as stdlib_log` aliasing.** A user who
  declares `effect Log { ... }` in their module currently has no
  way to also reach the stdlib `Log` effect by a qualified name.
  The brief explicitly deferred this; tracked separately if a
  user requests it.
- **No warning on shadow.** The compiler silently accepts
  shadowing. A diagnostic ("warning: `effect Log` shadows the
  stdlib effect; the stdlib default handler is disabled in this
  module") would help the LLM-authorship goal but belongs in a
  separate lane.
- **Compatibility is op-name-only.** Two decls with matching op
  names but differing param/return types still pass the gate.
  In practice this is fine — the typer rejects calls against the
  wrong signatures before emit — but a future hardening could
  also check arg/ret shape.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T22:16:04-04:00	tier0	OK	-
2026-05-06T22:23:29-04:00	tier1	OK	-
2026-05-06T22:25:18-04:00	tier1-asan	OK	-
2026-05-06T22:27:25-04:00	selfhost-llvm	OK	-
```
