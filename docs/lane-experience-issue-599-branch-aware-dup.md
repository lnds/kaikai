# Lane experience — issue #599 branch-aware dup elimination (ABORTED)

**Lane:** `perceus-599`
**Date:** 2026-05-20
**Outcome:** Aborted per STOP rule. No PR opened. Compiler edits reverted.

## Scope as planned

Make `pcs_pass`'s dup-wrap predicate branch-aware. The conservative
variant (`pcs_count_non_lam_uses >= 2`) wraps `__perceus_dup` around
every non-last read of every multi-use binding, *including reads in
mutually-exclusive match arms and if-branches that cannot both
execute*. The fix: count the maximum non-lam reads on **any single
execution path** (max over arms, sum over sequential composition),
and skip the wrap when that count is exactly 1.

Targets (from the issue gate):
- `kaic2` self-compile wall drop ≥ 10%
- `kai_internal_dup` total count drop ≥ 20%
- Selfhost byte-identical
- Tier 0/1/1-ASAN green
- Fixture `examples/perceus/branch_aware_dup.kai`

## Scope as shipped

**Nothing shipped.** Implementation produced runtime-incorrect RC
discipline (`panic: non-exhaustive match` during second-iteration
bootstrap). After 4 iterations of alignment, the lane hit the STOP
rule and was aborted.

## What worked

1. **Empirical baseline established.**
   - kaic2 self-compile wall: ~68.0 s (median of 3 runs on this
     branch, post-Phase 3 unboxing + KAB2 + LSP work; not the 5.74 s
     figure from the original honesty doc — that snapshot pre-dated
     the unboxing wave).
   - `kai_internal_dup` occurrences in the kaic2-emitted
     `stage2.c`: **43 145**.
   - Output md5: `80110875301fe40b37b48d293e6e1353` (deterministic
     when stderr is suppressed; the warnings stream is the only
     non-deterministic line ordering).

2. **The hypothesis was empirically validated.**
   Adding branch-aware analysis to `pcs_is_non_last` drove
   `kai_internal_dup` count from 43 145 down to 9 460 — a **78 %
   drop**, comfortably above the 20 % target. The compiler.kai
   source pattern-matches exhaustively in `synth_*` / `infer_*` /
   `emit_*` walkers, so the dominant dup wraps did sit in
   mutually-exclusive arms.

3. **The max-path counter (`pcs_collect_max_paths_expr`) is
   structurally sound.**
   Built as a parallel companion to `pcs_collect_uses_expr`:
   `mp_seq` for sequential composition, `mp_alt` for `EMatch` arms
   and `EIf` branches. Lambdas contribute 0 (LUBlocked path stays
   intact). Pre-computed once per fn body. Did not need to touch
   the existing `Use` type or `last_use_for`.

## What did not work — the 5-pass coordination

The issue's own warning was exact:

> **5-pass coordination caveat (lesson from #593):** the perceus
> arm-drop pass, dup-wrap pass, tcrec dropmask, and reuse
> recogniser all currently assume the conservative-dup output.
> Changing `pcs_pass` to emit fewer dups may break their
> invariants.

I aligned three of them (`pcs_collect_exit_drops`,
`pcs_collect_block_let_exit_drops`, `tcrec_compute_site_dropmask`)
and the binary still panicked on its own bootstrap. The remaining
mis-aligned passes are likely:

- `pcs_arm_drop_pass` (drops arm-pattern bindings) — uses a
  *local* `arm_count` that's independent of the global `mp` and
  not branch-aware. If the rewriter skips the wrap on an arm-bound
  name whose body reads it twice across mutually-exclusive
  sub-arms, `pcs_collect_arm_drops` still emits a drop that
  double-frees.

- Stage-0/stage-1 runtime primitives consume linearly — every
  `__perceus_dup` skipped removes an incref the consumer was
  silently relying on. The conservative-dup discipline made every
  read into "incref then consume"; the new discipline makes the
  single-path read into "raw transfer". The transfer is safe iff
  every consumer is RC-aware (decrefs on consume) AND no
  later-path drop tries to re-decref. The lane discovered the
  drop side; the consumer-side audit is still open.

- `pcs_collect_arm_drops` rescans the arm body for arm-bound
  names with `[nm]` as scope, then decides on `arm_count >= 2`.
  Branch-awareness here needs a per-arm-body max-path counter,
  not the fn-wide `mp` (because pattern-bindings in sibling arms
  shadow each other — same name, different scopes, different
  refs).

## Structural surprises

1. **Tuple types in type position do not parse.** First attempt
   typed the max-path map as `[(String, Int)]`. The parser sees
   `(String, Int)` as the start of an arrow type and errors with
   "expected `->` in function type". Replaced with a named sum
   `type MPE = MPE(String, Int)`. The kaikai surface idiom for
   alists is variant constructors, not tuples — matches the rest
   of the compiler (`EU`, `ME`, `EA`, `BB`).

2. **`mp` keyed by name only loses scope discrimination.** Two
   pattern-bound `h` in sibling arms collide in `[MPE]` because
   the alist has no scope dimension. My first attempt extended
   `mp` with arm pattern bindings; it produced a stable `mp` but
   the dup-wrap decisions for arm-bound names became wrong
   because `mp_alt(arm1_h_count, arm2_h_count)` is not what
   "max reads of THIS arm's h" means. Pulled the arm-binding
   handling out: `mp` only tracks fn params; `pcs_is_non_last`
   falls back to flat count for non-param names.

3. **Bootstrap fixed-point reveals correctness, not single-pass.**
   stage1 (which still uses conservative-dup) compiles the new
   compiler.kai without issue. The new kaic2 (compiled by stage1)
   then compiles compiler.kai and emits less dups — but the
   resulting binary, when run, panics. The bug is invisible at
   "first level" because stage1 is the lifeguard. Reaching it
   requires the full fixed-point cycle.

4. **The wall regression source may be subtler than #599 names.**
   The issue cites `docs/perceus-honesty-targets.md`'s 2.15 s →
   5.74 s figure. The refresh note from 2026-05-16 (#604)
   already acknowledges those numbers were superseded by Phase 3
   unboxing + #123. The actual current wall (post-unboxing) is
   ~68 s for kaic2 self-compile; the issue's 10 % target on 5.74 s
   does not map onto today's reality. Re-measuring is part of
   future scoping.

## Fixtures attempted

Did not land a fixture. The lane never reached an acceptance gate
where a fixture would be meaningful.

## Real cost vs estimate

Issue estimated 200–400 LOC for the analysis itself, plus a "risk:
5-pass coordination" warning. Actual:

- Analysis itself: ~330 LOC (`pcs_collect_max_paths_*` family,
  `mp_seq` / `mp_alt`, type `MPE`).
- `pcs_*` chain threading of `mp`: ~80 LOC of signature changes,
  threaded through ~14 functions.
- Downstream pass alignment attempts: ~50 LOC (exit_drops,
  block_let_exit_drops, tcrec_compute_site_dropmask). All
  insufficient.

Total touched: ~460 LOC, well within the 1500-LOC stop ceiling.
The blocker was not LOC; it was undermodeled invariants in
`pcs_arm_drop_pass` and likely a fourth downstream pass.

## Follow-ups for the next attempt

1. **Audit every site that pivots on `pcs_count_non_lam_uses >= 2`
   or its arm-local twin** before touching `pcs_is_non_last`.
   Grep landed 5 such sites; the lane only aligned 3.

2. **Build a regression fixture first.**
   The shape the lane needed but never landed:
   `fn f(x) { match s { A -> use(x); B -> use(x) } }` where
   `KAI_TRACE_RC` asserts `kai_internal_dup` calls == `kai_incref`
   calls from the consumer side. Without a fixture that exercises
   the bug shape AND its downstream emitters, alignment work is
   blind.

3. **Decouple "param vs arm-bound" from the analysis sooner.**
   The lane learned mid-implementation that `mp` keyed only on
   name collides across arms with same-named pattern bindings.
   Next attempt: type the analysis as
   `(scope: [String]) -> Map[ScopeId, Int]` so arm-locals get
   their own keying. Or simpler: scope the optimisation to fn
   params explicitly from day 1 (and let arm-bound names keep
   conservative drops).

4. **Re-validate the wall hypothesis on the current baseline.**
   The 5.74 s figure in `docs/perceus-honesty-targets.md` is
   stale (superseded by #604's refresh note). Today's kaic2
   self-compile is ~68 s; a 10 % drop is ~7 s. Is the dup
   machinery still the dominant cost on this baseline? The
   #599 hypothesis was framed against a 5.74 s wall; the 68 s
   wall has other contributors (post-unboxing, post-cache).
   Re-attribute first.

## Why this retro instead of a PR

CLAUDE.md `Lane discipline`:

> If you find a bug outside your lane, open a GitHub Issue with
> repro + hypothesis. Do not fix it inline.

The lane found a bug *inside* its scope (downstream pass
mis-alignment) that exceeded the 1-fix-per-worktree budget when
attempted. The honest move is to abort, document, leave the
hypothesis validated for future work, and not ship a binary that
panics on its own bootstrap.

The dup-count drop (78 %) and the design (max-path counter) are
both useful artifacts for the next attempt. They live in this
retro and in the lane history; they do not live in `main`.

## State left for next lane

- This worktree (perceus-599) on branch `perceus-599`: changes
  reverted at lane close; the implementation lives in the git
  reflog only.
- Issue #599: stays open. Add a comment with link to this retro
  + the dup count drop empirical (43 145 → 9 460).
- No new issue filed for the `pcs_arm_drop_pass` mis-alignment —
  it's a known coupling per the issue body, not a separate bug.
