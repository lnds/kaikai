# Lane experience — perceus-fix-iter2-emitter

Lane FIX iteration 2 of the Perceus emitter-discipline rework. Iter 1
(PR #307) closed DIAG site #1 (`prelude_table` str leak, -78%) via
runtime short-string interning. Iter 2's brief targets sites 2-4, which
the iter 1 retro flagged as needing **emitter-pass changes** rather
than runtime fixes:

- Site 2 — `list_has` cons leak (Pattern C, branch-asymmetric pat-binder
  drop).
- Site 3 — `mk_expr` record leak (Pattern B, caller-side AST-builder
  discipline).
- Site 4 — `map_expr_kind` record leak (Pattern B, same as site 3).

This lane attempted sub-iteration 1 against Pattern C (the largest
remaining shape, the `list_has`-style branch-asymmetric leak). The
patch under-performed: instead of meeting the per-batch invariant
(≥ 20% absolute drop on the targeted tag), total leak count went
**up** by 4%, with no tag dropping by the threshold. The patch was
reverted per the lane brief's STOP rule. No emitter-pass change
shipped. The headline outcome is **a documented negative result
plus a precise structural finding** that reframes what iter 3 (or
beyond) needs to do.

## Objective metrics

- Wall clock (start → end): 2026-05-06T21:19:02 → 2026-05-06T21:48:12
  (≈ 29m).
- Code delta: 0 lines committed (sub-iteration 1 patch reverted in
  full after invariant failure).
- Tier gates: not run — patch reverted before promotion.
- Selfhost byte-identical: verified clean main is still byte-identical
  on the C backend after revert.

## DIAG re-run baseline (post-#307)

Built `kaic2` with `-DKAI_TRACE_RC=1 -DKAI_TRACE_RC_LEAKSITE=1`,
ran `KAI_TRACE_RC=1 KAI_TRACE_RC_LEAKSITE_TOP=20 ./kaic2 compiler.kai`.

Per-tag totals (baseline = post-#307):

| tag      |  baseline  |
|:---------|-----------:|
| int      |    557,880 |
| char     |      4,528 |
| str      |    425,816 |
| cons     |  5,361,788 |
| record   |  6,871,548 |
| variant  |  5,839,936 |
| closure  |    484,477 |
| array    |      8,274 |
| **total** | **19,554,249** |

Top-3 (post-#307, str interning already in place):

| Rank | scope_fn      | tag         | leak       | leak% |
|-----:|:--------------|:------------|-----------:|------:|
|  1   | list_has      | cons        |  1,267,985 |  95.5 |
|  2   | mk_expr       | record      |  1,248,010 |  96.9 |
|  3   | map_expr_kind | record      |  1,176,966 | 100.0 |

Confirms iter 1 retro's ranking. The post-#307 ordering is identical to
the pre-#307 ordering at sites 2-4 because str interning didn't touch
record / cons paths.

## Pattern classification of top-3

Read the kaikai source for each fn and the emitter's generated C output
(`./kaic2 stage2/compiler.kai > /tmp/kaic2-emit.c`). Findings:

### Site 1 — `list_has` cons (Pattern C, branch-asymmetric)

`stage2/compiler.kai:9132`:

```kaikai
fn list_has(names, target) : Bool {
  match names {
    [] -> false
    [n, ...rest] -> if n == target { true } else { list_has(rest, target) }
  }
}
```

Emitted C (vanilla kaic2, line 13728 of /tmp/kaic2-emit.c):

```c
if ((_scr && _scr->tag == KAI_CONS && 1 && 1)) {
  KaiValue *kai_n    = kai_incref(_scr->as.cons.head);
  KaiValue *kai_rest = kai_incref(_scr->as.cons.tail);
  _r = ({
    KaiValue *_c = kai_op_eq_v(kai_n, kai_internal_dup(kai_target));
    KaiValue *_r = kai_op_truthy(_c)
      ? kai_bool(1)                     // path A: leaks kai_rest
      : ({ KaiValue *_t0 = kai_rest;    // path B: transfers kai_rest
           ... goto _kai_list_has_entry; ... });
    kai_decref(_c); _r;
  });
  break;
}
```

`kai_n` is consumed by `kai_op_eq_v` (which decrefs both args, see
`stage0/runtime.h:2746`). `kai_rest` is transferred only on path B
(tail-call rebind). On path A (`n == target`), `kai_rest` stays
incref'd and the match-arm exit's `kai_decref(_scr)` reclaims the
spine — but the standalone `kai_rest` ref is never released.

This is exactly Pattern C: the pat-binder is consumed in one
branch (tail-call) but leaks in the other (success).

### Site 2 — `mk_expr` record (Pattern B, caller discipline)

`stage2/compiler.kai:1585`:

```kaikai
fn mk_expr(k, line, col) : Expr =
  Expr { kind: k, line: line, col: col, ty: None, mode: MUnknown }
```

The fn is a plain record allocator. The DIAG attribution
(`scope_fn = mk_expr`) means the **allocation** happened inside this
function's call frame; the leak is wherever the caller discards the
result. Auditing `mk_expr`'s callers would close this — same shape
as the `ok_e` site (rank 5/6) and likely overlaps `map_expr_kind`'s
leak too (record allocation in `ECall(callee, args_)` etc.).

### Site 3 — `map_expr_kind` record (Pattern B, same as site 2)

`stage2/compiler.kai:15857`. Visitor that allocates fresh `ExprKind`
nodes for every composite case (`ECall`, `EField`, `EBinop`, …). Each
call returns a fresh record; callers (parser, desugar) discard the
result without binding it to a let-name in some hot paths.

## Sub-iteration 1 — pat-binder force-dup + arm-exit drop wrap

Targeted Pattern C (site 1, `list_has`). The patch added a new
`pcs_close_arm_leaks` pass to `pcs_rewrite_arms` that:

1. After the existing `pcs_rewrite_expr` pass, walked the arm body's
   tail-position terminal.
2. If the terminal was a tcrec sentinel call, left it alone (tcrec's
   own dropmask handles fn-params, and the goto bypasses any wrap).
3. Otherwise wrapped the terminal as
   `EBlock([SLet(__pcs_arm_ret, force_dup(terminal)), drops…], EVar(__pcs_arm_ret))`,
   where `force_dup` walks the terminal's sub-tree and wraps every
   `EVar(pat_binder)` with `__perceus_dup`. The exit drops `kai_<name>`
   for each pat-binder.
4. Filtered out `_`-prefixed unused-by-convention pat-binders so we
   didn't synthesise drops for nonexistent C locals.
5. Skipped guarded arms.

Code: ~165 lines in `stage2/compiler.kai`, all inside the perceus
pass; no emitter change required, because `__perceus_dup` /
`__perceus_drop` already lower correctly in `emit_call_expr`.

### Expected behavior

- Path A (`true → kai_bool(1)` in list_has): exit drops fire, `kai_rest`
  released, **leak closed**.
- Path B (tail-call): goto bypasses exit drops; force-dup is also
  suppressed inside sentinel calls, so kai_rest is transferred raw —
  unchanged from baseline.

### Actual result — invariant FAILED

| tag      |  baseline  |  iter 1   |  delta    |
|:---------|-----------:|----------:|----------:|
| int      |    557,880 |   559,502 |    +1,622 |
| char     |      4,528 |     4,546 |       +18 |
| str      |    425,816 |   448,860 |   +23,044 |
| cons     |  5,361,788 | 5,608,619 |  +246,831 |
| record   |  6,871,548 | 7,139,841 |  +268,293 |
| variant  |  5,839,936 | 6,048,199 |  +208,263 |
| closure  |    484,477 |   537,806 |   +53,329 |
| **total** | **19,554,249** | **20,355,673** | **+801,424** |

**Total leak rose by 4.1%.** No tag dropped — every tag rose. The
target site `list_has` cons stayed effectively flat (1,267,985 →
1,283,539, +1.2%).

### Why it under-performed — the structural finding

The patch's failure has a clean root cause that reframes what iter 3
(or beyond) needs to do:

The `force_dup` step adds `__perceus_dup(rest)` reads at every use of
a pat-binder in the wrapped subtree. For `list_has`:

- Path A (`true → kai_bool(1)`): no pat-binder reads in the subtree,
  so force_dup is a no-op. Exit drop fires, releases kai_rest. ✓
- Path B (tail-call recursive): the recursive call's args reach
  `kai_rest`, but force_dup explicitly skips sentinel-call args (per
  `tcrec_is_sentinel(callee)` guard) precisely so the goto's transfer
  semantics aren't disturbed. So path B is unchanged. ✓

But for **arms whose body itself wraps a sub-call that mutates the
pat-binder structure** (the bulk of compile-pass arm bodies, like
`ECall(callee, args_) -> ECall(f(callee), map(args_, f))`), the wrap
puts `__perceus_dup(callee)` and `__perceus_dup(args_)` everywhere.
Those dups bump the alloc count for every visit, while the original
pat-binder is also exit-dropped — net zero on leak, but +1 alloc
per visit.

Compounding: this fires on **every match arm in the entire
compiler**, not just `list_has`-shaped arms. The alloc/incref
overhead of the wrap is paid by every traversal of every AST node in
every compile pass. The leak counts rose because the additional dups
themselves leak in arms whose body does discard their result without
a clear consumption path — a long tail of edge cases the wrap
machinery cannot disambiguate without per-arm-body flow analysis.

In other words: the patch's regression isn't in `list_has` (the
target site nudged sideways, not down); it's in **every other arm
in the compiler** where the conservative force-dup added an extra
dup that wasn't balanced cleanly by the exit drop. Closing site 1
without breaking sites 2-N requires per-arm flow knowledge the
single-pass approximation doesn't have.

The reverted patch ran selfhost and converged byte-identical in 1
iteration on the C backend — convergence wasn't the failure mode.
The failure was empirical leak measurement.

## Sub-iteration 2 — not attempted

Per the lane brief: *"If after sub-iteration 1 the invariant fails
(≥20% absolute drop NOT achieved), STOP and report. Don't escalate
to a bigger patch hoping for cumulative effect — that's the Phase
1+2 failure mode."* The patch under-performed; STOPped per brief.

## Selfhost convergence iterations needed

The reverted (clean) tree is byte-identical on the C backend in 1
iteration:

```
$ make -C stage2 selfhost
self-hosting fixed point: OK
```

The sub-iteration 1 patch also converged byte-identical in 1 iteration
on the C backend during testing — convergence wasn't a blocker; the
empirical leak measurement was.

## Empirical evidence — top-10 before / after sub-iter 1 (then reverted)

| Rank | scope_fn         | tag       | baseline  | iter 1 (patch) |
|-----:|:-----------------|:----------|----------:|---------------:|
|  1   | list_has         | cons      | 1,267,985 |      1,283,539 |
|  2   | mk_expr          | record    | 1,248,010 |      1,251,133 |
|  3   | map_expr_kind    | record    | 1,176,966 |      1,306,450 |
|  4   | ok_e             | variant   |   808,514 |        810,513 |
|  5   | ok_e             | record    |   776,418 |        778,324 |
|  6   | map_expr_kind    | variant   |   714,546 |        814,224 |
|  7   | tok              | cons      |   694,601 |        696,545 |
|  8   | map_expr_kind    | cons      |   649,175 |        725,589 |
|  9   | prelude_table    | variant   |   640,440 |        603,945 |
| 10   | extract_…tycon   | variant   |   611,177 |        611,538 |

The negative deltas appear in every site that sees match-arm AST-
visitor traversal — exactly the long tail the wrap surcharges.

After revert: per-tag totals match the baseline by construction
(the working tree is identical to clean main's stage2/compiler.kai).

## Friction points

- The lane brief presumed iter 1's runtime-cache approach
  generalised to a stand-alone "force-dup + arm exit drop" perceus
  pass. The structural finding is that **pat-binders interact with
  tcrec's sentinel/goto path in a way that any per-arm wrap must
  reason about**: tcrec runs before perceus, plants sentinels for
  self-tail-calls, and emits a `dropmask`-driven goto block. The
  goto skips out of the enclosing statement-expression, so any
  wrap an arm's perceus pass adds is bypassed on tail-call paths.
  Iter 3 needs either: (a) merge perceus's arm-exit drops into
  tcrec's dropmask vocabulary (so the goto path also drops
  pat-binders); or (b) hoist pat-binders to fn-param scope so
  `pcs_collect_exit_drops` already covers them; or (c) per-branch
  flow analysis that places drops at each path's leaf without a
  blanket wrap.
- The mid-investigation realization that `kai_op_eq_v` consumes both
  inputs (`stage0/runtime.h:2746`) shifted the diagnosis: the
  `kai_n` pat-binder of `list_has` is NOT leaked; only `kai_rest`
  is. Mis-reading this would have led to a more aggressive (and
  even more counter-productive) patch.
- Time spent in static analysis (~1h) before the first patch was
  excessive. Iter 1 closed in 28m end-to-end; this lane spent ~30m
  in analysis before realising the fix needed to coordinate with
  tcrec, and still landed on a wrap that didn't account for AST-
  visitor arms. Lesson for iter 3: read the emitted C for at least
  one site before designing the pass.

## Subjective summary

A negative result that pinned down the structural blocker more
precisely than the lane brief's framing did. The Pattern A/B/C
taxonomy in the brief is a useful first-cut, but it understates
the coupling between perceus's per-arm drops and tcrec's tail-call
goto. Without a coordination strategy between those two passes,
**any** isolated perceus extension that adds drops will fight the
goto path.

The right next move (if the project pursues this further) is to
extend `tcrec_emit_goto`'s dropmask vocabulary to cover pat-binders
in the arm-scope at the call site. That's an extension that fits
within `tcrec_rewrite_decl`'s existing scope tracker (it already
walks `pat_bindings` for tail-position correctness, see line 33863
+ 33876 in stage2/compiler.kai). Sub-iter 2 of this lane would
have attempted that, but the per-batch invariant gate from sub-
iter 1 prohibited escalation.

## Limitations / next lane scope

1. **tcrec ↔ perceus pat-binder coordination** is the structural
   blocker for sites 2-4 of the original DIAG. Iter 3 should
   teach `tcrec_emit_goto` to drop arm-scope pat-binders before
   the goto, in addition to the existing fn-param dropmask.
   Estimated 100-200 lines in `tcrec_rewrite_decl` plus the goto
   emit; selfhost-byte-identical risk modest because changes are
   localised to one pass.

2. **Pattern B (caller-side discipline at `mk_expr` /
   `map_expr_kind`)** wasn't reached. Likely fits a different
   pass entirely — auditing record-allocator caller paths for
   missing exit drops in let-bindings whose RHS is a fresh
   record. Stage1 + stage2 both ship with what's effectively a
   "no exit drop on raw record let-bindings" rule.

3. **Reuse-in-place predicate** (issue #210) was off-limits per
   the brief and remains unaddressed.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T21:19:02-04:00	lane-start	-	-
2026-05-06T21:33:31-04:00	DIAG-baseline	OK	-
2026-05-06T21:48:12-04:00	sub-iter-1-patch	FAIL_INVARIANT	-
2026-05-06T21:48:12-04:00	revert	OK	-
2026-05-06T21:48:12-04:00	lane-end	REPORTED_STOP	-
```
