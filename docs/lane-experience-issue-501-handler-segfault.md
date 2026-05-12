# Lane experience — issue 501 (handler-no-resume + Nothing op + typed let segfault)

## Objective metrics

- Lane start: 2026-05-11.
- Patch size: stage2/compiler.kai +12 / -3 (one comment + one local
  declaration + one cast at `kai_evidence_push_with_jmp` call site,
  inside `emit_handle`).
- Fixtures: 3 new files in `examples/effects/` plus a new
  `test-issue-501` Makefile target wired into both `.PHONY` and the
  top-level `test` target.
- Diagnosis to fix: ~40 min (longer than the brief's 4–8 hr
  estimate suggested because the bug was *not* in the typer or the
  handle-codegen shape, but in the C language semantics around
  `setjmp`/`longjmp` interacting with `-O2`).
- Selfhost: byte-identical fixed point.

## Reproducer (verbatim from #501, after rewriting `println` to
`print` so the fixture compiles without the prelude wrapper)

```kai
effect Fail {
  fail() : Nothing
}

fn main() : Unit / Stdout {
  let r : Int = handle {
    Fail.fail()
  } with Fail {
    fail(resume) -> 0
  }
  print("r=" ++ int_to_string(r))
}
```

Observed before the fix (C backend, `-O2`): segfault, no diagnostic.
After the fix: `r=0` (stdout default handler appends a newline).

## Bisection — what actually triggers it

The brief enumerated four required ingredients (Nothing op,
no-resume clause, typed `: Int` let, binding used afterward).
Empirical bisection:

1. **`-O0` works.** First clue: the `kai run` driver passed `-g -O0`
   on the bootstrap chain but the user-facing path defaults to
   `-O2`. The `bin/kai run` segfault did not reproduce when the
   same generated C was recompiled with `-g -O0`. This narrowed
   the surface from "compiler emits wrong handle code" to
   "compiler emits C that is fragile to optimisation."
2. **String case prints empty string** (`r=`). Whatever the
   discard slot is reading back is *not* the clause's return
   value — it is reading a stale NULL.
3. **No-let case works.** `let _ = handle { ... }` does not bind
   the result, so the consumer never reads through `r` and the
   stale NULL is harmless.
4. **No-annotation case fails differently** — Show dispatch panic
   rather than segfault. That is a separate typer bug (`r` is
   inferred as `Nothing` instead of unifying with the handler's
   `Int`); not in scope here.

## Root cause

`emit_handle` in `stage2/compiler.kai` lowers a `handle { body } with
Eff { clauses }` to a stmt-expression of roughly this shape:

```c
({
  EvFail _ev = {0};
  …
  KaiEvidence _node;
  jmp_buf _jmp;
  KaiValue *_discard = NULL;
  KaiValue *_body_result;
  if (setjmp(_jmp) == 0) {
    kai_evidence_push_with_jmp(&_node, "Fail", &_ev, &_jmp, &_discard);
    _body_result = ({ … op call … });   // longjmp may fire from here
    kai_evidence_pop();
  } else {
    _body_result = _discard;
  }
  _body_result;
})
```

The op-call site, when the clause does not call `resume`, stores
the clause's return value through the slot the runtime kept:

```c
*_node_op->discard_slot = _op_r;   // aliases &_discard
kai_evidence_pop();
longjmp(*_node_op->handle_jmp, 1); // jumps to the else branch
```

Per **C99 §7.13.2.1/3**, an automatic local that is *changed*
between `setjmp` and `longjmp` and is **not** declared volatile
has indeterminate value after `longjmp`. `_discard` matches
exactly that pattern: declared as automatic in the `setjmp`
function-equivalent, modified through the alias `*discard_slot`
between `setjmp` and `longjmp`, no volatile qualifier. With
`-O2`, clang and gcc both elect to keep the original NULL in a
register across the discard write, so the else-branch reads back
NULL and the surrounding consumer (Show.show, ++ on a String,
etc.) dereferences NULL → SIGSEGV.

`-O0` works only because the optimiser does not perform the
hoist; it is allowed to crash there too — the program is just
lucky.

## Fix

Three lines of edit, localised to `emit_handle`:

1. `KaiValue *_discard = NULL;` →
   `KaiValue * volatile _discard = NULL;`.
2. The push call cast: `&_discard` →
   `(KaiValue **) &_discard` so the qualifier mismatch (the
   runtime field is plain `KaiValue **`) does not warn or break
   `-Wpedantic`. The runtime writes through that slot exactly
   once in straight-line code immediately before `longjmp`, so
   the cast is benign — there is no race or visibility concern
   to lose by stripping the qualifier at the boundary.
3. A 7-line block comment above the `concat_all` documenting
   the C99 invariant and the `-O2` failure mode.

No runtime change. No semantics change. The LLVM backend was not
affected — `llvm_emit_handle` does not implement the discard
branch at all (the clause body's return value flows through
`last_val` directly), so the LLVM path was already producing
correct code for this shape.

## Fixtures

Three positive fixtures in `examples/effects/`:

- `issue_501_handler_no_resume_nothing_op_int_let.kai` — the
  exact reproducer minus the `println` indirection. Expected:
  `r=0`.
- `issue_501_handler_no_resume_string.kai` — String-typed
  discard value. Catches a regression where the cast or
  qualifier handling would break for non-integer clause results.
- `issue_501_handler_no_resume_unused.kai` — handle stmt-expr
  value dropped (no `let`). Confirms the volatile fix does not
  break the no-binding shape.

A new Makefile target `test-issue-501` (in `stage2/Makefile`)
compiles each fixture's emitted C with **`-O2`** explicitly,
since the default stage2 CFLAGS is `-O0` and the bug is invisible
there. Wired into the top-level `test` target so tier1 picks it
up automatically.

The fourth fixture sketched in the brief
(`handler_no_resume_no_annotation`) was dropped: without the
`: Int` annotation the typer infers `r : Nothing` and the
following `print("r=" ++ ...)` consumer fails on Show dispatch,
which is a separate typer bug (out of scope for #501). Filed as
a follow-up note here rather than as a new issue, since the
sigil is "type unification with a Nothing-bodied handle expr"
and that smells like a wider typer area than a single fix.

## Coverage gaps surfaced

- The existing `test-runtime-shadow` and `test-effect-runtime`
  paths all compile their fixtures with the stage2 default
  `-O0`, so a longjmp-volatility regression elsewhere in the
  emitter would have the same blind spot. The new fixture only
  pins `_discard`; if a future emit lane introduces a similar
  setjmp/longjmp-fragile local (a state-machine continuation
  cell, say), an audit would still need to find it manually.
  Considered widening one of the existing -O0 targets to also
  run -O2 for free coverage; deferred — the cost of doubling
  the compile cycle on every effect-runtime fixture exceeded
  the value, and the targeted fixture catches the specific
  shape that broke.
- `bin/kai run` defaults to `-O2`. The bootstrap chain
  (`stage1/kaic1`, `stage2/kaic2`) builds with `-O0`. So the
  *compiler* never crashes on its own handle expressions even
  if it emits broken C — the gap is between what `make selfhost`
  exercises and what end-user programs actually see. Nothing to
  fix here, just documenting the asymmetry.

## Real cost vs estimate

- Brief estimate: 4–8 hours.
- Actual: ~50 min (reproduce + bisect 15 min, root-cause via C99
  invariant 10 min, fix + fixtures + Makefile wiring 15 min, retro
  + selfhost 10 min).
- Why under: the diagnostic path was unusually short because
  the `-O0` vs `-O2` split was the first thing inspecting the
  emitted C surfaced, and the C99 §7.13.2.1 rule pinned the
  fix without requiring runtime instrumentation.

## Follow-ups left

- **Typer-side issue (separate)**: `let r = handle { Fail.fail()
  } with Fail { fail(resume) -> 0 }` infers `r : Nothing`
  instead of unifying with the handler clause's `Int`. The
  fixture for this shape was dropped from #501; would need its
  own issue framing the unification semantics ("does a
  Nothing-bodied handle take its type from the body or from the
  handler clauses?").
- **LLVM emitter discard branch**: `llvm_emit_handle` happens
  to produce the right answer for this shape today, but it does
  not implement the longjmp discard path explicitly. A future
  case (handler that does not resume but the body's continuation
  has side-effects past the op call) would expose the gap.
  Tracked under the m8.x deferred concurrency runtime memo.
