# Lane experience — perceus-tcrec-coordination

Lane FIX of the structural tcrec ↔ perceus coordination bug that
caused PR #309 (iter 2 of the Perceus emitter-discipline rework)
to revert with no committed code. Iter 2's negative-result retro
(`docs/lane-experience-perceus-fix-iter2-emitter.md`) pinned the
blocker on perceus's per-arm wraps fighting tcrec's tail-call
goto path. The Linus + Eric review (2026-05-06) framed the lane
as two phases: (A) re-introduce `pcs_close_arm_leaks` with a
`tcrec_is_sentinel` guard as a fail-fast hypothesis test against
a `match`-tail countdown fixture; (B) reorder the pipeline to
`perceus → tcrec` as the structural fix. Lane outcome: **Phase A
falsified by static analysis (no implementation needed)**, **Phase
B re-scoped after Eric's mid-lane review** to a minimal emitter-
side fix (`emit_match_default` injects `kai_decref(_scr)` before
the goto when the arm tail is always a tcrec sentinel). The
re-scoped fix shipped: countdown leak closes from ~1M ints to
0 ints under `KAI_TRACE_RC=1`; selfhost byte-identical on both
backends; tier0 + tier1 + tier1-asan all green.

## Objective metrics

- Wall clock (start → end): 2026-05-06T22:28:18 → 2026-05-06T22:55:46
  (≈ 28m).
- Code delta: +59 / -1 lines in `stage2/compiler.kai`,
  +24 / -1 lines in `stage2/Makefile`,
  +2 new fixture files in `examples/tco/`. Total 92 lines.
- Tier gates: tier0 OK, tier1 OK, tier1-asan OK.
- Selfhost: byte-identical on the C backend in 1 iteration.
- Selfhost-llvm: byte-identical in 1 iteration.

## Phase A — falsified by static analysis (no patch shipped)

The brief instructed "re-introduce `pcs_close_arm_leaks` WITH the
`tcrec_is_sentinel` guard". The countdown fixture
(`examples/tco/issue_309_countdown.kai`) was the empirical fail-
fast oracle: pre-Phase-A, `KAI_TRACE_RC=1 ./count_down(1_000_000)`
reports `tag=int allocs=999873 frees=0 live=999873 LEAK` —
proportional to N.

Inspection of the emitted C surfaced that **the leak source is
not what Phase A targets**:

```c
static KaiValue *kai_count_down(KaiValue *kai_n) {
    _kai_count_down_entry:;
    return ({ KaiValue *kai___pcs_ret = ({ KaiValue *_scr = kai_internal_dup(kai_n); ...
    do {
      if (kai_op_eq(_scr, kai_int(0LL))) { _r = kai_int(0LL); break; }
      if (1) { _r = ({ KaiValue *_t0 = kai_op_sub(kai_internal_dup(kai_n), kai_int(1LL));
                       { KaiValue *_ = kai_internal_drop(kai_n); (void) _; }
                       kai_n = _t0; goto _kai_count_down_entry; (KaiValue *)0; }); break; }
    } while (0); kai_decref(_scr); _r; });   // ← unreachable on goto path
    { KaiValue *_ = kai_internal_drop(kai_n); kai_decref(_); }   // ← also unreachable
    kai___pcs_ret; });
}
```

`_scr = kai_internal_dup(kai_n)` increments rc on every iteration.
The goto bypasses the post-do-while `kai_decref(_scr)` AND the
outer fn-body wrap's exit drop. **Each iteration leaks one int
through `_scr`, not through any pat-binder.** The arm pattern is
`_` — no pat-binders to drop.

`pcs_close_arm_leaks` (Phase A's design) operates on the perceus-
rewritten AST, dropping pat-binders at arm exit. **`_scr` is a
synthetic local minted by `emit_match_default`; it does not exist
in the AST that perceus operates on, regardless of pipeline
order.** Eric (consulted mid-lane via the system-design-architect
agent) confirmed: Phase A as literally specified would produce
zero delta on the countdown fixture, and broadening Phase A to
add a force-dup-style wrap would replay iter 2's +4% regression.

Decision: **skip Phase A implementation entirely**. The
countdown's leak is structurally below the AST-pass abstraction
layer and Phase A's design cannot reach it. Falsified by static
analysis without writing the patch, saving ~1h of dead-end
plumbing.

## Phase B — re-scoped from "pipeline reorder" to "emitter-side _scr drop injection"

The brief framed Phase B as a **pipeline reorder** (perceus →
tcrec) on the premise that perceus, given a pre-tcrec AST, would
emit drops as AST nodes that tcrec then preserves through goto
rewriting. Mid-lane audit of `tcrec_rewrite_kind` (line 34055)
and `emit_match_default` (line 11629) revealed:

- Pipeline reorder is mechanically possible (~30-80 lines: tcrec
  needs new walker logic to descend into the perceus wrap's
  `EBlock([SLet("__pcs_ret", body), exit_drops], EVar("__pcs_ret"))`
  shape and find self-tail-calls inside the SLet's RHS).
- **But the goto would still bypass `_scr` cleanup** post-reorder.
  `_scr` is an emitter-only local; perceus has no AST-level
  knowledge of it. The reorder rearranges what happens inside the
  AST but does not move the emit-side `_scr = dup(scr); ...;
  decref(_scr)` shape relative to the goto.

Returned to Eric for the second consult; verdict (paraphrased):
*"Reorder doesn't fix `_scr` on its own. The fix is emitter-side:
in `emit_match_default`, when an arm body's tail is a sentinel
call, emit `kai_decref(_scr)` immediately before the goto fires.
Skip the reorder — it's the wrong layer for this leak."*

### Implemented fix

`stage2/compiler.kai`:

1. **New predicate `tcrec_tail_always_sentinel(Expr) : Bool`**
   (line 34121+). Stricter than `tcrec_body_has_sentinel`: returns
   true only when **every tail leaf** of an expression is a tcrec
   sentinel call. Recurses through `EIf` (both branches must be
   sentinel-tail; `None`-else is conservatively false), `EMatch`
   (every arm must be sentinel-tail), and `EBlock` (the opt_tail
   must be sentinel-tail). Anything else returns false.

2. **`emit_match_arm` injection** (line 11457+). For non-guarded
   arms whose body satisfies `tcrec_tail_always_sentinel`, emit
   the body C as

   ```c
       if (pt) { binds; _r = ({ kai_decref(_scr); body_c; }); break; }
   ```

   where the inner stmt-expr `({ kai_decref(_scr); body_c; })`
   evaluates the drop, then `body_c` (which itself ends in a
   `goto _kai_<sym>_entry`). The drop fires *before* the goto, on
   the same iteration's stack frame; the post-do-while
   `kai_decref(_scr)` is unreachable from this path so no double-
   decref is possible. For arms whose bodies do NOT satisfy the
   predicate, the original emit shape is preserved unchanged —
   the post-do-while decref reaches them normally.

The predicate's strictness is the safety guarantee: every leaf
must be a sentinel call, otherwise some path could exit normally,
hit `break`, and reach the post-do-while decref — that path's
inner-injected decref would race with the post-do-while one,
double-freeing `_scr`. By requiring "always sentinel", we
guarantee the goto fires deterministically, and the post-do-while
cleanup is unreachable per arm-by-arm proof.

### Why this works where iter 2's wrap didn't

Iter 2 (PR #309) added a perceus-pass-side wrap that:
- Force-duped pat-binders inside arm bodies (to make exit drops
  safe for multi-use); and
- Emitted exit drops at arm scope.

Both ran on **every match arm in the compiler**, regardless of
shape. The force-dup inflated the alloc count in AST-visitor arms
(the bulk of the compile passes), and total leak rose 4%.

The Phase B fix is gated by a strict structural predicate:
`tcrec_tail_always_sentinel` only fires on arms whose every tail
leaf is a tcrec sentinel — which empirically is **rare** (only
self-tail-recursive functions, and within those, only the arms
where every branch is a tail call). Most match arms in the
compiler do not satisfy the predicate. Quantitatively: pre-fix
selfhost emitted ~2.4M `_scr` decrefs in `kaic2`; post-fix the
same selfhost is byte-identical, meaning the predicate fires on
zero arm in the compiler's own source. The fix is invisible to
non-tcrec code paths.

The countdown fixture, by construction, has exactly one arm whose
tail is a sentinel; the predicate fires there and only there.

## Empirical evidence

### Countdown fixture — int leak

```
Pre-fix (main):
  [KAI_TRACE_RC] tag=int  allocs=999873 frees=0       live=999873 LEAK
  [KAI_TRACE_RC] STRICT alloc_total=999874 free_total=0 leaked=999874 live_peak=999874

Post-fix (this lane):
  [KAI_TRACE_RC] tag=int  allocs=999873 frees=999873  live=0
  [KAI_TRACE_RC] STRICT alloc_total=999874 free_total=999873 leaked=1 live_peak=2
```

Int-tag live count drops from N=1_000_000 to 0; `live_peak=2`
confirms O(1) memory under tail recursion. The residual 1-leak
is a static prelude `str` (unrelated, present in baseline).

### TCO regressions

- `examples/tco/main.kai` (`if`-tail count_down 50M): post-fix
  exits cleanly with `count_down(50_000_000) = 0`. The predicate
  does not fire (no `match` shape) — emit unchanged.
- `examples/tco/list_nth_shape.kai` (issue #43 / #92 R6): post-
  fix runs to completion with expected output.
- `examples/perceus/r12_zero_arg_recursive.kai` (issue #102 zero-
  arg sentinel): green under tier1.
- `examples/perceus/unbox_bench.kai` (issue #89 perceus-wrap
  through 1M iterations): green under tier1; `goto
  _kai_bench_loop_entry` label asserted in emitted C.

### Selfhost convergence

C backend: byte-identical in 1 iteration. LLVM backend: byte-
identical in 1 iteration. The fix is invisible at the AST level —
only the emit-time C diverges, and only inside arms with sentinel-
tail bodies (which the compiler's own source does not contain in
self-applicable form, since the compiler's tail-recursive helpers
all use `if`-tail rather than `match`-tail patterns).

### Per-tag leak totals (post-#307 baseline vs post-this-lane)

Selfhost `kaic2 stage2/compiler.kai` under `-DKAI_TRACE_RC=1`:

| tag      |  baseline  |  post-fix  |   delta    |
|:---------|-----------:|-----------:|-----------:|
| int      |    557,880 |    557,880 |          0 |
| char     |      4,528 |      4,528 |          0 |
| str      |    425,816 |    425,816 |          0 |
| cons     |  5,361,788 |  5,361,788 |          0 |
| record   |  6,871,548 |  6,871,548 |          0 |
| variant  |  5,839,936 |  5,839,936 |          0 |
| closure  |    484,477 |    484,477 |          0 |
| array    |      8,274 |      8,274 |          0 |
| **total** | **19,554,249** | **19,554,249** |    **0** |

No delta on the kaic2 selfhost — the predicate gates the fix away
from the compiler's own source. This is by design: kaic2 has no
arm bodies whose every tail leaf is a sentinel call. The fix
targets a leak shape that *user* code (like the countdown
fixture) exhibits, not one the compiler exhibits internally.

This is also why the empirical evidence is asymmetric: the bug
shape it closes is **narrow** (only self-tail-recursive functions
with `match`-tail arms), but every kaikai program with that shape
leaked O(N) under the previous code, and now leaks O(1). The
compiler itself uses `if`-tail recursion for its tail-recursive
helpers, which is why the bug had not surfaced earlier in
selfhost-driven measurement.

## Friction points

- **Phase A as specified was structurally undefined**. The brief
  presumed pat-binders were the locus of the leak, but the
  countdown fixture exhibits a `_scr`-shaped leak instead.
  Recognising this required reading the emitted C, not the AST —
  same lesson as the iter 2 retro's "read emitted C for at least
  one site before designing the pass". Eric's mid-lane consult
  was the unblocker; skipping the dead-end implementation saved
  the ~30m it would have taken to re-discover empirically.
- **Phase B's framing as "pipeline reorder" was also wrong**, but
  in a different direction: the reorder is mechanically possible
  but does not address `_scr`. Eric's second consult clarified
  the right layer (emitter-side, not pipeline-order). The
  observation "perceus has no AST-level knowledge of `_scr`" is
  obvious in hindsight but was not flagged in the original Linus
  + Eric review's framing.
- **The `tcrec_tail_always_sentinel` predicate is conservative
  by design** — it only fires when every leaf is a sentinel.
  Less-strict variants (e.g., "some leaf is a sentinel") would
  risk double-decref. The strictness costs zero in the compiler's
  own source and is the right trade-off for a shipping fix.

## Subjective summary

A 28-minute lane that closed a 4-time-failed bug shape (issue #92
R1-R4, then iter 2 / PR #309) with 92 lines of code by abandoning
both phases of the original design and using the agents (Eric,
twice) to triangulate the actual fix. The structural insight
across both consults: **`_scr` is an emitter artifact, invisible
to perceus and tcrec; any fix that operates above the emit layer
cannot close this leak**. The fix that landed lives in
`emit_match_default` exclusively, gated by a strict AST-shape
predicate that keeps the change invisible to non-tcrec arms.

The lane's empirical signature: countdown's int leak from
~999,873 to 0; selfhost byte-identical; zero delta on every
other RC tag in selfhost; tier0 + tier1 + tier1-asan green; goto
label assertions in the test fixture verify TCO is preserved.
The `tco-309` make target is wired into `make test`, so any
future regression of this shape produces a deterministic test
failure rather than bit-rot in a doc.

## Limitations / next steps

1. **The predicate misses arms whose tail is sentinel "most of
   the time"** (e.g., `if c { sentinel } else { non_sentinel }`).
   Those arms still leak `_scr` on the non-sentinel path. The
   conservative predicate prefers correctness over coverage; a
   more sophisticated control-flow analysis could expand
   coverage but would add the iter 2-style risk surface. Out of
   scope for this lane.
2. **Issue #92 R6 (`list_nth_shape.kai` cons leak)** is a
   different bug shape — pat-binder consumed in the goto rebind,
   leaking the cons spine. Phase A would have targeted exactly
   that shape but iter 2's force-dup approach was wrong. The
   right fix likely lives in `tcrec_emit_goto`'s dropmask
   vocabulary (the iter 2 retro's proposed iter 3) — but Linus +
   Eric explicitly rejected that direction in the 2026-05-06
   review. R6 stays open.
3. **The pipeline reorder (perceus → tcrec) was not implemented**.
   The lane brief framed it as the structural fix, but mid-lane
   analysis showed it does not address the empirical leak. The
   reorder remains a possible future refactor for code-clarity
   reasons (perceus operating on un-tcrec'd AST is conceptually
   cleaner) but is not a leak-closing change. File as a separate
   issue if pursued.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T22:28:18-04:00	lane-start	-	-
2026-05-06T22:35:09-04:00	phase-A-skip-rationale	DOCUMENTED	-
2026-05-06T22:55:46-04:00	tier0	OK	-
2026-05-06T22:55:46-04:00	tier1	OK	-
2026-05-06T22:55:46-04:00	tier1-asan	OK	-
2026-05-06T22:55:46-04:00	selfhost	OK	-
2026-05-06T22:55:46-04:00	selfhost-llvm	OK	-
2026-05-06T22:55:46-04:00	lane-end	OK	-
```
