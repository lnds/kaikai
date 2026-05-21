# Lane experience — issue #599 branch-aware dup elimination

**Status:** INFRASTRUCTURE LANDED, optimisation stubbed. The triple
condition for safety (max-path ≤ 1 + consumed-on-every-path + all-
consumers-linear) is correct, validated on the fixture, but enabling
it on self-compile trips at least one more downstream pass we have
not yet aligned. Tier 0 byte-identical with stub.

**Dates:**
- 2026-05-20: first attempt in worktree `perceus-599`. Aborted; no
  code shipped.
- 2026-05-21: second attempt in-tree. Landed full infrastructure
  including all three safety conditions and TCO-site dropmask
  alignment.

## What landed in main

1. **Fixture** `examples/perceus/branch_aware_dup.kai` +
   `.out.expected` — canonical bug shape. `fn pick(s: String, t: Tag)`
   with `match t { A -> use(s); B -> use(s); C -> use(s) }`. With the
   optimisation active, `kai_internal_dup(kai_s)` disappears from
   every arm and the read is a raw transfer to `string_concat`.

2. **`pcs_max_paths_*` family** (16 fns, ~120 LOC): branch-aware
   max-non-lam-reads counter over an `Expr`. Sequential composition
   sums; `EMatch` arms and `EIf` branches take max.

3. **`pcs_consumed_on_every_path` family** (10 fns, ~110 LOC):
   guarantees the binding is consumed on every path through the body
   (covers the missing-else / missing-arm leak scenario).

4. **`pcs_all_consumers_linear` family** (12 fns, ~190 LOC): walks
   the body and verifies every read sits in a linear-consumer
   position. Bare `EVar(nm)` in a return-tail / fn arg / match
   scrutinee / variant field is linear; `EField(nm, _)` and
   `EIndex(nm, _)` are non-linear (borrow-only); calls through
   `EField(<cap>, op)` or intrinsics are non-linear; explicit
   `__perceus_*` calls are non-linear.

5. **`skip_set: [String]` parameter** threaded through:
   - all 15 `pcs_rewrite_*` functions and `pcs_is_non_last`,
   - `pcs_collect_exit_drops` (suppresses exit-drop for skipped
     params),
   - `tcrec_compute_site_dropmask` (suppresses TCO-site drop for
     skipped params),
   - `tcrec_rewrite_body` + `tcrec_rewrite_kind` + `tcrec_rewrite_arms`
     (carries skip_set down to dropmask compute at every recursive
     site).

6. **`pcs_branch_aware_skip_params(pnames, body) : [String]`**
   computes the skip-set via the triple condition. CURRENTLY STUBBED
   to return `[]`. Activation is a one-line edit documented in the
   source above the stub.

7. **`pcs_collect_exit_drops`** integrates `skip_set` — suppresses
   exit drop when the rewriter elided the matching dup wrap.

8. **`tcrec_compute_site_dropmask`** integrates `skip_set` —
   suppresses TCO-site drop for skipped params.

## What does NOT land

The optimisation. With `pcs_branch_aware_skip_params` returning a
non-empty set, the bootstrap fixed-point on `compiler.kai` panics
with `panic: non-exhaustive match` (earlier iterations crashed with
SIGBUS in `mfm_alloc`; aligning `tcrec_compute_site_dropmask`
upgraded SIGBUS to typer panic — a different bug, still a bootstrap
failure). The remaining mis-aligned passes are likely:

- `pcs_arm_drop_pass` — emits per-arm drops based on per-arm
  use-count, not the global skip_set.
- Possibly `pcs_collect_block_let_exit_drops` for `let` bindings
  whose RHS references a skip-set param.

Each one requires the same threading + skip-set conditional we did
for the others. Cost per pass: ~30 LOC, but each adds a stop-the-
world iteration of bootstrap testing.

## Empirical findings (preserved)

- kaic2 self-compile wall: ~68 s (darwin-arm64, 2026-05-21).
- `kai_internal_dup` in emitted stage2.c: 11,135 baseline.
- With basic max-path optimisation: 9,753 (-12.4%).
- With max-path + consume-coverage active: 10,011 (-10.1%).
- With max-path + consume-coverage + linear-consumer active:
  10,066 (-9.6%). The linear-consumer check rules out a small
  number of additional cases, slightly reducing the cut.
- Output md5 baseline: `0b902cd9a4ce08ad3bc5079978f56a92`.

## Activation toggle

```kai
# In compiler.kai, replace:
fn pcs_branch_aware_skip_params(pnames: [String], body: Expr) : [String] = []

# With:
fn pcs_branch_aware_skip_params(pnames: [String], body: Expr) : [String] = match pnames {
  []           -> []
  [h, ...rest] -> {
    let mp = pcs_max_paths_in_expr(h, body)
    let cv = pcs_consumed_on_every_path(h, body)
    let ln = pcs_all_consumers_linear(h, body)
    let rest_skip = pcs_branch_aware_skip_params(rest, body)
    if mp <= 1 and cv and ln { [h, ...rest_skip] } else { rest_skip }
  }
}
```

Then re-run the bootstrap fixed-point. If panic on
`compiler.kai` self-compile, identify the failing pass (likely
`pcs_arm_drop_pass`) and apply the same skip_set integration.

## Cost vs estimate

The original issue estimated 200–400 LOC. Actual this attempt:
~700 LOC (max_paths + consumed + linear + skip_set threading +
pcs_collect_exit_drops + tcrec_compute_site_dropmask + tcrec
threading). Within the 1500-LOC stop ceiling but at the upper
end.

The blocker has consistently been the same: each new safety
condition exposes one more downstream pass that assumed
conservative-dup. The chain of dependencies is longer than the
issue's "5-pass coordination caveat" suggested — it's at least 6
passes (rewriter + exit_drops + tcrec_dropmask + tcrec_rewrite_*
threading + arm_drop_pass + ...).

## Why the lane ships stubbed instead of reverted

All the analysis, the linear-consumer check, the threading, the
exit-drop integration, the TCO alignment, AND the activation toggle
are in place. Reverting would force the next attempt to redo all of
it. Shipping stubbed costs zero runtime (skip_set is always `[]`,
short-circuits early in `pcs_is_non_last` and the other consumers)
and leaves a one-line activation toggle for when `pcs_arm_drop_pass`
gets aligned.

## State left for the next lane

- All helpers and threading live in main.
- Activation requires aligning the remaining downstream pass(es).
- The fixture compiles correctly with the optimisation active
  (shape validated).
- Issue #599 stays open with this retro.
