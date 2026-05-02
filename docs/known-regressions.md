# Known regressions

Active bugs in `main` that are not blocking selfhost / `make test` but
should be fixed when the appropriate lane visits the relevant code.

Each entry records the symptom, a minimal repro, the hypothesis, and
the fix-verification path so the next agent does not have to
re-discover anything.

---

## R1 — `list_contains` / `list_sort_by` self-recursion leaks a row variable when called from an effectful context

**Status**: **FIXED** 2026-04-26. The hypothesis ("row-unify path
problem") was wrong. The actual bug: the stdlib core file (then
`stdlib/core.kai`, now `stdlib/core/list.kai` after the 2026-04-27
split) declared its polymorphic functions as
`pub fn list_contains(xs: [a], x: a) : Bool`
*without* the `[a]` tparam brackets. Post-m7b #13, lowercase
identifiers in type position are nominal `TyCon`s unless declared
in `[…]`, so `a` was a concrete type and the call site failed to
unify with `[Int], Int`. The reason `make test` looked green is
that the recursive self-call inside the stdlib unifies `a` with
itself; only an external caller with concrete element types
exposes the mismatch.

**Fix**: `fn_scheme_of_decl` and `infer_decl` now auto-collect
lowercase identifiers in a decl's signature that were not
declared in `[…]` and treat them as *implicit* tparams. The
explicit-bracket form keeps working; users who wrote it pre-#13
do not need to rewrite.

**Side effect**: the m7b #13 negative fixture
`examples/sugars/m7b_13_lowercase_undeclared.{kai,err.expected}`
is removed because `pub fn id(x: a) : a = x` is now valid.

Original report below for posterity.

---

**Severity**: medium. `make test` is **green** — every official
fixture under `examples/` compiles. The regression manifests only
when a program *imports the new stdlib list ops* and *its `main` (or
caller) carries an effect row*.

### Symptom

```
$ make -C demos verify
...
state_explicit       FAIL kaikai: error: type mismatch in function call
state_var            FAIL kaikai: error: type mismatch in function call
```

Per-error detail (from `demos/build/state_explicit.err`):

```
error: type mismatch in function call
  --> state_explicit/main.kai:201:34
  = note: expected: ([a], a) -> Bool
  = note: found:    ([Int], Int) -> ?t5 / ?e1
error: type mismatch in function call
  --> state_explicit/main.kai:389:51
  = note: expected: ([a], (a, a) -> Int) -> [a]
  = note: found:    ([Int], (Int, Int) -> Int) -> ?t0 / ?e0
```

The reported file/line points into the demo source but the column
reflects the **prelude** prepended by `kaic2 --prelude
stdlib/core.kai` (the pre-split monolith; today the equivalent is
the chain of `--prelude stdlib/core/*.kai`). Lines 201 and 389 of
the concatenated file fell on the recursive call sites inside
`list_contains` and `list_sort_by` respectively (declarations
formerly at `stdlib/core.kai:163` and `:375`, now in
`stdlib/core/list.kai`).

### What is in the demos

`demos/state_explicit/main.kai` uses **only** `State[Int]` plus
`Console.print`; it does not call `list_contains` or `list_sort_by`
itself. The error comes from elaborating the prelude in this caller's
typing context.

`demos/state_var/main.kai` is similar: uses `var n = 0` (m7b #5b
sugar that desugars to `with State[Int](init) as n`) plus
`Console.print`.

Neither demo touches the new list ops directly. The bug is in how
the typer elaborates the prelude when **the surrounding context
carries an effect row**.

### Hypothesis

Probable interaction between m7b parametric effects (m7b #11 / #12)
and self-recursive prelude functions:

1. `list_contains(xs: [a], x: a) : Bool` (and `list_sort_by`)
   has a recursive call inside its body.
2. m7b #11/#12 introduced row inference for callees: at each call
   site of a polymorphic function, a fresh row variable is
   instantiated.
3. The recursive self-call inside `list_contains` instantiates its
   own row var `?e1`. In a pure context (`make test` fixtures), `?e1`
   unifies to the empty row by default.
4. In a caller whose `main` has a non-empty row (`/ Console`,
   `/ State[Int]`, etc.), the prelude is elaborated **after** the
   caller's row variable is bound. Some unification step fails to
   propagate the caller's row into `?e1`, leaving it free. The
   resulting type does not match the canonical signature
   (`([a], a) -> Bool` with no row) — type mismatch.

This is consistent with: `make test` fixtures use pure `main` (no
row), so `?e1` collapses; demos use effectful `main`, so `?e1` is
exposed.

### Repro outside `demos/`

The minimal repro (do **not** check this in — only for diagnosis):

```kai
# regress.kai
fn main() : Unit / Console = {
  let _ = list_contains([1, 2, 3], 2)
  Console.print("ok")
}
```

```sh
# Pre-split form (kept for the original repro shape):
./stage2/kaic2 --prelude stdlib/core.kai regress.kai > /dev/null
# Post-split equivalent:
./stage2/kaic2 $(for f in stdlib/core/*.kai; do echo --prelude $f; done) regress.kai > /dev/null
# both fail with the same `?e1` row-var leak (pre-fix)
```

### What `make test` does that masks the bug

Existing `examples/stdlib/list_basic.kai` (and siblings) declare
`fn main() { ... }` with no row annotation. The fix path for
`make test` should add at least one fixture with effectful main
that uses a stdlib list op — e.g.:

```kai
# examples/stdlib/list_in_effect_context.kai
fn use_in_effect() : Unit / Console = {
  if list_contains([1, 2, 3], 2) {
    Console.print("yes")
  } else {
    Console.print("no")
  }
}

fn main() : Int / Console = {
  use_in_effect()
  0
}
```

If this fixture passes, the regression is fixed.

### Where to look in the typer

`stage2/compiler.kai` — most likely in the unification path for
parametric row labels added by m7b #11 (`row.ty_args` propagation) or
m7b #12 (open row variables). Specifically, the moment a generic
function is instantiated at its call site **inside the elaboration
of a polymorphic function whose own row has not yet collapsed**.

The error shape after the m7b sweep:

```
expected: ([a], a) -> Bool
found:    ([Int], Int) -> ?t5 / ?e1
```

`expected` is the registered scheme (no row — `list_contains`'s
declared signature is `: Bool` with no `/ row`). `found` is the
self-call's instantiation: tyvars are bound (`a := Int`) but a
fresh row var `?e1` survives. The mismatch is row-shape: empty
row vs row-with-free-var. In a pure caller, `?e1` ends up unified
to `{}` by some side-channel (TBD); in an effectful caller, it
stays free.

The owner is the next agent who **specifically audits the row
side of `unify_*` and `synth_app`** in stage2/compiler.kai. The
m7b sweep (#14, #15, #16, #17, #2a-c, #4, #7) closed in 2026-04-26
without touching this path — none of those tasks needed it.

### Verification path

After a candidate fix:

```sh
make                                    # selfhost still green
make test                               # all suite passes
make -C demos verify                    # state_explicit, state_var → OK
make -C demos/9d9l verify               # 4d9l unchanged (still gated on use Effect / etc.)
make -C demos/vs verify                 # vs/ unchanged
```

Specifically `state_explicit` and `state_var` must flip to OK; that
is the smallest visible signal that the row-leak is gone.

### Workaround for callers (temporary, do not commit)

Casting the call site through a wrapper with explicit row breaks the
leak in some experiments — but the fix is in the typer, not the
caller side. No source-level workaround is recommended; let the
typer be fixed.

---

## R2 — `m8x_2_yield_interleave` SIGSEGVs at runtime

**Status**: **FIXED** 2026-04-29 evening. Bisect localised the
regression to commit `4a77d49` (m8 bug #12 in_dispatch fix). Root
cause: the `in_dispatch` flag lived on the evidence node, but
spawned fibers inherit the parent's `evidence_top` and therefore
share the same nodes — fiber A's "I'm dispatching X" flag skipped
X for *every* fiber, so when fiber B (running after a swapcontext)
looked up the same effect, the lookup walked past X and returned
NULL. The op-call site then dereferenced NULL.

**Fix**: moved `in_dispatch` from `KaiEvidence` to `KaiFiber`
(`in_dispatch_node`, a pointer to the node currently being
dispatched on this fiber). The lookups compare
`node == kai_current_fiber()->in_dispatch_node` instead of reading
a flag on the node. The op-call emitter saves the previous value,
sets `in_dispatch_node` to its own node, runs the clause, then
restores — preserves nesting via per-call-site C locals
(`_saved_disp`). Fiber B sees its own `in_dispatch_node` (NULL on
spawn from `calloc`), so X is visible to it even while A is
dispatching X.

`m8_12_self_delegating_handler` still passes (per-fiber state
covers the same self-recursion case as the global flag); m8x_2
now interleaves correctly (`A0 B0 A1 B1 A2 B2`); selfhost
byte-identical, `make demos-no-regression` baseline 23.

Original report below.

---

**Original status**: OPEN as of 2026-04-29 evening. Predates today's commits;
reproducible at v0.4.0 (commit `af384cb`, R2 m8.x scheduler land).

**Severity**: medium. `make test-effects` fails on this fixture and
aborts before reaching subsequent tests in the target. `make
selfhost` and `make demos-no-regression` are unaffected.

### Symptom

```
$ cd stage2 && rm -f kaic2 build/stage2.c && make kaic2 >/dev/null
$ ./kaic2 --path ../stdlib ../examples/effects/m8x_2_yield_interleave.kai \
    > /tmp/x.c
$ cc -std=c99 -I ../stage0 /tmp/x.c -o /tmp/x
$ /tmp/x ; echo "exit=$?"
exit=139
```

`/tmp/x` runs no output and exits with SIGSEGV (139 = 128 + 11).

### What the fixture does

Two spawned fibers each print three marks (`A0`/`A1`/`A2` and
`B0`/`B1`/`B2`) with `fiber_yield()` between prints. Under the
cooperative scheduler the expected interleave is `A0 B0 A1 B1 A2 B2`.
The crash happens before any output reaches the buffer — likely on
the first `fiber_yield()` or the trampoline entry of the second
fiber.

### Why this matters

The R2 scheduler land (v0.4.0, 2026-04-28) declared `make test
clean` and the m8.x-followup pinned `m8x_2_yield_interleave` as
the demo gate for Phase 2. The fixture was either never run as
part of the gate or broke after the gate was claimed.

The cumulative runtime work since v0.4.0 (in_dispatch flag in
`4a77d49`, match-scrutinee/short-circuit/prelude leak plugs in
`9fe6f6d`, small-int + char cache in `69c6166`) all touch
`stage0/runtime.h` near the scheduler. Bisect across the day's
commits would localise — but the bug also reproduces at v0.4.0
itself, so the regression is older.

### Hypothesis

Three candidates, in order of likelihood:

1. **Stack growth on swapcontext entry**. The fiber stack model
   pinned in `docs/fibers-impl.md` ships without a guard page; if
   `worker_a` / `worker_b` overflow the allocated stack on the
   first call frame (`Stdout.print` lowers to a few helper calls),
   the trampoline lands in unmapped memory.
2. **Refcount underflow on the closure passed to `fiber_spawn`**.
   The Phase 1 unboxing land (`69c6166`) changed how small-int
   primitives interact with the singleton cache; if the closure's
   captured row decref'd the wrong slot, the trampoline reads a
   freed value when it tries to call the body.
3. **`in_dispatch` flag leak**. The flag is initialized to 0 in
   the push helpers, but if `Spawn.spawn`'s clause body sets the
   flag without clearing it on a code path the emitter forgot,
   subsequent ops loop or read garbage.

### Verification path

Bisect with: `git bisect start HEAD v0.4.0 -- stage0/runtime.h
stage2/compiler.kai`, build kaic2 fresh at each step, run the
fixture binary, mark good/bad. Once localised, the offending commit
identifies which area (scheduler, refcount, dispatch).

The new fixture `examples/effects/m8_12_self_delegating_handler.kai`
exercises `in_dispatch` set/clear without going through the
scheduler — it passes cleanly under HEAD, so the `in_dispatch` flag
itself is sound. That narrows the suspect to scheduler interaction
or refcount, not the flag.

### Workaround

None for users — the fixture is internal. Until the bug is fixed,
`make test-effects` will fail at this point. The new
`m8_12_self_delegating_handler` target lives after `m8_9_supervision`
and before `m12_8_y_phase4_*`, so it never executes under the broken
target run; verification is via the manual command above.

---

## R3 — `examples/minimal/interp.kai` panics with `non-exhaustive match` under stage 2

**Status**: **FIXED** 2026-04-29 evening. Bisect localised the
regression to `9fe6f6d` (`runtime,emit: plug match-scrutinee +
short-circuit + prelude leaks`) — the hypothesis below was
correct.

**Root cause**: that commit added `kai_decref(_scr)` at every
match-exit path, on the premise that perceus_pass would dup
let-bound variables before they reached the match. Inspection of
the emitted C for `interp.kai`'s `main` showed otherwise:

```c
KaiValue *kai_e = kai_variant(...);   /* RC=1 */
kai_prelude_print(kai_string_concat(
  kai_to_string(kai_show(kai_e)),     /* call 1: kai_e raw */
  kai_string_concat(kai_str(" = "),
    kai_to_string(kai_prelude_int_to_string(
      kai_eval(kai_e))))));            /* call 2: kai_e raw */
```

Neither call site wrapped `kai_e` in `kai_internal_dup`. Pre-fix
the match leaked the scrutinee, masking the missing dup. Post-fix
the match's `kai_decref(_scr)` released `kai_e` after `kai_show`
ran, and `kai_eval` read freed memory whose variant tag pointed
to garbage — dropping into the `panic("non-exhaustive match")`
branch.

**Fix**: `emit_match_expr` now emits `kai_incref` on entry to the
match so the scrutinee's refcount is net-zero across the match.
Each PBind still gets its own incref (m5.x Step A); the entry
incref + exit decref pair just keeps the producer slot intact
while the match runs. The leak savings of `9fe6f6d` for the
match-scrutinee piece are reduced back to zero, but the and/or
short-circuit fix and the 12 prelude helpers from the same
commit still pay their way.

The deeper bug — perceus_pass not duping let-bound variables that
are read more than once across an expression tree — is now pinned
under `docs/m5x-followup.md` as a follow-up. Once perceus_pass
learns to dup, the entry incref here can drop again to recover
the savings.

`make test` clean (`test-run` no longer aborts), `make selfhost`
byte-identical, `make demos-no-regression` baseline 23.

Original report below.

---

**Original status**: OPEN as of 2026-04-29 evening. Predates the
in_dispatch fix (reproducible on `12bce96` before any of the
local commits). `make selfhost` is unaffected (byte-identical
codegen between stage 1 and stage 2 even when both produce a
panicking binary), so the gate stayed green; `make test`'s
`test-run` target catches it and aborts.

### Symptom

```
$ ./stage2/kaic2 examples/minimal/interp.kai > /tmp/i.c
$ cc -std=c99 -I stage0 /tmp/i.c -o /tmp/i
$ /tmp/i
panic: non-exhaustive match
```

The same source compiled with `stage1/kaic1` runs cleanly and
prints `(2 + (3 * 4)) = 14`. So the bug is in the stage 2 emitter
(or runtime path it triggers), not in `interp.kai`.

### Hypothesis (confirmed)

Likely candidate: the match-scrutinee leak plug in `9fe6f6d`
(`runtime,emit: plug match-scrutinee + short-circuit + prelude
leaks`) changed how match arms decref the scrutinee. If a code
path now extracts a variant payload after the scrutinee was already
released, the runtime sees garbage and lands in the catch-all
"non-exhaustive" branch. Stage 1 doesn't carry that plug yet, so
its emit avoids the issue.

---

## R4 — discarding a `Fiber[T]` value (`let _ = fiber_spawn(…)`) deadlocks the scheduler

**Status**: **FIXED** 2026-04-29 (Fibers Tier 1 lane). Root cause was
exactly the hypothesis below. Fix: scheduler-side fiber RC discipline
(Option A from the lane brief).

Concretely:

- `kai_default_spawn_spawn` now allocates the wrapper before enqueue
  and `kai_incref`s it. The wrapper RC therefore starts at 2: one for
  the caller (the user-visible `Fiber[T]` handle) and one for the
  scheduler.
- `kai_fiber_trampoline`'s DONE/CANCELLED tail `kai_decref`s
  `self->value` after walking awaiters, before dequeue + setcontext.
  That single decref pairs with the spawn-side incref.
- The decref may bring RC to 0 while the trampoline is still running
  on `self`'s private stack. `kai_free_value`'s `KAI_FIBER` branch
  detects `v->as.fib == kai_current_fiber()` and defers struct + stack
  free into a single-slot `kai_pending_free`. The next fiber drains
  the slot at trampoline entry / post-`swapcontext` in yield/park.
- Awaiters and `select` did not need to change: the caller's
  borrowed `fib_v` keeps the wrapper alive for the duration of the
  `Spawn.await(fib)` call (the caller's stack frame is suspended on
  the park).

`examples/effects/m8_fiber_discard.kai` now passes (the fixture moved
out of `make stress-fixtures`'s expected-fail block) and a sibling
`examples/effects/m8_fiber_discard_yields.kai` covers the same shape
with an explicit `fiber_yield()` between sends.

`make tier1` clean; `make demos-no-regression` baseline 24 holds;
`demos/ping_pong/` round-robin output unchanged.

Original report below for posterity.

---

**Original status**: OPEN as of 2026-04-29 evening. Surfaced while writing
`demos/ping_pong/`. Workaround documented in the demo: bind every
spawned Fiber to a real name and call `fiber_await` before exiting
the `with_mailbox` body.

### Symptom

```kai
fn run() : Unit / Actor[String] + Spawn + Console = {
  let me = Actor.self()
  let _ = fiber_spawn(() => worker(me, "hi"))   # discard via `_`
  Stdout.print(Actor.receive())
}
```

```
$ ./binary
kai: deadlock — fiber parked with empty run queue (2 parked total)
```

The same source with `let f = fiber_spawn(…)` followed by
`fiber_await(f)` runs to completion. The bug triggers regardless of
whether the worker yields, recurses, or sends a single message.

### Hypothesis

`fiber_spawn` returns a `Fiber[T]` value whose RC starts at 1.
`let _ = …` is treated as a discard, so the RC drops to 0
immediately. Under the R1 Perceus flip, RC=0 frees the value via
`kai_free_value`'s `KAI_FIBER` branch. But the scheduler still
holds a raw `KaiFiber *` in its ready queue (and the trampoline
walks Phase-5 link chains), so the freed struct gets picked up
later as garbage memory and the cooperative loop wedges.

The fix is per-fiber RC discipline: either
(a) the scheduler takes its own incref when it enqueues the fiber
    and decrefs in the trampoline DONE / CANCELLED branch, OR
(b) the Fiber value's KAI_FIBER drop path checks `state` and
    no-ops when the fiber is still on the run queue (defers the
    free to the trampoline).

Option (a) is the cleaner contract — every reference path holds
its own ref. Option (b) is closer to a tactical patch.

### Repro

Minimal — three lines suffice:

```kai
import actor; import spawn
fn worker(t: Pid[String]) : Unit / Actor[String] + Console = Actor.send(t, "hi")
fn run() : Unit / Actor[String] + Spawn + Console = {
  let me = Actor.self(); let _ = fiber_spawn(() => worker(me))
  Stdout.print(Actor.receive())
}
fn main() : Int / Console + Spawn = { with_mailbox { run() } 0 }
```

### Workaround for callers

Bind every spawned fiber and `fiber_await` it before exiting the
mailbox / spawn scope:

```kai
let f = fiber_spawn(…)
…
fiber_await(f)
```

### Verification path

After a candidate fix:
- `demos/ping_pong/main.kai` rewritten with `let _ = fiber_spawn(…)`
  for at least one of the three workers — should still produce the
  round-robin output.
- New fixture `examples/effects/m8_fiber_discard.kai` with the
  three-line repro above, hooked into `make test-effects`.

Out of r4-mc-callsite-rewrite scope; deserves its own runtime
lane post-m4c.

(Both verification artifacts shipped with the fix; the
`m8_fiber_discard.kai` fixture is now part of `make test-effects`,
and `m8_fiber_discard_yields.kai` covers the discard + yield shape.)

---

## R5 — `demos/euler4` segfaults on Linux runtime; passes on macOS

**Status**: **FULLY RESOLVED** 2026-05-01 (issue #42) — the
self-tail-call goto rewrite is now mirrored into both
`stage0/emit.c` (the C bootstrap) and `stage1/compiler.kai`
(kaic1's emit), in addition to the original landing in
`stage2/compiler.kai` from issue #37. Every binary in the
bootstrap chain (kaic0 → kaic1 → kaic2) and every program
kaic2 emits runs with O(1) C-stack on tail-self-recursive
fns. The transitional `kai_runtime_bump_stack_rlimit`
constructor and `<sys/resource.h>` include have been
removed from `stage0/runtime.h`; the `STACK :=
ulimit -s …` workaround in `stage2/Makefile` is now an
empty no-op variable for the same reason. Verified locally
under `ulimit -s 8192` (Linux's default 8 MiB main-thread
stack): `make tier0` and `make tier1` both stay green and
selfhost remains byte-identical at both kaic1 and kaic2.

**Final fix for emitted programs (issue #37)**: the C emitter
rewrites every self-tail-call into a `goto`-loop with
parameter rebinding. A new `tcrec_rewrite_decls` pass
between unboxing and Perceus replaces the callee `EVar` of
every tail-position self-call with a sentinel
(`__kai_tcrec|<c_sym>|<dropmask>|<p0>|<p1>|...`); the C
backend then emits a rebind+goto block instead of a
`kai_<sym>(...)` call, with `_kai_<c_sym>_entry:;` planted
before the enclosing `return ({ ... });`. Every kaikai
self-tail-recursive fn in **kaic2-compiled** code now uses
constant C-stack space, so `demos/build/euler4-bin` runs
with `ulimit -s 256` on macOS.

**Bootstrap-chain closure (issue #42)**: the rewrite is now
mirrored at both lower stages so kaic1's binary (built from
kaic0's emit) and kaic2's binary (built from kaic1's emit)
also run with O(1) C-stack on their internal recursive
passes. `stage0/emit.c` walks each fn body before emit,
marks every tail-position N_CALL whose callee is the
enclosing fn with a `TCO_TAIL_CALL` flag on the AST node,
and emits a rebind+goto block at the call site; the
matching `_kai_<name>_entry:;` label is planted by
`emit_fn_body`. `stage1/compiler.kai` carries the same
logic in kaikai-minimal — the same sentinel encoding as
stage 2, threaded through the existing pipeline after
`perceus_pass`. Stage 1 reuses stage 2's per-call dropmask
based on `last_use_for` / `pcs_count_non_lam_uses`; stage 0
mirrors that predicate against its existing single-use
counter (`total_count == 1 && lambda_count == 0` → ref
already transferred → no drop, otherwise → drop).

**Why the runtime bump came first**: the runtime
`kai_runtime_bump_stack_rlimit` constructor unblocked CI in
~30 lines and shipped same-day. The proper fix touches
`emit_call_expr`, `emit_fn_body`, the run pipeline, and every
fixture's expected C — appropriate for its own lane (which
became issue #37 / this branch).

---

**Interim fix (PR #36, removed by issue #37)** — runtime
constructor bumps `RLIMIT_STACK` at startup (`stage0/runtime.h:
kai_runtime_bump_stack_rlimit`). Root cause was none of the
three ranked hypotheses below; surfaced as H4 during local
repro on the Ubuntu CI box.

**Verification path**:

```sh
ulimit -s unlimited && demos/build/euler4-bin   # pre-fix proof: PASSES
                                                 # → confirmed stack overflow,
                                                 #   not guard pages / signal
ulimit -s 8192      && demos/build/euler4-bin   # post-fix: PASSES anyway
                                                 # → constructor in runtime.h
                                                 #   bumps RLIMIT_STACK before
                                                 #   `main` runs.
```

**Why H1/H2/H3 were wrong**:

- H1 (stack guard pages from PR #28). Did not apply: euler4
  spawns no fibers, so `kai_install_fiber_sigsegv_handler` was
  never called. The handler that would have decorated a
  guard-page hit was not even installed.
- H2 (`kai_main_fiber` `-Wmissing-braces`). The diagnostic was
  cosmetic; C99 zero-fills missing fields and the resulting
  layout matched mac's. `cancel_pad` ends up zero either way,
  and main_fiber never runs through the trampoline that uses
  the pad.
- H3 (`__builtin_*` ABI difference). euler4 is arithmetic-only
  on cached `kai_int` paths; `__builtin_popcount` and friends
  do not appear on the hot path.

**Real root cause (H4)**: the C the emitter generates for
self-tail-recursive functions wraps the recursive call in a
statement-expression like

```c
return ({ KaiValue *_r = (cond ? base : kai_search(...));
          kai_internal_drop(i); kai_internal_drop(j);
          kai_internal_drop(best); _r; });
```

The Perceus drops *after* the call inhibit C-level tail-call
optimisation under both gcc-13 and clang-18 on Linux —
verified at `-O2`, the symbol `kai_search` is reached with
`call kai_search` rather than `jmp`. With `search` recursing
~1 M deep on Project Euler #4 (1000 × 1000 grid plus
~log10(prod) `rev_loop` per iteration, no TCO collapse), the
8 MiB glibc default main-thread stack overflows. macOS shipped
a more permissive default that happened to fit, so the same
binary worked there.

**Fix shape**: a `__attribute__((constructor))` registered at
runtime startup raises `RLIMIT_STACK` from the kernel default
to 256 MiB (or `rlim_max`, whichever is smaller). Linux honours
the new limit on subsequent automatic stack-grow page faults,
so the kernel grows the main-thread stack as deep as the
program needs. No-op where the soft limit is already
permissive (most macOS configurations).

**Why this is the right shape, not the right destination**: the
proper fix is emitter-side recognition of self-tail-calls
followed by a `goto`-loop rewrite (or moving the drops to
*before* the recursive call wherever they can run there). That
is invasive — touches `emit_match_expr`, the Perceus pass, and
every fixture's expected C — and not worth blocking R5 on. The
runtime bump is a one-liner that gets every existing demo
passing under CI today; the proper fix moves to a follow-up
lane (`docs/m5x-followup.md` candidate).

**What changed**:

- `stage0/runtime.h`: added `#include <sys/resource.h>` and the
  `kai_runtime_bump_stack_rlimit` constructor.
- `.github/workflows/tier1.yml`: dropped `BASELINE=23`; CI now
  reads the same `demos/baseline.txt = 24` as local mac.
- `CHANGELOG.md`: entry under `[Unreleased]`, `### Fixed`.

Original report below for posterity.

---

**Original status**: OPEN, surfaced by the first GitHub Actions CI run
on PR #33 (2026-04-30). Not Linux-distro-specific in any obvious
way: ubuntu-latest under both gcc-13 (default `cc`) and clang-18
(`make CC=clang`) reproduce the segfault; macOS Darwin 25.4 with
clang passes. CI baseline on Linux is dropped to 23 (vs the local
mac baseline of 24) until R5 is fixed; see
`.github/workflows/tier1.yml` (`BASELINE=23`) and the local
`demos/baseline.txt` (still 24).

**Symptom**: `make demos-no-regression` reports
`euler4 FAIL runtime: Segmentation fault (core dumped)` on Linux.
The same compiled stage-2 self-host binary produces correct output
for euler1 / 2 / 3 / 5 / 6 in the same run.

**Repro on Linux**:

```sh
git checkout main          # at or after 7c71bbf
make CC=clang demos-no-regression
# euler4 line shows SIGSEGV
```

**Source under suspicion**:

```
fn search(i: Int, j: Int, best: Int) : Int = {
  if i > 999 { best }
  else if j > 999 { search(i + 1, i + 1, best) }
  else {
    let prod = i * j
    let nb = if is_palindrome(prod) and prod > best { prod } else { best }
    search(i, j + 1, nb)
  }
}
```

`search` and the inner `rev_loop` are both tail-recursive; the
demo Makefile compiles the emitted C with `-O2`, so TCO should
fire under either compiler. ~405 K iterations of `search` with
log10-deep `rev_loop` per iteration (~2.5 M function calls in
total under -O2 TCO collapse, more without).

**Hypotheses, in decreasing order of likelihood**:

1. **Stack guard pages (PR #28) interact poorly with Linux signal
   handling at high call-frequency.** The mac branch of the guard
   was implicitly tested when the lane landed; the Linux branch
   was not, since this is the first Linux run. The signal handler
   may not re-arm correctly after a near-miss touch, or the
   `mprotect` page sits at the wrong offset relative to glibc's
   stack growth direction.
2. **`KaiFiber kai_main_fiber` static initializer (runtime.h:390)**
   has `-Wmissing-braces` warnings on both gcc and clang on Linux.
   The C standard zero-initializes missing fields, so the diagnosis
   should be cosmetic, but a struct-layout difference between the
   mac and Linux builds (e.g., a flexible-array trailing member
   that the missing braces let drift) would explain a runtime
   crash that compiles clean.
3. **`__builtin_*` ABI difference** — euler4 uses arithmetic only,
   no obvious builtin, but the `kai_int` cache + `kai_alloc`
   constructors do go through paths that use builtins (popcount,
   etc.). Less likely.

**Fix-verification path**:

1. Reproduce on Linux locally (lima, docker, or a remote Linux
   box). `lima` is recommended — it gives a real x86_64 Linux VM
   without leaving mac.
2. Run under `gdb` with `set follow-fork-mode child`. Capture the
   stack at SEGV. If the top frame is in `kai_alloc` /
   `kai_decref`, the bug is in the runtime's per-iteration alloc
   path. If it is in `search` itself, TCO is not firing and a
   stack-frame fix is needed.
3. If it's the guard-page hypothesis, the fix is in
   `kai_fiber_install_guard` (or whatever PR #28 named it) plus
   the signal handler. Tier 2 honest claim about fibers Tier 1
   needs the Linux numbers updated in
   `docs/fibers-honesty-targets.md` after the fix.
4. Once fixed, drop `BASELINE=23` from `.github/workflows/tier1.yml`
   and let CI use the same `demos/baseline.txt` as local mac.

**Why CI ships with the gap rather than blocking on the fix**:
the value of having CI for *new* regressions today is larger than
the cost of a single pre-existing demo failing on Linux only. The
gap is documented and bounded; if any other demo flips to FAIL on
Linux, CI catches it because the baseline (23) still trips.

---

## Demos failing inventory (as of v0.7.1, 2026-04-29)

`make demos-no-regression` reports **20 passing (baseline 20)** plus
**6 failing**. **None of the 6 are regressions** — they fall into two
categories:

1. **Aspirational** — demos that intentionally use syntax for features
   not yet shipped (m7b #5b sugar, tuples).
2. **Pre-existing pre-m14 v1** — demos that depend on language pieces
   or stdlib pieces that were never finished (regex anchors, demo
   not updated to use `!` postfix on `Option`, module path resolution
   plus legacy `println`).

Documenting each failure with root cause and fix path so the next
agent does not need to re-discover. Updating this section is part of
any milestone close that changes the failure inventory.

### Aspirational (intentionally future-looking)

These demos declare in their own comments that they exercise sugar
forms not yet landed. Their FAIL is the expected status until the
feature ships.

#### `demos/state/main.kai` — m7b #5b sugar

```kai
fn make_counter() : () -> Int / Mutable = {
  var n = 0
  () => {
    n := n + 1
    n
  }
}
```

**Error**:

```
type mismatch in return type of make_counter
expected: () -> Int / Mutable
found:    () -> ? / State[?t1] + ?e0
```

**Cause**: `var n = 0` desugars to `with State[Int](0) as n` — the
typer infers `State` effect, not `Mutable`. The `var → Mutable`
collapse is m7b #5b sugar (see `docs/syntax-sugars.md`), not yet
landed. Demo intentionally uses the sugar form; for the explicit
parametric handler that runs today, see `demos/state_explicit/`.

**Fix path**: m7b #5b sugar lane (post-MVP polish).

#### `demos/stack/main.kai` — m7b #5b sugar (handler-clause scope)

```kai
fn run() : Unit / Stack + Console = {
  with Stack {
    var xs = []
    push(x) -> { ... }
    pop()   -> ...
  }
}
```

**Error**: `expected operation name in handler` at `var xs = []`.

**Cause**: `var` inside a handler clause body would desugar to a
sibling `with State[T](init) as xs`, but that handler-clause-scope
extension to m7b #5b is also not landed. Demo intentionally
aspirational. For the explicit parametric handler that runs today,
see `demos/stack_explicit/`.

**Fix path**: m7b #5b sugar lane (post-MVP polish), same as `state`.

#### `demos/toquefama/main.kai` — tuples (REJECTED in m8.5)

```kai
fn count_toques(guess: [Int], target: [Int]) : Int = match (guess, target) {
  ([], _)                  -> 0
  (_, [])                  -> 0
  ([g, ...gs], [t, ...ts]) -> { ... }
}
```

**Error**: `expected ')' after expression` at the parenthesised
match scrutinee `(guess, target)`.

**Cause**: tuple match expressions. m8.5 (2026-04-27) measured a
parser-combinator suite and **rejected tuples** as a second product
form (n=7, generic-record baseline beat tuples on both LOC and
signature length). The tuple syntax is not coming back.

**Fix path**: rewrite the demo to either `match a, b { PatA, PatB ->
... }` (m7d §27 multi-arg match sugar, post-m14 v1.A) or wrap inputs
in a `Pair[a, b]` record. Demo migration, not language change.

### Pre-existing — feature deferred

These demos exercise stdlib or language pieces that are deferred to
known follow-up lanes.

#### `demos/forth/main.kai` — FIXED 2026-04-29

Refactored to extract `token_of` from the lambda passed to `map`, so
the `Fail` effect can propagate from a real fn body (the `!`
postfix doesn't work inside lambdas, so the path here was
explicit `match string_to_int(n) { Some(i) -> TNum(i); None ->
Fail.fail("...") }`). Also switched the inner pipe from `|>`
(apply) to `|` (map) for concision: `s |> string_split(" ") |
token_of` reads more naturally than the previous
`s |> string_split(" ") |> map((w) => match w { ... })`.

Originally renamed the local `fn show(stack: [Int]) : String` to
`fn render_stack(...)` because the bare `show` collided with the
`Show.show` protocol method (single-dispatch picked the protocol
even though there is no `Show for List`). Same name-clash class as
the `resolver-arity-aware` lane targets, but at same-arity instead
of mismatched-arity.

**Resolved separately (v0.8.1, lane: resolver-local-shadow).** The
pre-resolve dispatcher rewrite now drops every op entry whose
`(name, arity)` is provided by a top-level DFn in the compilation
unit, so local fns shadow same-name same-arity protocol ops.
`render_stack` is back to `show`; both call forms (`show(result)`
and bare references) resolve to the local fn.

The golden was wrong (`3` vs the correct `-3`) — Forth convention
is `[7,4] -` → `4 - 7 = -3`. Updated.

Original report below for posterity.

##### Original report

#### `demos/forth/main.kai` — `!` postfix on `Option` (demo not updated)

```kai
fn tokenise(s: String) : [Token] / Fail = {
  s |> string_split(" ") |> map((w) => match w {
    n -> TNum(string_to_int(n))   # string_to_int returns Option[Int]
    ...
  })
}
```

**Error**:

```
type mismatch in function call
expected: (Int) -> Token
found:    (Option[Int]) -> ?t7 / ?e3
```

**Cause**: `string_to_int` returns `Option[Int]`. The demo passes the
`Option` directly to `TNum(_)` which takes `Int`. The intent is
`TNum(string_to_int(n)!)` — propagate the `None` to the enclosing
`Fail` handler via `!` postfix.

`!` postfix on `Option` IS landed (m7e §13, v0.1.0). Demo just
hasn't been updated to use it.

**Why the obvious 5-min fix doesn't work** (foreground experiment
2026-04-29): replacing the call site with
`TNum(string_to_int(n)!)` fails with a different error:

```
error: `!` is only valid inside a function body
help: `!` propagates from the enclosing function; it cannot appear
inside a lambda or at top level
```

The `!` postfix is restricted to the immediately enclosing fn body;
the call site here is inside a `(w) => match w { ... }` lambda
passed to `map`. The lambda has no Fail row that `!` can propagate
through.

**Fix path**: ~30 min refactor — extract `token_of` from the
`tokenise` lambda into a top-level `fn token_of(w: String) : Token
/ Fail`, then call `tokens.map(token_of)`. The `!` lives inside
`token_of`'s body and propagates correctly. Demo edit only, no
language change needed.

(A more invasive language fix would be to allow `!` to escape
through a lambda whose enclosing fn has the matching `Fail` row.
Out of scope for this demo.)

#### `demos/mini_ledger/main.kai` — regex anchors

```kai
type AccountId = String where matches /^acc_[a-zA-Z0-9]{8}$/
```

**Error**: `unexpected character '$'` at the closing anchor.

**Cause** (re-diagnosed 2026-04-29): the regex stdlib already
supports `^` / `$` end-to-end (`RxAnchor`, `TAnchor`, NFA threading
in `stdlib/regexp.kai`). The actual blocker is upstream: kaikai
has no `/.../` regex-literal syntax in the lexer. The character
between `where matches` and the type's closing structure is just
parsed as expression tokens, so `/^acc_...{8}$/` lexes as
`/`, identifier, `[a-zA-Z0-9]`, ..., `$` — and `$` is not a
valid token in expression position.

**Fix path**: full regex-literal lane. Lexer change to detect
`/.../` (with the usual ambiguity heuristic vs the `/` operator —
look at the previous token; division only after expressions, regex
only after operators / keywords / open brackets). Parser branch
for `matches <regex_literal>` predicates. Emit / runtime hookup so
the regex source feeds `rx_parse_pattern` once at compile time
(refinement predicate) or the call site (general use). Bigger
than the half-day estimate this entry used to carry — defer to a
proper lane.

#### `demos/spiral/main.kai` — FIXED 2026-04-29 (v0.9.2)

Three lanes had to land before this demo compiled cleanly:

1. **resolver same-module preference** (v0.9.1) — unblocked the
   `repeat` collision between `stdlib/loop.repeat` and
   `stdlib/core/list.repeat`.
2. **didactic error for bare cap reads** (v0.9.2) — turned the
   spiral source's bare `n`, `top`, ... reads into a typed
   error pointing at each call site with a `@name` / `name :=
   <expr>` help line. The original demo predated the `var`
   sugar's actual semantics; the new diagnostic surfaced every
   bad site at once.
3. **closure capture of `kai_alias_<a>_id`** (v0.9.2) — the
   tagged-op dispatch now stays enabled across lambda
   boundaries because the closure literal carries the
   enclosing handle's handler id as an `__alias_id__<a>`
   sentinel cap, packed as a `kai_int` in the capture array
   and unpacked by the lambda body prologue. Without this the
   inner while body's `@n` was routed to the innermost State
   handler by name and silently shadowed by every nested
   `var`. Nested-lambda free vars also now propagate to the
   outer closure's capture set so `grid` / `dim` / cap ids
   referenced by an inner closure stay visible at the outer's
   construction site.

After the demo migration to `@n` / `n := @n + 1`, the 4×4
clockwise spiral renders correctly:

```
1 2 3 4
12 13 14 5
11 16 15 6
10 9 8 7
```

Demos baseline raised 22 → 23.

##### Original report

#### `demos/spiral/main.kai` — module path + legacy `println`

```kai
import loop

fn fill(grid: Array[Int], dim: Int) : Unit / Mutable = {
  ...
  if c >= dim { println(acc) }
}
```

**Errors** (in order, with full prelude chain):

1. `cannot open module 'loop' (tried demos/spiral/loop.kai)` —
   module path resolution.
2. `undefined name 'println'` — bare `println` does not resolve to
   the `Stdout` effect post-m12.8 Phase 4b atomic-effects split.

**Cause**:

1. The `import loop` resolver looks under the demo's directory, not
   `stdlib/`. m6.2 v1 ships the `--path stdlib` flag for this; the
   `demos/Makefile` passes it but the bin/kai wrapper does not by
   default.
2. `println` is a legacy bare builtin (pre-m7a). After m12.8 Phase 4b
   the canonical surface is `Stdout.println(s)` (or `println(s)`
   with `use Stdout` in scope). The demo predates that.

**Why the obvious 10-min fix doesn't work** (foreground experiment
2026-04-29): adding `use Stdout` and `--path "$ROOT/stdlib"` to
`bin/kai`'s `compile_to_binary` got past the original errors but
surfaced a deeper bug:

```
warning: bare name 'repeat' is exported by multiple modules with no
root-file shadow: list, loop; use a qualified call (e.g.
list.repeat(...)) to disambiguate
error: type mismatch in function call
expected: (Int, () -> Unit / ?e1) -> Unit / ?e1
found:    (Int, Int) -> ?t3 / ?e2
```

`stdlib/loop.kai` exports `repeat(n: Int, body: () -> Unit / e)`
(control-flow combinator) and `stdlib/core/list.kai` exports
`repeat(x: a, n: Int) : [a]` (list of n copies). Different arity,
different types — but the resolver picks one by name without
arity-filtering and the call site fails to type-check. This is the
same bug the `resolver-arity-aware` lane is fixing right now.

**Fix path**: blocked on `resolver-arity-aware` (in flight). Once
that lane lands, the `use Stdout` + `--path stdlib` edits become a
~10-min demo+driver fix as originally estimated.

**Update 2026-04-29**: resolver-arity-aware shipped (v0.7.2) and
the same-name same-arity collision was diagnosed deeper: when
`stdlib/loop.repeat` (arity 2) coexists with `stdlib/core/list.repeat`
(arity 2 with different signature), the typer needs same-module
preference to resolve `repeat(...)` recursively from inside a body
that lives in `loop`. v0.9.1 added `fns_prefer_module` in both
backends (`emit_fn_body`, `llvm_emit_fn`) — the EFn table is
rotated so same-module entries come first, and `efn_resolve`'s
ambiguous fallback returns the first match. `stdlib/core/list.kai`
also has `list_repeat_loop` as the internal recursive helper so
the user-facing `repeat` and `list_repeat` no longer call each
other through the global namespace.

The `repeat` collision is gone (verified: `kai_loop__repeat` and
`kai_list__repeat` both link cleanly when `import loop` is used
alongside the prelude). `spiral` still fails with a separate bug
in the `var` desugar — references to `r2`, `left`, `n` inside
the inner `while { ... }` blocks emit as bare `kai_<name>` instead
of `State` capability reads, so `cc` rejects with `use of
undeclared identifier`. That belongs to a `var`-desugar lane.

### Categorization summary

| Demo | Category | Effort | Owner |
|---|---|---|---|
| `state` | aspirational m7b #5b | post-MVP | wait for sugar lane |
| `stack` | aspirational m7b #5b | post-MVP | wait for sugar lane |
| `toquefama` | aspirational tuples (REJECTED) | demo migration | rewrite to `Pair` or multi-arg match |
| ~~`forth`~~ | **FIXED 2026-04-29** | ~30 min refactor done | extracted `token_of`, `|` map pipe, renamed `show` → `render_stack`, fixed golden |
| `mini_ledger` | regex anchors not parsed | 0.5d | m12.6.x #7 lane |
| `spiral` | repeat collision (loop vs list) | blocked | wait for `resolver-arity-aware` lane |

**Foreground experiment results (2026-04-29)**: the original audit
estimated `forth` at 5 min and `spiral` at 10 min. Both hit deeper
issues than expected:
- `forth`'s `!` does not work inside a lambda — needs a refactor,
  not a 1-line edit.
- `spiral`'s `import loop` collides with `stdlib/core/list.kai` on
  the `repeat` name — blocked until the resolver learns to filter
  candidates by arity (lane in flight).

`mini_ledger` waits for the regex anchor lane regardless.

`state`, `stack`, `toquefama` stay failing as honest reminders of
deferred features — fixing them either changes the demo's intent
(state / stack) or rewrites it for a feature that will not ship
(toquefama).

### Demos baseline policy

`demos/baseline.txt` records the minimum count of OK+PASS demos
that must keep passing on every commit. Editing the baseline is
allowed only when:

- A demo legitimately becomes aspirational (its feature was retired
  or deferred), at which point the baseline drops by 1.
- A demo that was previously failing flips to OK, at which point
  the baseline rises by 1.

Demos in this section with category "aspirational" or "feature
deferred" are NOT counted in the baseline; they fail by design
until their upstream lane lands.

---

## R6 — TCO precise per-call-site dropmask (rule 3) cannot land under current Perceus emit

**Status**: **OPEN — re-land blocked** as of 2026-05-01 (issue #43).

**Symptom**: any `tcrec_compute_site_dropmask` shape that distinguishes
`LUAt + count==1 AND p NOT in args` from `LUAt + count==1 AND p in
args` (rule 3) makes the kaic2 self-compile abort on Linux ubuntu-clang
with `malloc(): unaligned tcache chunk detected → core dumped`.
Three structurally different attempts have hit the same abort:

1. **PR #41 first try** (reverted before merge):
   `tcrec_compute_site_dropmask(params, uses, args: [Expr], i, acc)` —
   threaded `args: [Expr]` directly, called `tcrec_args_have_evar(args, p)`
   from inside the dropmask function body.
2. **PR #48 first commit (522b9df)**: dropped `[Expr]` from the
   dropmask signature, added `tcrec_per_param_has_evar(args: [Expr],
   pnames: [String], i, acc)` as a new helper called from
   `tcrec_rewrite_kind`. The new helper still threads `[Expr]` and
   calls `tcrec_args_have_evar`.
3. **PR #48 second commit (bd54356)**: removed the helper entirely,
   replaced it with `map(pnames, (p) => tcrec_args_have_evar(xs, p))`
   inside `tcrec_rewrite_kind`. The lambda captures `xs` via closure
   so no helper takes `[Expr]` as a parameter.

All three abort identically on Linux. macOS Darwin malloc tolerates
the imbalance and the same builds pass `make tier1` locally — the
mac/Linux drift in glibc malloc-tcache strict mode is the same
pattern as R5 euler4 (issue #34).

**Locus** (per PR #41 8-step bisection, archived in PR #41's
force-push history at tag `backup-r37-pre-rebase`): a refcount
imbalance in stage 1's perceus emit for some shape involving
`[Expr]`, walking, and the `kai_internal_dup` chain that follows.
The PR #48 closure-via-`map` attempt suggested the suspect shape
is broader than just "function takes `[Expr]` parameter" — even
threading via lambda capture trips it. AddressSanitizer with
`--ulimit stack=64MB` reproduces the abort path as a stack
overflow in `kai_report_errors_loop`, but the mechanism is
secondary: stack overrun corrupts heap metadata, glibc's tcache
check fires on the next `malloc`. ASan's stack-overflow detector
fires before the heap corruption path; without ASan, glibc's
detector is what fails. Both signals point at the same root
cause: under-incref of some `[Expr]`-shaped value during the
dropmask computation, cascading through the call chain.

**What's documented in tree**:

- `examples/tco/list_nth_shape.kai` — canonical rule-3 shape
  (`nth_loop(xs, i)` with `xs` as match scrutinee, recursive call
  passing `(t, i - 1)`). Compiles + runs end-to-end under the
  conservative dropmask. The cons-cell leak the rule would close
  is bounded (one cell per iteration, 100k cells for a 100k
  walk) and not visible to a stdout-only golden.
- `make test-tco-regression` — runs the fixture, validates output.
  Does NOT enforce rule 3 today.
- `docs/lane-experience-tco-dropmask-regression.md` — full
  retrospective of the PR #48 attempts.

**Path forward (out of scope for this lane)**:

1. **Audit stage 1's perceus emit** for `[Expr]`-typed values and
   the dup/decref chain that wraps them. The bisection placed the
   bug there; PR #45's stage 1 mirror added more emit complexity
   and the abort still reproduces.
2. **Make the imbalance visible on macOS**, e.g., by adding strict
   alloc tracing to `runtime.h` (per-tag free counts, sentinel
   patterns on freed chunks). This unblocks local debugging and
   was already flagged as "not in scope" inside PR #48.
3. **Or: change the rule-3 representation entirely** — precompute
   a side table in a pass that runs before perceus and never
   consults `[Expr]` values during the dropmask computation. The
   side table key is the call site's source location; the value
   is the per-param flag bitmask.

The conservative dropmask shipped in `main` today (rules 1 + 2)
is correct but coarse. The `nth_loop`-shape leak is bounded and
documented as a follow-up. R6 is the inverse of "fix and ship":
the fix is structurally clear, the *channel* to ship it through
is broken.

**Related**: issue #43, PR #41 (force-push history), PR #48
(closed). The dropmask logic in `tcrec_compute_site_dropmask` is
inline-commented at `stage2/compiler.kai:24121` with a forward
pointer to this section.

---

## R7 — kai fmt v1: same-line trailing comments may be promoted to the line above

**Status**: **KNOWN LIMITATION** as of 2026-05-01 (Tongariki kai
fmt v1, this lane).

**Symptom**: a comment written at end-of-line on the same line as
code, e.g.

    let x = 42  # answer to everything

may be re-emitted by `kai fmt` as a full-line comment placed
immediately before the construct it was attached to:

    # answer to everything
    let x = 42

The semantic content is preserved (the comment is not lost); only
its position relative to its line shifts. Idempotency holds: a
second `kai fmt` run is a no-op.

**Why**: the lexer (`stage2/compiler.kai:lex_skip_comment`) drops
line comments before the token stream reaches the parser, so AST
nodes have no end-of-line attachment slot. v1 collects comments
into a side list keyed by source line, then drains them at line-
boundary breakpoints during pretty-printing — sufficient for
between-decl and between-stmt comments, not for trailing ones.

**Repro**:

    cat > /tmp/t.kai <<EOF
    fn main() {
      let x = 42  # answer
    }
    EOF
    kai fmt /tmp/t.kai
    cat /tmp/t.kai

**Fix path**: a future Tongariki / Anga Roa lane (probably
combined with the m11 diagnostics-quality pass) needs to either
(a) annotate AST nodes with their source span end and attach
trailing comments at parse time, or (b) emit comments as
`TkComment` tokens during lexing and have the formatter walk
the token stream alongside the AST. Option (b) is the smaller
change and matches how gofmt handles the same problem.

---

## R8 — `let n = INT_LITERAL` followed by use inside `#{int_to_string(n)}` interpolation breaks the C build

**Status**: **OPEN** as of 2026-05-01 (Tier 3 arm A — Trace effect lane).

**Symptom**: kaic2 emits clean C from a body like

    fn main() : Unit / Stdout = {
      let n = 42
      Stdout.print("v=#{int_to_string(n)}")
    }

but `cc` rejects the resulting `.c` with two errors:

    error: use of undeclared identifier 'kai_n'; did you mean 'kair_n'?
    error: incompatible integer to pointer conversion passing 'int64_t' to
           parameter of type 'KaiValue *'

The let-binding is emitted as `int64_t kair_n = 42LL;` (unbox-phase 2
chose the unboxed form because the rvalue is a literal `Int`). The
later use, however, lives inside the desugar of the
`#{int_to_string(n)}` interpolation segment, which calls
`kai_prelude_int_to_string(kai_n)` — the **boxed** name. The two
emit passes disagree on which alias to spell.

**Repro** (any name; `n` and `value` reproduce identically):

    cat > /tmp/repro.kai <<'EOF'
    fn main() : Unit / Stdout = {
      let n = 42
      Stdout.print("v=#{int_to_string(n)}")
    }
    EOF
    cd stage2
    ./kaic2 /tmp/repro.kai > /tmp/repro.c            # OK
    cc -I ../stage0 /tmp/repro.c -o /tmp/repro       # FAIL: kai_n vs kair_n

**Workarounds that avoid the bug**:

- Replace interpolation with explicit `++`:
  `Stdout.print("v=" ++ int_to_string(n))` — works.
- Use a function parameter instead of a let-binding:
  `fn show(n: Int) ... Stdout.print("v=#{int_to_string(n)}")` — works.
- Pass a non-literal arithmetic expression directly without binding:
  `Stdout.print("v=#{int_to_string(40 + 2)}")` — works.

The Trace fixture (`examples/effects/trace_basic.kai`) was rewritten
to use `++` concatenation in the one place a `let`-bound `Int` flows
into a string-interpolation `#{...}` segment. The interpolation form
is kept everywhere else (function-parameter sources are fine).

**Why** (hypothesis): the unbox-phase-2 emit pass rewrites
`let n = <int_literal>` to a C `int64_t kair_<n>` local and records
`n -> kair_n` in its alias map for downstream `Var` references in
the same fn body. The string-interpolation desugar in the typer/lower
expands `"...#{e}..."` into `kai_string_concat(..., kai_to_string(
kai_prelude_int_to_string(<e>)))` *before* the unbox pass walks the
expression — or after, but with a different alias-lookup helper that
does not consult the unbox map. Either way the `Var` printed in the
interpolation slot keeps the boxed name `kai_n`, while the local was
renamed to `kair_n`.

**Fix path**: belongs to the unbox-phase-2 / interpolation interaction
work. Two paths forward:

- Make the interpolation desugar run **before** the unbox pass so the
  unbox alias map sees the synthesised arg position and either keeps
  the binding boxed (simplest) or boxes-on-use through a
  `kai_int(kair_n)` wrapper.
- Or: have the unbox pass walk into `StringInterp` segments when it
  scans for use sites of `let`-bound int locals, and emit
  `kai_int(kair_n)` at each interpolation slot.

The second is the smaller change but requires the unbox pass to know
about every desugar that produces a fresh `Var` reference — fragile.
The first is structural.

**Why this is documented here, not fixed**: the Trace lane (Tier 3
arm A) is stdlib-only and explicitly forbids touching emit/unbox
internals. Per `CLAUDE.md` lane discipline, this regression is logged
for a future emit-pass lane to pick up.

---

## R11 — Single-read of a stateful handler clause's `state` aliases the EvE storage; downstream decref-aware sinks (`string_concat`, `string_length`, …) free the prefix mid-handler

(Discovered separately in the Tier 3 arm A experiment 2 lane.
May be a more specific manifestation of R10's "parameterised
outer + self-delegating inner crashes on 2nd op call" surface,
or a distinct Perceus-pass bug. Diagnosis differs: this entry
attributes the crash to `pcs_is_non_last` returning false on a
single state read, leading to a raw pointer transfer that
downstream `kai_decref` chains then free out from under the
next op call. R10's diagnosis attributes the crash to
`in_dispatch_node` save/restore not handling parameterised
resume entries. Two-bug or one-bug-two-views is open until a
follow-up lane unifies the analyses.)

**Status**: open. Discovered 2026-05-01 in the Tier 3 arm A
experiment 2 lane while authoring `with_log_prefix` in
`stdlib/trace.kai`. Worked around at the source level; runtime
semantics need a compiler fix.

### Symptom

A parametric handler clause that reads `state` once and forwards
it to a consumer that decrefs (e.g. `kai_prelude_string_concat`,
`kai_prelude_string_length`, `kai_prelude_string_join`) frees the
storage backing `self->state` after the FIRST op call. The next
op call lookups the same evidence node, reads the freed slot, and
either prints empty / garbage or segfaults — order-dependent
based on heap reuse.

ASAN repro from the lane (commit prior to the workaround):

    AddressSanitizer: heap-use-after-free … 4 bytes inside of 40-byte region
    READ of size 4 at … in kai_string_concat runtime.h:1216
      #1 kai_prelude_string_concat runtime.h:1389
      #2 _kai_trace__with_log_prefix__…__clause_84_5_checkpoint tp.c:267
    freed by thread T0 here:
      #2 kai_decref runtime.h:830
      #3 kai_prelude_string_concat runtime.h:1390
      #4 _kai_trace__with_log_prefix__…__clause_84_5_log tp.c:272

The `_log` clause runs first and decrefs the value the EvE state
slot still aliases; the `_checkpoint` clause runs next and reads
the same now-freed pointer.

### Minimal repro (logical)

    effect P[T] { read() : T }

    fn use_twice(body: () -> Unit / Trace) : Unit / Trace = {
      handle {
        handle { body() } with Trace {
          log(msg, resume) -> {
            Trace.log(P.read() ++ ": " ++ msg)   # consumes P.read()
            resume(())
          }
          ...
        }
      } with P[String]("hello") {
        read(resume) -> resume(state)            # SINGLE state read
        return(x) -> x
      }
    }

The fixture in `examples/effects/trace_prefix.kai` exercises this
exact shape; before the source-level workaround, it produced two
empty `[trace] ` lines instead of the second/third prefixed
messages, and ASAN flagged the UAF as above.

### Hypothesis

`pcs_is_non_last` (`stage2/compiler.kai:23808`) wraps an `EVar`
read in `__perceus_dup` only when it has ≥2 non-lam uses (or any
in-lam use). A clause body with a single `state` read therefore
emits a *raw transfer* — `KaiValue *kai_p = self->state` — and
the consumer's decref bookkeeping reduces the EvE state slot's
refcount below 1, freeing the storage even though the EvE
itself is still on the evidence stack and will be read again by
the next op call.

The implicit invariant the perceus pass relies on is "the binding
owns its reference and the consumer is the last reader." For
ordinary locals this is sound. For `state` it is wrong: `state`
is an *alias* of `self->state`, and the storage's lifetime is the
handler's, not the clause's. The single-read case needs an
implicit dup, the same way the closure-capture path does
(`pcs_is_non_last` returns true unconditionally when `in_lam_here`).

### Workarounds (source-level, no compiler change)

The `with_log_prefix` `read` clause forces multi-use:

    read(resume) -> {
      let _keep_alive = state    # use 1 of state — perceus wraps in __perceus_dup
      resume(state)              # use 2 — perceus wraps in __perceus_dup
    }

Emitted C (relevant slice):

    return ({
      KaiValue *kai__keep_alive = kai_internal_dup(kai_state);
      kai_decref(kai__keep_alive);
      kai_cont_resume(k, kai_internal_dup(kai_state));
    });

Each read bumps `state`'s refcount; the dummy binding is dropped
immediately by perceus's exit-pass; the value passed to `resume`
carries the +1 the consumer's decref will burn off. Net per call
is +0 on `self->state` (and possibly a small leak on the dummy if
the exit drop is missing, which is acceptable under the loose
runtime per `pcs_is_non_last`'s comment).

A second workaround that also avoids the UAF: change the consumer
chain to one that does **not** decref its inputs. The string
prelude does — concat / length / join all decref. The arithmetic
prelude does not (Int + Int reads operands without decref), which
is why `Reader[Int]` plus `+ 1` (e.g.
`examples/effects/m7b_11_followup_distinct_types.kai`) has not
caught this bug for ~6 months.

### Fix path (compiler)

Make `pcs_is_non_last` treat the special clause names `state` and
`log` as always non-last (even with one use), so every read pays
a `kai_internal_dup`. The change is local to
`pcs_rewrite_clauses` (`stage2/compiler.kai:23772`): pass a flag
through the clause-scope extension and consult it in the dup
predicate. Stable runtime cost: one extra `kai_incref` per
stateful-clause `state` read, balanced by the consumer's existing
decref. No pre-existing fixtures regress because the only paths
that exercised stateful clauses with non-trivial state types
(Int, primitive) had RC sinks that did not decref.

### Why this is documented here, not fixed

The experiment 2 lane is stdlib-only — `with_log_prefix` lives in
`stdlib/trace.kai`. Modifying `pcs_is_non_last` would touch the
emit-pass internals, outside the lane's scope per `CLAUDE.md`.
The source-level workaround keeps `with_log_prefix` shippable
today; the structural fix belongs to a future Perceus / RC lane.

## R9 — handler clauses do not capture parameters of the enclosing fn

**Status**: **CLOSED** 2026-05-02 by commit `d3a4184` on lane
`r9-clause-env` (issue \#60). Captures detected in
`lc_record_clauses`, threaded through a per-handle env struct
allocated at the install site and read back via
`((env *) self->env)->kai_<name>` in the clause prologue. Each
read goes through `kai_internal_dup` so per-clause perceus
accounting stays valid across clause invocations. Fixture at
`examples/effects/r9_clause_capture.kai`. The C backend is the only
fixed half; the LLVM backend (`llvm_emit_clause_body`) still ignores
the captures field — same R9 symptom under `--emit=llvm`, scheduled
as a mirror follow-up after the C selfhost is stable.

**Status (historical)**: OPEN as of 2026-05-01 (Tier 3 experiment 2
arm B — `with_log_prefix` lane). Blocked any handler-composition
helper that needed to thread a *value* (not a previously-installed
effect) into its clause bodies.

**Symptom**: kaic2 accepts and emits

    fn make_logger[e](
      prefix: String,
      body: () -> Unit / Trace + e
    ) : Unit / Stdout + e = handle {
      body()
    } with Trace {
      log(msg, resume) -> {
        Stdout.print("[" ++ prefix ++ "] " ++ msg)
        resume(())
      }
      ...
    }

but `cc` rejects the resulting C with

    error: use of undeclared identifier 'kai_prefix'

at every clause-body site that names `prefix`. The emitter spells
the var as `kai_prefix` (the boxed alias of the outer parameter)
inside a stmt-expr that has no `prefix` in scope — handler clauses
are emitted as standalone C functions whose only inputs are
`(*EvE self, op_args..., k)` and the closure-env capture path is
not threaded.

**Repro** (any helper-with-param shape reproduces; this one is the
shortest):

    cat > /tmp/r9.kai <<'EOF'
    fn make_logger[e](
      prefix: String, body: () -> Unit / Trace + e
    ) : Unit / Stdout + e = handle {
      body()
    } with Trace {
      log(msg, resume) -> {
        Stdout.print("[" ++ prefix ++ "] " ++ msg)
        resume(())
      }
      checkpoint(name, resume) -> {
        Stdout.print("[" ++ prefix ++ "] cp: " ++ name)
        resume(())
      }
      return(x) -> x
    }

    import trace
    fn worker() : Unit / Trace = { Trace.log("hello") }
    fn main() : Unit / Stdout = make_logger("P", worker)
    EOF
    cd stage2
    ./kaic2 --path ../stdlib /tmp/r9.kai > /tmp/r9.c           # OK
    cc -I ../stage0 /tmp/r9.c -o /tmp/r9                       # FAIL: kai_prefix

**What the spec says should happen**: `docs/effects-impl.md`
§*Op clause as ordinary function* (lines 612–616) is explicit:

> Each op clause is compiled as an ordinary function with signature
> `(self: *EvE, op_args..., k: Cont[Ret]) -> Answer`. The `EvE`
> struct stores the function pointers and any per-handler state;
> **closure captures of the enclosing scope go through `self.env`**.

The `self.env` path is the spec'd channel for closing over the
enclosing fn's locals/params from inside a clause. The current
emitter emits the bare boxed name as if it were already in scope
and never installs anything in `self.env`.

**What works today** (so the blast radius is bounded):

- Plain effect-op calls inside a clause body (`Stdout.print(...)`,
  `Trace.log(...)`) — these resolve through the runtime evidence
  stack, not through closure capture.
- Parameterised handler `state` (e.g., `with State[Int](init) { get(resume)
  -> resume(state) }`) — the `state` keyword is part of the handler
  protocol, accessed via `*EvE`'s state slot, not a capture.
- String literals in clause bodies — no capture needed.
- Lambda capture *inside ordinary fns* (e.g., `(x) => prefix ++ x`
  bound to a let in the same fn) — works correctly. The bug is
  specifically about the **handler clause body**, not lambdas in
  general.

**Workarounds the spec leaves you, if you must build a stateful
handler today**:

1. **Push the value into a parameterised handler's `state`**, then
   read it via `Eff.op()` from the clause body. Conceptually this
   is what the spec means by "no closure capture needed if the
   value lives in `state`"; it works in isolation but trips R10
   below when combined with an inner self-delegating handler.
2. **Pass the value as the op's argument at every call site**,
   bypassing the helper entirely. Defeats the purpose of a helper.
3. **Emit a top-level helper fn that takes the value**, then call
   it from the clause body. Same problem — the call site still has
   to thread the value into the helper, and the value comes from
   the enclosing fn's params, which the clause cannot see.

**Fix path** (delivered 2026-05-02 in lane `r9-clause-env`):

The collect pass (`lc_record_clauses`) computes per-clause free vars
via the existing `fv_expr` walker, treating the clause's own params
plus the magic `state` / `log` aliases as bound. The captures field
on `ClauseInfo` accumulates the result (with `__alias_id__<a>`
sentinels filtered — alias-id capture in clauses is a follow-up).
At code-gen, `emit_handle_env_typedefs` mints one env struct per
`(enc_fn, line, col)` triple covering the union of every clause's
captures; `emit_handle` allocates the struct on the install-site
stmt-expression frame, populates one field per capture from the
surrounding `lcs`, and stashes a `void *` pointer in `_ev.env`.
The clause prologue (`emit_clause_env_prologue`) casts `self->env`
back to the env type and rebinds each `kai_<name>` via
`kai_internal_dup` — the dup is what makes per-clause perceus
accounting compose with the env's borrow scope (one shared env-side
borrow + one fresh refcount per clause invocation, vs. the early
draft that omitted the dup and freed captured strings after the
first invocation).

**RC contract**: the env struct holds a borrow scoped to the handle
stmt-expression. The caller's `kai_<name>` outlives the handle
body and is dropped in the surrounding fn's perceus epilogue.
The clause body's prologue dup gives perceus's per-clause analysis
a fresh reference it may consume freely; nothing else in the env
path runs incref / decref.

**Workarounds left in place**: `stdlib/trace.kai` still ships the
parameterised-handler workaround (`with_log_prefix` threads
`prefix` through a `TracePrefix[String]` state slot). The fix lets
that helper be rewritten to capture `prefix` directly, which is a
follow-up. Pointer left in the closing-commit message of the lane;
no in-line deletion in this PR.

**LLVM mirror**: `llvm_emit_clause_body` ignores the captures
field. The same R9 symptom reproduces under `--emit=llvm`. Spec'd
as a follow-up so the C backend can stabilise first.

---

## R10 — parameterised handler outer + self-delegating handler inner crashes/corrupts

**Status**: **OPEN** as of 2026-05-01 (Tier 3 experiment 2 arm B).
Surfaced while attempting to use R9's documented workaround
(parameterised handler `state` slot in place of closure capture).

**Symptom**: a `handle ... with Eff[T](init) { ... }` outer wrap
around a `handle ... with Eff2 { ... }` inner whose clause body
*self-delegates* (calls `Eff2.op(...)` from inside an `Eff2.op`
clause) crashes the runtime on the second op invocation. The
specific shape that reproduces:

    handle {
      handle {
        body()                                       # body: () -> R / Eff2 + e
      } with Eff2 {
        op(arg, resume) -> {
          let v = Eff[T].read()                      # read outer state
          Eff2.op(v ++ ": " ++ arg)                  # delegate to next outer Eff2
          resume(())
        }
        ...
      }
    } with Eff[T](init) { ... }                      # parameterised outer

Tested in two flavours, identical end state:

- `Eff[T] = Reader[String]`, `Eff2 = Trace`. SIGBUS on the third
  `Trace.log` invocation; first invocation produces correct output;
  second and third produce blank lines preceded by garbage memory.
- `Eff[T] = State[String]`, `Eff2 = Trace`. SIGSEGV on the second
  `Trace.log` invocation.

**Repro** (Reader variant):

    cat > /tmp/r10.kai <<'EOF'
    import trace

    pub fn with_log_prefix[R, e](
      prefix: String, body: () -> R / Trace + e
    ) : R / Trace + e = handle {
      handle { body() } with Trace {
        log(msg, resume) -> {
          Trace.log(Reader.ask() ++ ": " ++ msg)
          resume(())
        }
        checkpoint(name, resume) -> {
          Trace.log(Reader.ask() ++ ": checkpoint: " ++ name)
          resume(())
        }
        return(x) -> x
      }
    } with Reader[String](prefix) {
      ask(resume) -> resume(state)
      return(x)   -> x
    }

    fn worker() : Unit / Trace = {
      Trace.log("first"); Trace.log("second"); Trace.log("third")
    }

    fn main() : Unit / Stdout = with_trace_default() {
      with_log_prefix("P", worker)
    }
    EOF
    cd stage2
    ./kaic2 --path ../stdlib /tmp/r10.kai > /tmp/r10.c          # OK
    cc -std=c99 -Wl,-stack_size,0x8000000 -I ../stage0 /tmp/r10.c -o /tmp/r10
    /tmp/r10                                                    # SIGBUS / blank lines

State variant: replace `Reader[String](prefix) { ask(resume) ->
resume(state); return(x) -> x }` with `State[String](prefix) {
get(resume) -> resume(state); set(v, resume) -> resume((), v);
return(x) -> x }` and `Reader.ask()` with `State.get()`. Same
shape, also crashes (SIGSEGV on the second invocation rather than
the third).

**Negative control** (works correctly — confirms self-delegation
without parameterised outer is fine):

    fn delegating_helper[R, e](
      body: () -> R / Trace + e
    ) : R / Trace + e = handle {
      body()
    } with Trace {
      log(msg, resume) -> {
        Trace.log("X: " ++ msg); resume(())
      }
      checkpoint(name, resume) -> {
        Trace.log("X: cp: " ++ name); resume(())
      }
      return(x) -> x
    }

`fn main() : Unit / Stdout = with_trace_default() {
delegating_helper(worker) }` produces three correct lines and exits
0. The bug is specifically the **interaction** between a
parameterised handler at one stack level and a self-delegating
handler at a deeper level.

**Hypothesis** (from looking at the emitted C and the in_dispatch
machinery): R2's fix (`in_dispatch_node` lives on the fiber, set
around clause execution) interacts badly with the resume path of a
parameterised handler. When the inner Trace clause calls
`Reader.ask()` (or `State.get()`), the lookup walks past the
in_dispatch'd inner Trace to find Reader. Reader's clause runs
`resume(state)` which one-shots back. But somewhere between the
restored stack and the next `Trace.log("...")` re-entry, either
the in_dispatch_node or the evidence_top has been corrupted —
subsequent ops either see a missing handler (crash) or call into
freed memory (blank string output).

The `examples/effects/m8_12_self_delegating_handler.kai` regression
covers the *same-effect* self-delegation case (works); what it
does NOT cover is **mixing self-delegation with a parameterised
outer handler** where the inner clause reads outer state via a
*different* effect lookup.

**Fix path** (out of scope for this lane):

1. Audit `kai_evidence_lookup_node` and the `in_dispatch_node`
   save/restore around resume entries. Likely the parameterised-
   handler resume path needs to clear/reinstate `in_dispatch_node`
   relative to the *innermost handler that owns the suspended
   continuation*, not the fiber's current top.
2. Add a fixture under `examples/effects/` that minimally
   reproduces the shape (this entry's repro, trimmed).
3. The `m8_12_self_delegating_handler` fixture should grow a
   parameterised-outer sibling so the regression is gated by CI.

**What works today** (the surface where R10 does not bite):

- Parameterised handler outer + non-self-delegating inner
  (`with State[Int](0) { ... }` outer, plain `Stdout.print` inside
  body — fine).
- Self-delegating inner + non-parameterised outer
  (`delegating_helper` above + `with_trace_default` — fine).
- Two parameterised handlers stacked, no self-delegation
  (`State[Int](0)` outer, `Reader[String]("foo")` inner — fine in
  isolation, not directly tested in this lane).

The bug is the *combination*. The matrix needs explicit fixtures
once the runtime fix lands.

**Why this is documented here, not fixed**: the Tier 3 experiment 2
arm B brief is stdlib-shaped (no compiler/runtime changes); the
combination R9 + R10 makes the lane's nominal target
(`with_log_prefix` ship to stdlib) non-viable today. Both findings
are the lane's substantive output; the report in
`docs/lane-experience-exp2-arm-b.md` walks through the full
discovery path.

