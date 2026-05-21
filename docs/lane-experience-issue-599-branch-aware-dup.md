# Lane experience — issue #599 branch-aware dup elimination

**Status:** LANDED 2026-05-21. Optimisation is active; self-host
fixed point byte-identical; tier 0 + tier 1 (excluding pre-existing
`test-holes` breakage) green; -7.9% kai_internal_dup on self-compile.

**Dates:**
- 2026-05-20: first attempt in worktree `perceus-599`. Aborted; no
  code shipped.
- 2026-05-21 AM: second attempt in-tree. Landed full infrastructure
  including all three safety conditions and TCO-site dropmask
  alignment, but stubbed because activation broke the self-compile
  fixed point.
- 2026-05-21 PM: third attempt (this one). Identified the actual
  root cause as a soundness bug in `pcs_consumers_linear_elems`
  (list-spread tail `[..., ...nm]` was treated as linear but the
  emitter applies `kai_incref` to it). One-line fix to the linearity
  predicate; activation toggle restored; bootstrap converges; closing.

## What landed

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
   position. Soundness fix this session: `ElSpread(EVar(nm))` in a
   list literal must be treated as **non-linear** because
   `emit_list_tail` lowers it to `kai_incref(nm)` — an incref-borrow,
   not a transfer. Without this fix, multi-iteration self-compile
   crashes (corruption visible as `panic: non-exhaustive match` or
   `field access on non-record`).

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
   computes the skip-set via the triple condition. ACTIVE — no
   stub.

7. **`pcs_collect_exit_drops`** integrates `skip_set` — suppresses
   exit drop when the rewriter elided the matching dup wrap.

8. **`tcrec_compute_site_dropmask`** integrates `skip_set` —
   suppresses TCO-site drop for skipped params.

## What broke (and what the diagnosis taught us)

The previous attempt (PR-stub) believed the blocker was downstream
*coordination*: one more Perceus pass had to also consult `skip_set`,
recursively, until all six passes agreed. Linus reviewed and pointed
to `pcs_arm_drop_pass` as the likely next culprit because it
re-scans arm bodies locally instead of consulting the global
use-counter.

This session inverted that diagnosis. Reproducing the bootstrap
failure under lldb revealed the panic is in **`kai_inv_param_names`**
inside the typer invariant walker — a function that reads `p.pname`
from a `[Param]`. The crash signature was a `KAI_VARIANT` (`TyVarT`)
where a `KAI_RECORD` (Param) was expected: classic use-after-free
followed by memory reuse. That is **memory corruption produced by
the rewriter eliding a dup wrap that the emitter could not honour as
a raw transfer**, not a pass that needed to learn about `skip_set`.

Bisecting which fn first received a non-empty skip-set under
`inv_param_names`'s flavour pinned the second param (`acc: [String]`).
`acc` appears once in each `match` arm:

- `NIL  -> acc`             (consumed)
- `CONS -> ... [p.pname, ...acc] ...` (spread)

`pcs_consumers_linear_elems` walked the list element body and
delegated to `pcs_all_consumers_linear(EVar("acc"))` → `EVar(_) ->
true` → **linear**. But the emitter (`emit_list_tail` for the trailing
spread) lowers `...acc` to `kai_incref(kai_acc)`, an incref-borrow,
not a transfer. Eliding the conservative dup wrap therefore left the
acc reference owned in two places (the new cons cell + the next
iteration's `kai_acc`), and the iteration that reassigned `kai_acc`
without dropping the previous owner accumulated a leaked ref. Over
thousands of iterations the heap state diverged enough that an
unrelated allocation reused a freed cell visible to
`inv_param_names`, producing the cross-type crash.

The fix is one match arm in `pcs_consumers_linear_elems`: a bare
`ElSpread(EVar(nm))` returns `false` for the linearity check on `nm`.
Any other spread (computed expression) recurses normally.

**Lesson**: when a pass-coordination story doesn't reduce to "yes,
one more table-lookup site," the real bug is usually a soundness gap
in a predicate the optimisation depends on. The earlier hypothesis
was reaching for ceremony (a `RcPlan` refactor to centralise five
duplicated predicates) when the right move was to read the emitter
and audit which expression shapes the rewriter's `__perceus_dup`
insertion is actually paired with.

## Empirical findings

- kaic2 self-compile wall: ~68 s (darwin-arm64, 2026-05-21).
- `kai_internal_dup` in emitted compiler.c:
  - Baseline (skip-set always `[]`): 11,254.
  - Active triple condition + soundness fix: **10,364 (-7.9%)**.
- Output of `kaic2 compiler.kai`:
  - Pre-activation md5: `6310d458ca3622a3647f13fda01b9c6d`.
  - Post-activation md5: `5e71c7c8930ed617f123200d417c7b05`.
  - **Self-host fixed point byte-identical** under the post md5.
- Fixture `examples/perceus/branch_aware_dup.kai` compiles + runs +
  matches `.out.expected`.

## Cost vs estimate

The original issue estimated 200–400 LOC. Actual landed total over
all three attempts: ~700 LOC (the same as the AM attempt, plus a
~12-line predicate fix). Within the 1500-LOC stop ceiling.

The work that closed the lane was a one-line diagnosis under lldb +
a single Edit to `pcs_consumers_linear_elems`. The cost was almost
entirely in routing past the wrong hypothesis from the previous
attempt's retro.

## Things this lane changed about how we look at Perceus bugs

1. **The emitter is the ground truth for what counts as "linear."**
   The rewriter inserts `__perceus_dup` calls; everything not
   wrapped is a raw transfer ASSUMING the emitter doesn't add an
   `kai_incref` of its own. A linearity-predicate audit needs to
   pair every `pcs_*_consumers_linear_*` site against the
   corresponding `emit_*` lowering and verify they agree.

2. **Multi-iteration self-compile failures look like memory
   corruption, not like rule violations.** When the bootstrap fixed
   point breaks but small fixtures pass, the leak/double-decref is
   accumulating across iterations — the visible crash is a downstream
   read on reused memory. lldb + stack trace at `kai_prelude_panic` /
   `kai_op_field` is the fastest way to localise.

3. **A coordination story should be falsified before it is followed
   to its conclusion.** "One more pass needs to consult the table"
   is the kind of story that can absorb infinite work without
   discovering the actual bug. Three attempts on #599 spent compute
   on this story; the third attempt only escaped by running the
   crashing binary under a debugger and reading what the trap said.

## Follow-ups

- The pre-existing `test-holes` failure (Makefile expects `unfilled
  hole:` but compiler emits `unfilled hole ?` — no colon) is
  orthogonal to this lane but is a tier-1 break on `main`. Open
  separate issue if not already filed.
- Audit other `pcs_*_consumers_linear_*` cases against `emit_*` for
  similar mismatches. Suspects: `EHandle` body emission (handler
  prologue may incref the body's free vars), nested closures.

## Final activation toggle (for historical reference)

The toggle the previous retro documented as "replace stub body with"
is now the actual body of `pcs_branch_aware_skip_params`:

```kai
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

Coupled with the one-arm fix in `pcs_consumers_linear_elems` that
treats `ElSpread(EVar(nm))` as non-linear for `nm`.
