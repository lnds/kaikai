# Lane experience — issue #599 branch-aware dup elimination

**Status:** PARTIALLY LANDED — infrastructure in place, optimisation
stubbed pending a linear-consumer annotation pass.

**Dates:**
- 2026-05-20: first attempt in worktree `perceus-599`. Aborted per
  STOP rule. No code shipped.
- 2026-05-21: second attempt in-tree. Infrastructure landed,
  optimisation stubbed (selfhost byte-identical preserved).

## Outcome (second attempt)

- Selfhost byte-identical: ✅ (with the optimisation stubbed).
- Tier 0 green: ✅.
- Fixture lands: ✅ — `examples/perceus/branch_aware_dup.kai`.
- Implementation working in isolation: ✅ — fixture goes from 3 dups
  of `kai_s` (one per match arm) to 0 with the analysis active.
- Bootstrap fixed-point with optimisation active: ❌ — kaic2_v2
  SIGBUS in `mfm_alloc` on stage 2 self-compile.

## What landed in main

1. **The fixture** (`examples/perceus/branch_aware_dup.kai` +
   `.out.expected`): canonical bug shape. `fn pick(s: String, t: Tag)`
   with `match t { A -> use(s); B -> use(s); C -> use(s) }` — each
   arm reads the param exactly once.

2. **`pcs_max_paths_*` family** (16 functions, ~120 LOC): branch-
   aware max-non-lam-reads counter. Sequential composition is sum
   (Block / Call / Binop / ...), `EMatch` arms and `EIf` branches
   take the max over alternatives.

3. **`pcs_consumed_on_every_path` family** (10 functions, ~110 LOC):
   guarantees that every execution path through the body actually
   consumes the binding (so missing-else / missing-arm scenarios
   don't leak). `EIf` without else cannot prove consume; `EMatch`
   requires all arms; `ELambda` cannot prove (closure capture is
   RC'd separately).

4. **`pcs_branch_aware_skip_params(pnames, body) : [String]`**:
   computes the set of fn params whose max-path ≤ 1 AND consumed on
   every path. **Returns `[]` in the shipped version** — the
   optimisation is stubbed. Activating requires changing one line
   (see *Activation toggle* below) plus a third condition this lane
   could not implement (see *Root cause of the bootstrap crash*).

5. **`skip_set: [String]` parameter** threaded through all 15
   `pcs_rewrite_*` functions and `pcs_is_non_last`. When skip_set
   is non-empty (the activation path), `pcs_is_non_last` returns
   `false` for matching names, skipping the dup wrap.

6. **`pcs_collect_exit_drops` extended** to accept skip_set and
   skip the exit drop for matching params. Required to balance the
   skipped dup-wraps; semantically neutral when skip_set is empty
   (verified by selfhost byte-identical).

## What does NOT land

The optimisation itself. With `pcs_branch_aware_skip_params`
returning a non-empty set, kaic2 emits less-dup'd code that runs the
fixture correctly but SIGBUS's on its own bootstrap. Root cause
below; the fix requires a separate analysis we ran out of budget
to land.

## Root cause of the bootstrap crash

The skip set is sound on two axes (max-path ≤ 1 AND consumed on
every path) but is missing a THIRD condition: the consumer must be
**linear** — i.e. it must consume the ref it receives without
re-increfing internally. Many stdlib helpers and runtime primitives
silently incref their arguments (e.g. `kai_op_field` makes a borrow
that does its own RC bookkeeping; `kai_record` may incref slots
internally; some `kai_prelude_*` helpers retain their args).

Pre-fix, the dup-wrap discipline made every read into
`incref-then-consume`: the consumer always got its own ref, and the
exit drop released the original. With the wrap skipped, the original
ref reaches the consumer raw. If the consumer is linear it transfers
ownership and the count is balanced. If the consumer internally
increfs, the original ref's RC drifts and the binding either leaks
or panics on the eventual `kai_decref` of a freed cell.

The fixture works because `string_concat` is linear (consumes both
String args). The compiler.kai bootstrap crashes because some of its
patterns hit non-linear consumers — the crash backtraces inside
`mfm_alloc + 592` called from `kai_record(n=11, ...)` in
`kai_cli_with_path`, suggesting one of the record slots received a
freed cell.

## What the next attempt needs

1. **A "linear-consumer" annotation** on stdlib + runtime entry
   points. Either runtime-declared (a registry of linear fn names)
   or compiler-inferred from arg consumption patterns. The third
   condition for skip-set inclusion becomes:
   *"max-path ≤ 1 AND consumed on every path AND every consumer is
   linear"*.

2. **A regression fixture per non-linear consumer** discovered, so
   the analysis can be safely tightened over time without re-
   introducing the bootstrap crash.

3. **OR — give up "branch-aware skip" entirely and target a
   different optimisation.** The 10-12% dup-count reduction this
   lane shows would only translate into a wall-time reduction if
   the dup machinery is still the dominant cost on the ~68 s
   baseline. That re-attribution is still an open follow-up.

## Empirical findings worth keeping

- kaic2 self-compile wall: ~68 s (darwin-arm64, 2026-05-21).
- `kai_internal_dup` in emitted stage2.c: 11,135 (baseline,
  conservative dup discipline).
- With basic max-path optimisation: 9,753 (-12.4%).
- With max-path + consume-coverage optimisation: 10,011 (-10.1%).
- Output md5 baseline: `0b902cd9a4ce08ad3bc5079978f56a92`.

## Cost vs estimate

The original issue estimated 200–400 LOC for the analysis. Actual
this attempt: ~470 LOC (max_paths + consumed + skip_set
infrastructure + threading + `pcs_collect_exit_drops` integration).
Within the 1500-LOC stop ceiling.

The blocker was NOT LOC. It was the non-linear-consumer assumption
buried in the runtime primitives — a problem the issue's "5-pass
coordination caveat" hinted at but didn't name specifically.

## Why the lane ships stubbed instead of reverted

The infrastructure is sound and selfhost byte-identical with stub
proves it. Future work needs the analysis already written here plus
the linear-consumer check. Reverting would force the next attempt
to redo 90% of this work. Shipping stubbed costs zero runtime
(skip_set is always `[]`, short-circuits early in `pcs_is_non_last`)
and leaves a one-line activation toggle.

## Activation toggle (for the next lane)

```kai
# Currently:
fn pcs_branch_aware_skip_params(pnames: [String], body: Expr) : [String] = []

# To activate, replace with:
fn pcs_branch_aware_skip_params(pnames: [String], body: Expr) : [String] = match pnames {
  []           -> []
  [h, ...rest] -> {
    let mp = pcs_max_paths_in_expr(h, body)
    let cv = pcs_consumed_on_every_path(h, body)
    let is_linear = pcs_all_consumers_linear(h, body)   # TODO: implement
    let rest_skip = pcs_branch_aware_skip_params(rest, body)
    if mp <= 1 and cv and is_linear { [h, ...rest_skip] } else { rest_skip }
  }
}
```

## First-attempt context (2026-05-20)

The previous attempt in worktree `perceus-599` validated the dup-
count axis (78% drop with full scope including arm-bindings) but hit
the same 5-pass coordination cliff. Aborted cleanly; no code shipped;
worktree + branch deleted; retro committed via the previous version
of this file. Key insights carried forward:

- Tuples in type position do not parse — use named sum types for
  alist values.
- The `mp` analysis keyed only on name collides across sibling arms
  with same-named pattern bindings. Restrict to fn params from day
  one. (This attempt followed that advice.)
- The 5.74 s figure cited in `docs/perceus-honesty-targets.md` is
  stale (the doc carries a 2026-05-16 refresh note). Real baseline
  measured by this attempt is ~68 s; the wall-time target should be
  re-derived.

## State left for the next lane

- The fixture, `pcs_max_paths_*`, `pcs_consumed_*`, and
  `pcs_branch_aware_skip_params` (stubbed) live in main.
- Activation requires the linear-consumer annotation (separate
  lane).
- Issue #599 stays open with this retro and the comment posted on
  #599 documenting the bootstrap-crash root cause.
