# Lane experience — LLVM multi-`var` State evidence resolution

Issue: refs #622 (cluster umbrella). Cluster F (`demos/spiral`).
Branch: `fix-multi-var-state` (forked from main through PR #713).
Plan: `docs/llvm-parity-plan-2026-05-26.md` Cluster F.

## Scope as planned vs as shipped

**Planned.** Fix the LLVM backend's wrong `@var` read when several `var`
cells (nested `State[T]` handlers) are live at once: a read resolved to the
wrong cell's value, corrupting an array index (`array_set: index 25 out of
range (len=16)` in `demos/spiral`). Make spiral byte-identical on both
backends, remove its parity skip, add a regression fixture, write a retro.
The brief flagged the bug as **possibly structural** in effect-evidence
resolution and asked to consult asu/linus (or leave the PR un-merged) if the
fix touched how evidence frames are pushed/resolved.

**Shipped.** Exactly the planned scope. The fix was **not** structural: the
per-instance evidence model (`handler_id` per handler, resolve aliased ops by
id) already exists in `runtime.h` and the **C backend already uses it**. The
LLVM backend had simply never implemented the mirror — it was explicitly
deferred ("m7c-c will use this for per-instance dispatch", a dead `let _ =
alias_opt` in `llvm_emit_handle`, and a comment in `llvm_emit_call` saying
"Per-instance dispatch on the LLVM side stays a follow-up"). I brought the
LLVM emitter to parity with the existing C model. No change to how evidence
frames are pushed or how the runtime walks them. **Did not consult asu/linus**
— the diagnosis showed it was completing a deferred mirror, not redesigning
the evidence stack.

## Root cause — which `@var` read went wrong, against which frame

A `var name = init` lowers to `with State[T](init) as name`; `@name` lowers
to an aliased op call `State.get@name` (the alias-rewrite pass tags the
callee with `@<alias>`), and `name := e` to `State.set@name`.

The C backend resolves an aliased op to a **specific handler instance**: the
handle install emits a local `KaiHandlerId kai_alias_<a>_id = _ev.handler_id;`
and the op-call site emits `kai_evidence_lookup_node_by_id(kai_alias_<a>_id)`.
Each `var` cell thus reads/writes *its own* node.

The LLVM backend ignored the alias tag entirely. `llvm_emit_op_dispatch`
always emitted `kaix_evidence_lookup_node(eff_name)` — **lookup by effect
name** — which returns the innermost (top-of-stack) `State` node. With one
cell this is correct; with N nested cells, **every** `@var` read resolved to
the most-recently-pushed cell.

Minimal repro that pins it (smaller than the brief's 6-cell case): two cells,
condition reads a literal, body reads both cells:

```kai
import loop
fn fill() : Unit / Console = {
  var top = 0
  var bottom = 3
  while { @top <= 3 } { print(int_to_string(@top)); print(int_to_string(@bottom)); top := @top + 1 }
}
```

C prints `0 3 1 3 2 3 3 3`; LLVM printed `3 3` — the very first `@top` read
returned **3**, i.e. the value of `bottom` (the last-pushed State cell), and
the loop ran once because `@top <= 3` saw 3 immediately on the next turn. The
read resolved to `bottom`'s frame, not `top`'s. So: **`get` (and `set`)
resolved to the wrong frame** — always the innermost State node by name,
regardless of which alias the surface `@var` named.

## The fix (3 edits, parity with the C model)

1. **`stage0/runtime_llvm.c`** — add `kaix_evidence_lookup_node_by_id(i64)`,
   a thin wrapper over the existing `kai_evidence_lookup_node_by_id` in
   `runtime.h` (the C backend already calls the latter inline). The runtime
   side already had everything; only the LLVM-callable symbol was missing.

2. **`emit_llvm.kai` — install (`llvm_emit_handle`).** When the handle has an
   `as`-bound alias, stash the freshly-minted `handler_id` under the sentinel
   local `__alias_id__<a>`. Innermost-alias-wins falls out of the locals
   stack being newest-first.

3. **`emit_llvm.kai` — dispatch (`llvm_emit_op_dispatch`).** When the op was
   tagged `@<alias>` and the sentinel is in scope, resolve via
   `kaix_evidence_lookup_node_by_id` instead of `lookup_node(name)`.

## Structural surprise the brief did not anticipate — the boxing detour

First attempt stored the `handler_id` register (an `i64`) directly into the
sentinel local. It worked for a flat handle body but the IR **failed to
verify** when the body was a `while` loop:

```
'%t6776' defined with type 'i64' but expected 'ptr'
  store %KaiValue* %t6776, %KaiValue** %t6800
```

A `while` body lowers to a closure. The closure's capture array is uniformly
`%KaiValue*`, and the free-variable analysis *does* pull the sentinel in
(the `@var` reads inside the loop reference it). So the captured sentinel
arrives as a boxed `%KaiValue*`, not an `i64` — exactly what the pre-existing
comment at `emit_llvm.kai:186` anticipated ("packs it as a `kai_int` so the
closure capture array stays uniformly `KaiValue *`; the lambda body unpacks
it on the way in"). That comment had been dead for as long as the dispatch
was deferred.

Resolution: store the sentinel as a **boxed** `%KaiValue*` (`@kaix_int(i64)`
at install) and **unbox** it (`@kaix_to_int(%KaiValue*)`) at the dispatch
before passing to `lookup_node_by_id`. Both helpers already existed and were
already declared in the LLVM preamble. This keeps the sentinel uniform with
every other captured local, so it threads through closures with no special
case. This was the only non-mechanical part of the lane.

## Fixtures

- `examples/effects/multi_var_state_index.kai` (+ `.out.expected` = `1`) —
  the brief's 6-cell repro (nested `while`, array index from `@top * dim +
  @c`). Auto-discovered by `tools/test-backend-parity.sh` (it walks
  `examples/effects`), so it now gates C↔LLVM parity. Verified to FAIL on a
  clean main tree (`exit code mismatch C=0 LLVM=1`) and PASS with the fix.
- Removed `demos/spiral/main.kai:622:...` from
  `tools/backend-parity-skips.txt`.

## selfhost / ASAN

- **selfhost byte-identical**: `kaic2b.c == kaic2c.c`. Critical — the
  compiler uses `var`/State heavily; a wrong evidence fix would corrupt it.
  The compiler self-compiles through the C backend, so byte-identity also
  confirms the C path was untouched (it was not).
- **tier1-asan OK** (standard suite) plus a manual **LLVM+ASAN** run of the
  new fixture and `demos/spiral`: clang `-fsanitize=address,undefined`
  against `runtime_llvm.c`, both exit 0 with correct output and **no
  sanitizer diagnostics**. The original bug was an OOB array read, exactly
  ASAN's wheelhouse — clean confirms the read now lands in-bounds.
- **backend-parity**: diff vs a clean-main baseline shows exactly one line
  removed (the new fixture flips FAIL→PASS) and **zero new divergences**. The
  6 remaining FAILs (`blackjack`, `poker_dealer`, `issue_141_log_default`,
  `issue_682_cancel_sibling_handler`, `auto_install`, `cross_package_effects`)
  are byte-identical to the baseline — nondet RNG/clock/scheduler and harness
  package issues, none touched by this lane.

## Cost vs estimate

The mechanical edits were ~30 min. The cost was in (a) narrowing the repro
from 6 cells to 2 to localise the wrong frame, and (b) the IR-verify failure
on the i64-vs-`%KaiValue*` capture, which sent me to read how closures
capture locals before realising the dead `:186` comment had already called
the shot. Both are the kind of thing the C-backend oracle resolves fast:
diffing how `emit_c.kai` resolves an aliased op (`lookup_node_by_id`) vs
`emit_llvm.kai` (`lookup_node` by name) pointed straight at the deferred
mirror.

## Follow-ups left for next lanes

- None for this cluster. The other Cluster-F entries
  (`process_basic` record/variant op-return marshalling) and the
  `m8_4_cancel`, `r9_clause_capture` continuation bugs are separate root
  causes, explicitly out of this lane's scope.
- The eric structural follow-up still stands: a tier0 symbol-coverage script
  that fails when a `kai_*` runtime helper lacks its `kaix_*` mirror. This
  lane is a textbook instance — `kai_evidence_lookup_node_by_id` shipped in
  `runtime.h` long ago, but its LLVM mirror was missing until now, and only a
  hand-written repro surfaced the gap.
