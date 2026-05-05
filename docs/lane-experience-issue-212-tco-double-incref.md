# Lane experience — issue #212 (TCO + Perceus dup double-incref on cons accumulator)

Date: 2026-05-04 (single working session, ~30 min wall).
Lane branch: `issue-212-tco-double-incref`.
Predecessor: PR #247 (closes #243). Filed by issue #209 retro
(`docs/lane-experience-issue-209-perceus-dup.md`) as the upstream
blocker preventing reuse-in-place from firing on TCO-built spines.

## Objective metrics

- Lane start: `2026-05-04T20:16:59-04:00`.
- Lane end:   `2026-05-04T20:29:32-04:00`.
- Wall-clock: ~13 minutes including audit, fix, fixture, all five
  tier gates, and pre/post perf measurement. Build / test
  invocations: 5 logged in `/tmp/lane-issue-212-tco-double-incref-builds.tsv`
  (appended below).

## Diagnosis

The bug is **not** a Perceus-vs-TCO interaction. Both the issue spec
and the audit candidates 1–3 placed the redundant dup either inside
the TCO rewrite or inside Perceus' `pcs_rewrite_expr`. Empirical
inspection of the emitted C for the repro showed a third site:

The structural `kai_incref(...)` wrap inside `emit_list_tail`
(`stage2/compiler.kai:10464`) for the **single-element tail-spread**
case `[ElSpread(x)]`. That wrap is the O(1) tail-spread trick — when
`acc` is last-use, the param's RC is exactly the +1 the cons cell
needs, so an explicit `kai_incref` plus `kai_cons` (which does NOT
incref its args) leaves the spread argument owned by the new cons
cell, balanced. **But** when Perceus has wrapped the read in
`__perceus_dup` (because `acc` is multi-use across branches —
trivially true for `if n==0 { acc } else { build(n-1, [n, ...acc]) }`),
the dup already incref'd the value. The structural `kai_incref` then
double-incref's it.

So the redundant dup is the spread emit's `kai_incref`, which is
correct for last-use spreads but redundant when stacked on top of
Perceus' dup. Neither pass is wrong on its own; their composition is.

## Fix shape

Smallest-delta path: in `emit_list_tail`, the `[ElSpread(x)]` case
now checks whether `x.kind` is `ECall(EVar("__perceus_dup"), [_])`
via a new `is_perceus_dup_call(e: Expr) : Bool` helper. If yes, the
spread is emitted as the lowered `kai_internal_dup(...)` directly
(one incref); if no, the existing `kai_incref(emit_expr(x))` path
fires (one incref, semantics unchanged for last-use spreads).

Net: ~15 lines of `stage2/compiler.kai`, contained in the spread
emit. Neither the TCO rewrite nor `pcs_rewrite_expr` is touched. The
fix is robust to either pass running first because the detection is
purely syntactic on the AST shape Perceus produces.

Code:

```kaikai
fn is_perceus_dup_call(e: Expr) : Bool = match e.kind {
  ECall(callee, _) -> match callee.kind {
    EVar(nm) -> nm == "__perceus_dup"
    _ -> false
  }
  _ -> false
}

# in emit_list_tail, [el] / ElSpread arm:
ElSpread(x) ->
  if is_perceus_dup_call(x) {
    emit_expr(src, x, lcs, fns, variants, lams, cls)
  } else {
    concat_all(["kai_incref(", emit_expr(src, x, lcs, fns, variants, lams, cls), ")"])
  }
```

The pre-fix emit for the repro:

```c
KaiValue *_t1 = kai_cons(
    kai_internal_dup(kai_n),
    kai_incref(kai_internal_dup(kai_acc))   // <-- two increfs
);
```

The post-fix emit:

```c
KaiValue *_t1 = kai_cons(
    kai_internal_dup(kai_n),
    kai_internal_dup(kai_acc)                // <-- one incref
);
```

## Perf measurement

### Repro fixture (`build(50, [])`)

The fixture is a faithful, minimal repro of the bug shape. It
allocates 50 cons cells and sums them.

| metric        | Pre-fix | Post-fix | Delta |
|---|---:|---:|---:|
| `alloc_total` |      99 |       99 |     0 |
| `free_total`  |      50 |       99 | +49   |
| `leaked`      |      49 |        0 | −49   |
| `live_peak`   |      52 |       52 |     0 |

The bug leaked exactly L−1 cons cells per `build(L, [])` call, which
matches the issue spec's "+1 leaked / over-incref'd reference per
step". The fix closes the leak — `free_total` matches `alloc_total`,
no leaked cells.

### `kaic2` self-compile

`./stage2/kaic2 stage2/compiler.kai > /dev/null`, three runs each on
macOS Apple Silicon, `-O0`. The audit projection from the briefing
was −36 % allocs, −5 to −15 % wall.

| metric                     | Pre-fix      | Post-fix     | Delta   |
|---|---:|---:|---:|
| `alloc_total`              | 119,834,961  | 119,881,200  | +0.04 % |
| `cons` allocs              |   6,180,329  |   6,180,046  | −0.005% |
| `reuse_in_place` (firings) |           0  |           0  | (n/a)   |
| Wall (median, s)           |       13.25  |       13.54  | +2.2 %  |

Honest comparison: **the audit projection does not materialise on
self-compile**. Two reasons, consistent with the #209 retro:

1. **Multi-use compiler patterns**. Almost every list inside
   `stage2/compiler.kai` (`Decl`, `Stmt`, accumulator state) is
   consumed by more than one closure or branch, so
   `pcs_rewrite_expr` pre-incref's the read for those uses, and the
   resulting cons spine enters the next match with RC ≥ 2. The dual-
   path emit's unique check correctly falls back to `kai_cons`. The
   self-compile workload is dominated by these multi-use shapes; the
   double-incref bug fixed here was *necessary but not sufficient*
   for the audit's projected gain to land on `kaic2`.

2. **The bug's leak was visible only as RC accounting**. The leaked
   cells are referenced by the live `kai_acc = _t1` rebind chain
   anyway; they are reachable from the eventual root, so the
   pre-fix self-compile didn't heap-leak in the OS sense. What the
   bug did was bump every cons cell's RC by +1 per iteration, which
   is what `kai_check_unique` reads off. So `alloc_total` is
   essentially unchanged; the gain is in `live_peak` *or* in
   `reuse_in_place` firings, neither of which improves until
   downstream multi-use-pattern blockers are cleared.

The wall regression of +2.2 % is within run-to-run noise (3-run
median; the /usr/bin/time grain is 0.01 s and the 3 samples spanned
13.25–13.56 s pre-fix vs 13.48–13.56 s post-fix). The post-fix path
is structurally cheaper (one fewer `kai_incref` call per spread-tail
emit site), so any persistent regression past noise would be
suspicious.

### Reuse-in-place firings

The briefing required `kai_reuse_or_alloc_cons` to fire NON-ZERO
times on `kaic2` self-compile post-fix. **It does not.** `KAI_TRACE_RC`
shows zero `reuse_in_place=N` line both pre- and post-fix. The
upstream blocker is item (1) above: Perceus pre-incref of multi-use
list reads. That is a separate lane.

This lane is correct on its own terms — the empirical `leaked=49 → 0`
on the issue's repro is the load-bearing acceptance criterion, and
selfhost byte-identical on both backends confirms the fix doesn't
change behaviour for any other shape. But the audit's projected
self-compile speedup needs the multi-use-pattern blocker cleared
before it shows up.

## Selfhost byte-identical convergence

Both backends converged in **1 iteration** (the standard kaic2 →
kaic2b → kaic2c roundtrip; kaic2b.c == kaic2c.c byte-for-byte).
This means the fix does not produce a different stage2/compiler.kai
transformation when applied to itself a second time — it is
idempotent at the C-emit level, as expected for a syntactic emit
change.

## Compiler errors I encountered

None. The fix compiled cleanly first try in stage 2 (kaic1 →
kaic2). The new `is_perceus_dup_call` helper is straightforward
nested-match on `ExprKind` / `EVar`, mirroring `pred_regex_pattern_named`
and `extract_regex_literal` (`stage2/compiler.kai:5636-5676`).

## Friction points

- **Was the issue spec right about the dup site?** No — it pointed
  at "TCO accumulator-call lowering" or `pcs_rewrite_expr` as the
  candidate. Both passes do exactly what they should; the redundant
  dup lives in `emit_list_tail` (the spread O(1) trick that predates
  Perceus). This is a third-pass interaction the issue spec did not
  enumerate, and reading the emitted C for the repro before reading
  the passes was the fastest way to localise it.

- **Did the perf projection materialise?** No, for the documented
  reasons above. Reporting honestly per the briefing instructions
  ("if delta is smaller than projected, document why").

- **Is the fix robust to future Perceus shape changes?** Yes within
  the current contract: `pcs_rewrite_expr` produces
  `ECall(EVar("__perceus_dup"), [arg])` exactly. If that contract
  changes (e.g. dup gets passed extra metadata), the
  `is_perceus_dup_call` helper would need updating, but the
  spread-tail emit's correctness invariant is captured in one place.

## Spec ambiguities or interpretive choices

- **Did you use Path 1, 2, or 3 of the briefing?** None of the three
  exactly. The briefing's three candidates were "TCO-aware Perceus
  dup", "Perceus-aware TCO", and "pass ordering". The actual fix is
  a fourth path: "spread-emit-aware composition" — local to neither
  TCO nor Perceus, sitting in the cons-list lowering that both
  passes feed. This is consistent with the briefing's framing
  ("Diagnose first; pick the smallest delta").

- **Did you instrument the runtime?** No. The existing
  `KAI_TRACE_RC` output (alloc_total, free_total, leaked,
  reuse_in_place) was sufficient; no temporary counters needed. The
  briefing's option to "instrument briefly if not present" was
  resolvable to "already present".

## Subjective summary

- **Confidence**: high. The empirical `leaked=49→0` on a 50-cell
  TCO-built spine is unambiguous; both selfhost backends are byte-
  identical at fixed point; tier1 + tier1-asan green, including the
  new fixture's ASAN sibling. The fix is local (~15 lines), purely
  syntactic, and idempotent.
- **Hardest**: resisting the issue spec's framing. The spec named
  TCO and Perceus as the two candidate sites; trusting the C output
  over the prose pointed elsewhere.
- **Easiest**: writing the helper + emit-arm change. The shape was
  mechanically dictated by what `__perceus_dup` lowers to (line
  10002–10011, `kai_internal_dup(arg)`).
- **Compiler help vs hinder**: kaic2's `--dump-typed` / `--dump-mono`
  passes are not needed here; the bug shows up in the textual C
  output of `./stage2/kaic2 repro.kai`, which is grep-searchable
  and unambiguous.

## Limitations of this report

- The audit's projected self-compile gain (−36 % allocs / −5 to
  −15 % wall) does not materialise. The reasons are (1) multi-use
  compiler patterns and (2) the bug's nature being RC-accounting
  rather than allocation-density. Both are expected per the #209
  retro and not surprising; the briefing explicitly required
  honest reporting if delta < projection.

- `reuse_in_place` count post-fix on self-compile is still 0. This
  fix unblocks the *possibility* of reuse on TCO-built spines but
  the actual firing requires either (a) a workload that builds
  truly-unique TCO spines and re-walks them via the recogniser
  arm, or (b) clearing the multi-use-pattern blocker upstream. Both
  are separate lanes.

- The fix does not address #210 (variant + record reuse). That is
  a different shape and out of lane scope.

## Build / test TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T20:22:03-04:00	tier0	OK	45
2026-05-04T20:27:01-04:00	tier1	OK	291
2026-05-04T20:27:59-04:00	tier1-asan	OK	53
2026-05-04T20:28:44-04:00	selfhost	OK	29
2026-05-04T20:29:30-04:00	selfhost-llvm	OK	41
```
