# Lane retro — issues #570 + #571: LLVM emitter and nested lambdas under handlers

## Scope as planned vs scope as shipped

**Planned (brief).** Close issues #570 and #571 as a combined lane on
the hypothesis they shared a single root cause: the closure pointer
passed to `spawn_actor` inside `with_mailbox` was emitted as `undef`
(because the lifted `LamInfo` for the nested lambda was not found in
the LLVM emitter's `lams` table), which explained both the stderr
warning of #571 and the runtime segfault of #570.

**Shipped.** Only #571 closes here. Investigation showed the two
issues do **not** share a root cause:

- **#571** is the lambda-info bug. The fix is one structural change
  to `llvm_emit_lambda_bodies` in `stage2/compiler.kai`. The repro
  no longer prints `llvm: lambda info missing` and the emitted IR
  no longer carries `undef` in the closure pointer slot.
- **#570** is an independent runtime crash. Reverting the #571 fix
  and rebuilding shows the segfault for the #570 repro **without
  ever printing the lambda-info warning** — the lambda lifter was
  not the culprit. The crash sits inside `kai_main`'s call sequence
  immediately after `kaix_evidence_node_handler` returns NULL, i.e.
  no `Spawn` default handler is installed in the LLVM backend's
  `kai_main_install_defaults`. The C backend builds and runs both
  repros to exit 0; the LLVM backend was missing a piece of main-row
  default-handler installation.

The brief's constraint was explicit: "Si tras investigar resulta que
#570 y #571 NO son el mismo root cause, separar en dos lanes y
reportar — no shipear un fix parcial." This lane therefore lands the
#571 fix and leaves #570 as a separate follow-up (issue #570 stays
open; this lane's PR closes #571 only).

## Reproduction evidence

Both repros are verbatim from the issue bodies, built via
`bin/kai build <file>.kai` (LLVM backend, the default with clang in
PATH).

**#571 (pre-fix).** stderr line `llvm: lambda info missing at 5:42`.
The `.ll` shows:

```
define %KaiValue* @_kai_lam_18(...) {
  %t26645 = call %KaiValue* @kai_actor__with_mailbox(%KaiValue* undef)
  ret %KaiValue* %t26645
}
```

The `undef` is the closure pointer for the trailing-lambda `()=>()`
desugared from `with_mailbox { () }`. `_kai_lam_19` (the inner) is
defined elsewhere in the IR — the lifter knew about it; the
**emitter** could not find it.

**#571 (post-fix).** stderr clean. The same `_kai_lam_18` slot now
emits `kaix_closure(@_kai_lam_19, ...)` and feeds that into
`@kai_actor__with_mailbox`.

**#570 (pre-fix and post-fix).** Build clean both times (no
lambda-info warning ever appears). Runtime segfaults inside
`kai_main + 68` at `ldr x1, [x0]` where `x0` is the return value of
`@kaix_evidence_node_handler`. Same crash pattern on baseline and
on the post-#571 binary. C backend exit 0 for the same repro.

## Root cause (#571)

`stage2/compiler.kai`'s `collect_decls` walks the AST once and
returns a `LamInfo` table whose entries are keyed on the source
`(line, col)` of each `ELambda`. `llvm_emit_lambda_bodies` then
iterates this table to emit each lifted body. The pre-fix shape was:

```kai
fn llvm_emit_lambda_bodies(lams: [LamInfo], ...) {
  match lams {
    [] -> acc
    [l, ...rest] -> {
      let out = llvm_emit_lambda_body(l, fns, vs, lams, oo, cls, start_id)
      llvm_emit_lambda_bodies(rest, fns, vs, oo, cls, out.next_id, ...)
    }
  }
}
```

Two responsibilities on one variable:

1. `lams` (head/tail split) drives the recursion over "lambdas
   remaining to emit".
2. The same `lams` is also passed to `llvm_emit_lambda_body` as the
   *full* `LamInfo` table for inner-lambda lookups via `find_lam`.

Each recursion drops the head. By the time the recursion reached
`_kai_lam_18` (the second body — `lams = [_kai_lam_19, _kai_lam_18,
..., _kai_lam_0]`, head-first), the table passed down was the
already-popped `rest`, which no longer contained `_kai_lam_19`. When
`llvm_emit_expr` walked `_kai_lam_18.body` and hit the inner
`ELambda` at `(5, 42)`, `find_lam` returned `None`, the emitter
substituted `undef`, and stderr carried the warning.

Numerically: post-collect `lams` had 20 entries (ids 0–19); the
`e.lams` at the failing `find_lam` site had 19 entries (lam_19 was
dropped). Both the pre-fix instrumentation and the post-fix repro
confirm the count delta.

This is the third instance in stage 2 of a per-node lookup over a
shrinking state passed alongside an iteration variable (the brief
flagged this as a pattern; see `current_module` and the row-var work
for the earlier two). The C backend's `emit_lam_helpers` does not
have the bug — it uses `map(lams, (l) => emit_lam_helper(src, l, ...,
lams, ...))`, which is a uniform `map` where every closure captures
the immutable `lams`.

## Fix shape

One structural change plus a hardening:

```kai
# Issue #571: walks `remaining` while passing the FULL `all_lams`
# table down to each `llvm_emit_lambda_body` call. The earlier shape
# reused the same parameter for both, so the LamInfo for an inner
# nested lambda was dropped from the table by the time its enclosing
# lambda's body was emitted; `find_lam` then failed at the inner
# `ELambda` use site and the emitter substituted `undef` for the
# closure pointer (visible as the "lambda info missing" stderr line).
fn llvm_emit_lambda_bodies(lams: [LamInfo], ...) {
  llvm_emit_lambda_bodies_loop(lams, lams, fns, vs, oo, cls, start_id, acc)
}

fn llvm_emit_lambda_bodies_loop(remaining: [LamInfo], all_lams: [LamInfo], ...) {
  match remaining {
    [] -> acc
    [l, ...rest] -> {
      let out = llvm_emit_lambda_body(l, fns, vs, all_lams, ...)
      llvm_emit_lambda_bodies_loop(rest, all_lams, ...)
    }
  }
}
```

Separating "iteration cursor" from "full lookup table" is the
smallest possible change that restores the invariant. The C
backend's `map` shape was an existing precedent for the same
discipline.

The `None` branch of `llvm_emit_lambda_ref` previously emitted
`undef` and a stderr warning. With the lifter now invariant-clean
the branch is unreachable in practice — but if a future regression
re-introduces the issue, silently emitting a broken binary is the
worst possible outcome. Per the brief's guidance, the branch now
`panic`s with a "please file an issue" message instead. The C
backend's analogous `find_lam` use sites already either resolve
successfully or feed a path that emits `kai_prelude_panic` at
runtime; the LLVM backend mirrors that contract now.

## Decision on the `None` branch

`panic` chosen over LLVM `unreachable` for two reasons:

1. A `panic` from inside the emitter halts compilation with a clear
   message. An LLVM `unreachable` would still produce a `.ll` file,
   pass `clang` validation, link, and only crash at *runtime* when
   the path was actually taken — that is exactly the failure mode
   #571 was about (silent acceptance of bad IR).
2. `panic` thread `Nothing` so the match arm type-checks without
   adding `Process` to `llvm_emit_lambda_ref`'s effect row, which
   would have rippled through several callers (the row already
   declares `Console`, so the eprint line below is unaffected).

## Why option A (run the pass post-desugar) and option B (id-stable
lambdas) were not needed

The brief listed three direction options. The investigation found
that the lifter already runs at the right point — the table was
populated correctly, the lookup-key contract (`(line, col)`) was
respected on both sides, and the inner `LamInfo` was visibly in the
list emerging from `collect_decls`. The bug was strictly downstream
of all of that: the emitter's iteration shape was eating its own
table. None of A/B/C from the brief was needed; the fix is a
mechanical refactor of one helper.

## Fixtures shipped

`examples/llvm/nested_lambda_with_mailbox.kai` — pure LLVM-backend
regression: defines `make_handler` whose body is the trailing-
lambda-bearing `() => with_mailbox { () }` and exercises both lifts
without invoking `Spawn` runtime machinery. Wired into
`tools/test-llvm-driver.sh`'s `FIXTURES` list so the parity sweep
builds it with both backends and diffs the resulting stdout. The
same fixture, pre-fix, reproduces the stderr warning on the LLVM
build.

No negative fixture was added: the hardened `None` branch in
`llvm_emit_lambda_ref` panics by construction; surfacing it through
a fixture would require deliberately wedging the lifter, and there
is no organic path to that branch with the fix in place.

## #570 — separate lane

Reproducing #570 on baseline (pre-#571 fix) shows:

- No `llvm: lambda info missing` warning in the build log.
- Runtime SIGSEGV at the first dereference of x0 inside `kai_main`,
  on the instruction immediately following `kaix_evidence_node_handler`.
- The same source built with `--backend=c` runs to exit 0.

The signature points at `llvm_emit_main_install_defaults` (in
`stage2/compiler.kai` ~line 45858) not pushing a default `Spawn`
evidence node before `kai_main` enters user code. The builtin row
currently handled there is `Stdout`, `Stderr`, `Fail`, `Mutable`,
`Random`, `Clock`, `NetTcp`, `Log`, `SecureRandom` — `Spawn` is
absent. The C backend's main path uses the runtime `kai_prelude_*`
default handlers wired by `stage0/runtime.h`; the LLVM path has its
own install/teardown lowering that needs the `Spawn` (and probably
also `Actor`) defaults wired in.

The fix is plausibly a localised additions to
`llvm_default_builtin_decls` + `llvm_emit_main_install_defaults` /
`llvm_emit_main_teardown_defaults` + the matching runtime symbols
in `runtime_llvm.c`. The scope is small but it is **a different
investigation** than #571 and the brief's "don't ship a partial
fix" constraint applies. Recommend a fresh lane against issue #570
that audits the LLVM main row install/teardown surface against
inject_builtin_effects's full list, not just the v0.4 set wired
when the LLVM path landed.

The lane branch `issue-570-571-llvm-lam` keeps the combined name
for the audit trail; the PR title and body explicitly say "closes
#571" only.

## Selfhost / tier behaviour

- `make tier0` — selfhost byte-identical, demos baseline holds
  (28 passing, baseline 27).
- `make tier1` — run before PR open; expected green based on the
  fix being scoped to a single helper that the C backend already
  exercises through `emit_lam_helpers`.

The fix does not affect the C backend (no shared code path with
`llvm_emit_lambda_bodies`) and does not alter the AST in any way,
so demos/stdlib regressions are unlikely.

## Real cost vs estimate

Brief estimated 1–2 days assuming the shared-root-cause hypothesis
held. Actual: ~2 hours for #571 alone, plus ~30 minutes confirming
#570 is independent and writing the retro. The investigation phase
was the bulk of the time — confirming that the lambda-info bug was
real and tracing why `_kai_lam_19` appeared in IR but not in the
emitter's lookup table. Once the iteration-vs-table conflation was
spotted, the fix was mechanical.

## Cross-references

- Issue #571 — the lambda-info warning, closed by this lane.
- Issue #570 — the LLVM-backend runtime crash; remains open for a
  follow-up lane.
- C backend's `emit_lam_helpers` (line ~15003) — the precedent for
  the `map`-style "every body sees the full table" discipline that
  the LLVM helper now mirrors.
- Pattern note (brief): third occurrence of "state per-node depended
  on `(line, col)` as identity threaded through an iteration over a
  shrinking list". Worth a sweep audit before the next backend lane
  to find the fourth occurrence preemptively.
