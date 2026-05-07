# Lane experience report — issue #92 stage 1 audit 2026-05-07

Best-effort retrospective by the implementing agent.

## Update — FIX LANDED 2026-05-07 (post-pivot)

After the initial STOP signal below, the brief was updated to pivot
into fix mode. The fix lives in the next commit on this branch as
`fix(perceus): extend tcrec rule 3 with single_use_is_borrow
predicate`. The predicate is conservative — it gates rule 3 firing
to **list-tail-spread** positions only. Match-scrutinee positions
(`match p { ... }`) are deliberately treated as non-borrow because
their leak status depends on perceus's `tcrec_tail_always_sentinel`
arm-shape decision (issue #309): when every tail leaf is a self-
tail-call, perceus injects an early `kai_decref(_scr)` before the
goto, and rule 3's drop becomes a double-decref. List-tail-spread
positions (`[..., ...p]`) are unconditionally safe to drop in the
goto; the cons cell's `kai_incref(p)` leaves the original ref alive
and only the goto-skipped exit-drops would release it.

Acceptance:
- ASAN selfhost cycle clean (was use-after-free in `kai_lex_skip_ws`).
- Selfhost byte-identical at vanilla flags.
- Tier 0 + Tier 1 + Tier 1-ASAN green.
- The `nth_loop` canonical leak fixture (one cons cell per
  iteration) remains under the conservative dropmask. Closing it
  requires per-call-site borrow-vs-consume reasoning that
  considers the enclosing match-arm shape; explicitly out of
  scope for this fix.

The bisection below remains accurate as a record of how the wrong
hypothesis was disproven. The "Recommended next step" section is
superseded by the landed fix.

## Goal

Per the brief: audit stage 1's perceus emit for `[Expr]` walks,
fix the RC imbalance that the diag lane (commit `94db614`) traced
to a heap-use-after-free in `kai_lex_skip_ws`, then re-land the
side-table dropmask (PR #290's `d2a2208`) as the proof gate.

The brief explicitly identified failure mode 1:

> If the ASAN trace points at a locus that is NOT in stage 1's
> perceus emit … STOP and report. The hypothesis (option #1 from
> #92) is wrong; rethink before patching.

## Outcome — STOP signal raised, then fix landed after pivot

Initial diagnosis (below) raised the STOP per the failure mode
clause. The brief was then updated to pivot into fix mode with the
locus + framing already in hand. The pivot section above
documents the landed fix.

## Original outcome — STOP, hypothesis is wrong

The diag retro's bisection was incomplete and its conclusion is
wrong. The bug is **NOT** in stage 1's perceus emit for `[Expr]`
walks. It is in **the rule-3 emit logic itself**: rule 3 plants
an extra `kai_internal_drop(kai_p)` at the goto block of a
self-tail-call when `p` has `LUAt count==1` AND `p` is not in the
recursive args, but the criterion does not account for whether
the single use **consumes** `p` (function-call argument without
`internal_dup`). When the use is consuming, the value is already
gone; the extra drop is a double-decref that surfaces as
heap-use-after-free under ASAN.

## Evidence

### Pin

```
HEAD:                 94db614 (docs(lane-retro): #92 diagnostic 2026-05-07)
VERSION:              0.44.1
Cherry-pick base:     d2a2208 from origin/issue-92-tco-dropmask-side-table
After cherry-pick:    b199155 (resolved 3 Makefile + 1 compiler.kai conflicts)
Date:                 2026-05-07
```

### ASAN abort reproduces identically

```
==44044==ERROR: AddressSanitizer: heap-use-after-free
READ of size 4 at 0x604000000f50 thread T0
    #0 kai_decref runtime.h:1556
    #1 kai_internal_drop runtime.h:1580
    #2 kai_lex_loop kaic2b.c:6239
    #3 kai_tokenize kaic2b.c:6254
    #4 kai_compile_source kaic2b.c:51339
    ...
freed by thread T0 here:
    #0 free
    #1 kai_free_value runtime.h:1548
    #2 kai_decref runtime.h:1560
    #3 kai_internal_drop runtime.h:1580
    #4 kai_lex_skip_ws kaic2b.c:5821
    ...
previously allocated by thread T0 here:
    #0 calloc
    #1 kai_alloc runtime.h:862
    #2 kai_record runtime.h:1753
    #3 kai_lex_new kaic2b.c:5703
    ...
```

Same trace as the diag retro reported. Lexer record allocated by
`kai_lex_new`, freed inside `kai_lex_skip_ws`, then read by
`kai_lex_loop` next iteration.

### Locus inspection — rule 3 plants the bad drop

The kaikai source (`stage2/compiler.kai:873`):

```kai
fn lex_loop(l: Lexer, acc: [Token]) : [Token] {
  match lex_next(l) {
    Step(l1, t) -> {
      let acc1 = [t, ...acc]
      match t.kind {
        TkEof -> list_reverse(acc1)
        _ -> lex_loop(l1, acc1)
      }
    }
  }
}
```

Use-analysis on `l`: 1 use (in `lex_next(l)`). Recursive call
args: `(l1, acc1)`. `l` ∉ args → rule 3 fires.

The C emit with rule 3 ON (cherry-pick `b199155`):

```c
static KaiValue *kai_lex_loop(KaiValue *kai_l, KaiValue *kai_acc) {
    _kai_lex_loop_entry:;
    return ({ KaiValue *_scr = kai_lex_next(kai_l);  /* ← consumes kai_l */
        ...
        ({ KaiValue *_t0 = kai_l1;
           KaiValue *_t1 = kai_internal_dup(kai_acc1);
           { KaiValue *_ = kai_internal_drop(kai_l); (void) _; }   /* ← rule 3 drop, double-decref */
           { KaiValue *_ = kai_internal_drop(kai_acc); (void) _; }
           kai_l = _t0; kai_acc = _t1;
           goto _kai_lex_loop_entry; ...
```

`kai_lex_next(kai_l)` is called without `kai_internal_dup`, so it
consumes `kai_l`. The post-call `kai_internal_drop(kai_l)` planted
by rule 3 is a second decref on the same already-freed cell.

The C emit with rule 3 OFF (vanilla `main`, no side table):

```c
        ({ KaiValue *_t0 = kai_l1;
           KaiValue *_t1 = kai_internal_dup(kai_acc1);
           kai_l = _t0; kai_acc = _t1;       /* ← NO drops, balanced */
           goto _kai_lex_loop_entry; ...
```

Vanilla is balanced. Rule 3 introduces the imbalance.

### Decisive bisection (experiment C — diag retro did NOT run this)

The diag retro tested only:
- A: walk `body` + use the resulting `dm_table` → fail.
- B: do not walk `body` (calls removed), `dm_table = []` → pass.

It concluded: "the mere act of walking `body` reintroduces the
heap corruption." That conclusion does not follow from A vs B
alone, because between them BOTH the walk AND the rule-3 emit are
toggled at once.

This lane added the missing experiment:

- **C: walk `body` (call kept), discard the result, pass `[]` to
  rewrite → PASS.**

```kai
let _dm_table_unused = tcrec_collect_dm_table(body, name, arity, params,
                                               pnames, uses, [], [])
let new_body = tcrec_rewrite_body(body, name, arity, c_sym_str,
                                  params, pnames, uses, [], [])
```

(Same edit applied symmetrically in `tcrec_rewrite_pcs_ret_wrap`.)

```sh
$ make selfhost
self-hosting fixed point: OK
```

The walker function `tcrec_collect_dm_table` and its helpers
(`tcrec_kind_has_evar`, `tcrec_args_have_evar`, etc.) all execute
inside kaic2 during the selfhost step. Their `[Expr]` traversals
fire on every body. Yet selfhost is byte-identical when their
output is not threaded into the rewrite. The walk is **not** the
trigger.

### What the trigger actually is

The trigger is: **rule 3 emits an extra drop at the goto block of
a self-tail-call site for a parameter whose single non-recursive
use is in a position that already consumes the value.**

The dropmask criterion in `tcrec_rule3_mask`
(`stage2/compiler.kai:34445`):

```kai
LUAt(_, _, _) ->
  if pcs_count_non_lam_uses(p.pname, uses, 0) == 1 {
    not tcrec_args_have_evar(args, p.pname)
  } else { false }
```

does not check what KIND of use the single use is. If it is a
borrow (e.g. `EField(p, _)` or a function call that internally
dups, like `lex_loop` calling `lex_skip_ws(kai_internal_dup(l))`),
the original `p` ref is still live at the goto and the drop is
correct. If it is a consume (`lex_next(p)` without `dup`), the
ref is already gone; the drop is a double-decref.

In `lex_loop`, `lex_next(l)` consumes `l`. In `nth_loop` (the
canonical fixture `examples/tco/list_nth_shape.kai`), the single
use of `xs` is `match xs { … }` — the match scrutinee. The match
desugar emits `kai_decref(_scr)` after the do-while, which
consumes the scrutinee value (which IS `xs` since the match goes
through a borrow). So even the canonical fixture has a consuming
single use.

(Whether match-scrutinee is borrow or consume in stage 2's emit
depends on the desugar shape; the lex_loop case is unambiguous
because it's a direct function call.)

### Why no perceus-emit audit will fix this

The brief's hypothesis, derived from the diag retro, was:

> Stage 1 emits a `kai_decref` on the lexer record inside
> `kai_lex_skip_ws` that it should not emit (or omits a
> corresponding `kai_incref`).

But this lane's experiment C shows stage 1's emit for the walker
functions is **NOT** the trigger. The bug is in the kaikai source
of `stage2/compiler.kai`'s rule 3 logic. Patching stage 1 won't
help; the same flawed rule 3 will re-emit the double-decref via
the patched stage 1.

### Why the bug is suddenly visible on macOS

#324 (closure capture lifecycle, leak rate 27.97% → 1.14%)
tightened RC discipline so that frees that were previously
masked by sloppy dup/drop counts now actually fire. A
double-decref that produced a still-live ref (because something
else was leaking) before #324 now produces a freed ref, exposing
the use-after-free.

This is consistent with the diag retro's hypothesis on visibility,
but the underlying bug is rule 3's logic, not stage 1's emit.

## Implications

### Option #1 from #92 is the wrong target

"Audit stage 1's perceus emit for `[Expr]` walks and fix the
imbalance" was the wrong framing. The walk is not the bug.

### The correct fix target

Rule 3's firing criterion needs an additional condition. It must
fire only when the single use is a **borrowing** use (not a
consume).

Stage 2 already has the use-classification machinery: it knows
whether a call site dups its args or consumes them, because the
emit pass walks every callsite to decide whether to plant
`kai_internal_dup` around each argument. The rule-3 dropmask
should consult that classification.

Sketch (not implemented in this lane):

```kai
fn tcrec_rule3_mask(params: [Param], uses: [Use], args: [Expr],
                    body: Expr, i: Int, acc: Int) : Int {
  match params {
    [] -> acc
    [p, ...rest] -> {
      let fire = match last_use_for(p.pname, uses) {
        LUAt(line, col, _) ->
          if pcs_count_non_lam_uses(p.pname, uses, 0) == 1 {
            if tcrec_args_have_evar(args, p.pname) { false }
            # NEW: rule 3 fires only when the single use BORROWS
            else { tcrec_single_use_is_borrow(body, p.pname) }
          } else { false }
        _ -> false
      }
      ...
    }
  }
}
```

Where `tcrec_single_use_is_borrow` walks `body` to find the
single occurrence of `EVar(p)` and returns true iff its parent is
a borrowing context (e.g. `EField`, function call where the
callee dups, match scrutinee where the desugar dups). Any
position the perceus emit would surround with
`kai_internal_dup(...)` qualifies as borrowing.

This is **structurally different** from the original brief and
needs separate scoping.

### Cost reality

The brief estimated 6–12 h. This lane spent ~1.5 h to reach the
STOP — most of it was reproducing the abort, comparing the
with-vs-without-rule-3 emit, and running experiment C to
invalidate the hypothesis. Saved the 4–10 h that an audit of
stage 1's perceus emit would have spent chasing a bug that isn't
there.

## Lane disposition

- HEAD pin recorded.
- Branch `issue-92-stage1-audit` left at `94db614` (no fix
  attempt landed).
- This retrospective doc is the only commit.
- Cherry-pick of `d2a2208` was reset; the conflict-resolved
  cherry-pick is reproducible from the recipe in this doc.
- No PR opened (acceptance gate not met because no fix).

## Things that did NOT need to happen this lane

- Building stage 1 with ASAN flags. The trace was already in
  the diag retro; rebuilding produced the same locus.
- Reading `pcs_rewrite_expr` in stage 1. It is not the locus.
- Comparing stage 1 vs stage 2's emit for `[Expr]` walks. They
  are nearly identical and equally innocent.
- Running tier1 / tier1-asan. Selfhost is the smaller failing
  signal; tier1 only adds noise at this stage.

## Limitations

- Single agent (Claude Opus 4.7), single session, ~1.5 h.
- The proposed fix sketch (`tcrec_single_use_is_borrow`) is not
  prototyped. It might run into its own complications (e.g. the
  desugar position determines borrow-vs-consume, which is not
  trivially derivable from `Expr` alone).
- Did not check whether other rule-3 firings in the codebase
  (e.g. inside `nth_loop`) are ALSO double-decrefs or if the
  match-scrutinee desugar happens to dup. The lex_loop case is
  unambiguous; nth_loop deserves its own check.
- Did not run Linux CI. Per acceptance gate, macOS local must
  pass first; it does not, so the gate stops here.

## Recommended next step

Open a follow-up issue (or update #92) with:

1. The correct framing: rule 3's firing criterion is
   under-specified — it fires when the single use is a consume
   as well as when it's a borrow. The drop is correct only when
   the use is a borrow.
2. The proposed fix shape: extend `tcrec_rule3_mask` to call a
   `tcrec_single_use_is_borrow(body, p)` predicate and gate
   firing on it.
3. The new evidence channel: experiment C makes the rule-3 logic
   visible without exercising the walker, which is independent
   of the diag retro's misdirection toward stage 1's emit.
