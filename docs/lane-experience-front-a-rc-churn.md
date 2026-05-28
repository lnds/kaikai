# Lane experience — Front A (InferState RC churn): a finding, not a fix

**Outcome: the lane's premise was refuted by measurement.** Front A was
planned as a perf win — kill the `InferState` wrapper rebuild that
`docs/front-a-inferstate-rc-churn-plan.md` blamed for "the bulk of the
10.3B increfs." Instrumented measurement shows that attribution is
false: the rebuild is **0.013%** of the increfs, and the implemented
optimization (correct, selfhost-stable) moved the total the wrong way
(+0.06%). This lane ships **no code change to the compiler** — only this
finding and a standalone branch-isolation fixture authored along the
way. The real RC churn is the Perceus by-value model emitting dup/drop
per typed-AST node, a structural lever adjacent to Perceus, not a typer
micro-opt.

## Scope as planned vs as shipped

| | Planned | Shipped |
|---|---|---|
| `Subst.slots → Ref` | yes (plan step 1) | **no** — blocked by effect masking (below) |
| `st_unify: Some(_) -> st` | yes (plan step 3) | implemented as Option B', then **reverted** (perf-neutral/negative) |
| incref drop | "large fraction of wrapper-rebuild increfs gone" | **none** — premise false; net +0.06% |
| selfhost fixpoint | required | held with the change (then reverted) |
| canary fixture | required | kept (`examples/perceus/match_backtrack_unify.kai`) |
| compiler change merged | the lane's deliverable | **none** |

## What the plan claimed

> `st_unify` calls `st_set_sub(st, s2)` on every successful unification
> — **millions of times** … incref'ing the ~17 surviving pointer fields
> each time. **That wrapper rebuild is the bulk of the 10.3B increfs.**

## What was built (and why it was reverted)

The plan's literal step (Subst slots → `Ref`) is blocked. `ref_*` ops
route through handler dispatch with no bare-builtin bypass (unlike
`array_set`), and the masking pass deliberately refuses to mask
`Mutable` for a locally-constructed Ref (infer.kai:11195-11208 NOTE).
Converting Subst slots to Ref pushes an unmaskable `/ Mutable` through
the whole inference call graph to `main`. Enabling Ref masking needs a
`ref_*` bare-builtin lowering — its own codegen+effects lane.

asu's review pivoted to **Option B'**: keep Subst on Arrays, and in
`st_unify` capture the three slot-Array lengths + two fresh counters
before `unify_env`, calling `st_set_sub` only when one changed (an
`array_grow` minted a fresh Array, or `bind_row_tails` bumped
`row_fresh`). Otherwise return `st` untouched — the in-place `array_set`
bindings are already visible through the existing wrapper.

This was **correct**: the selfhost fixed point held (`kaic2b.c ==
kaic2c.c`). The load-bearing correctness catch — initially missed in the
abstract review — is the `row_fresh` bump via `unify_row` →
`bind_row_tails` (infer.kai ~6149): a by-value counter that `Some(_) ->
st` would silently lose, corrupting fresh-row-var allocation. The
5-signal detection (3 lengths + 2 counters) covers the entire Subst
wrapper; selfhost stability confirms completeness.

It was reverted because it does not achieve the lane's goal (see below)
and marginally regresses the metric it was meant to improve.

## The measurement (the actual finding)

Profiled binary: `build/stage2.c` built with `-DKAI_PROFILE_RC=1 -O2`,
run as `KAI_PROFILE_RC=1 ./kaic2 main.kai` from `stage2/`. **This is the
real self-compile** — `main.kai` imports every `compiler/*.kai` module.
(The flat `build/bundle.kai` is NOT directly compilable by kaic2; a run
against it aborts on the first unresolved `import compiler.X`. An aborted
run is how the earlier "~1.4B increfs" figure was mismeasured.)

`st_unify` call-site triggers over a full self-compile (counters patched
into the generated C, one per branch of the Option-B' ternary):

```
[SU] norebuild=76521  rebuild=16748      → total st_unify = 93,269 calls
```

incref totals, baseline (main) vs the Option-B' branch:

```
baseline:      incref = 10,278,869,434
with change:   incref = 10,285,199,320     (+6,329,886 = +0.06%)
```

The arithmetic: removing ~17 increfs on each of 76,521 no-rebuild calls
saves **~1.3M increfs** — **0.013%** of 10.3B. The five extra reads per
call (`array_length` ×3 + two field reads) slightly *exceed* the saving,
so the net goes UP. `st_unify` is called **93k times**, not "millions."
A 17-per-call saving on a 93k-call function cannot move a 10.3B needle.

## Where the 10.3B increfs actually come from

Type-walk call counts over a full self-compile:

```
[ATY] apply_ty=1,341,824  apply_row=264,543  apply_tys=1,241,981  subst_lookup=231,644
```

Even the hottest, `apply_ty` at 1.34M calls, is three orders of
magnitude short of 10.3B (10.3B / 1.34M ≈ 7,670 increfs per call —
impossible per individual call). **There is no single hot function whose
call count explains the total.** The increfs are distributed across the
entire compiler: Perceus emits a `kai_internal_dup` (incref) for almost
every argument pass and field read in every kaikai function. 10.3B is
the aggregate cost of the by-value RC discipline over 77.6k LOC.

This is the third time a per-feature "this one structure is the
bottleneck" diagnosis has failed measurement (see
`project_kaikai_perf_plan_9s_was_false`,
`project_kaikai_mixed_param_unboxing_perf`). The 45% RC-traffic *share*
is real and reproduces; its *attribution* to any single structure has
not.

## Design decisions and alternatives considered

- **Ref vs Array for Subst slots.** Plan said Ref; chose to stay on
  Array (Option B') to avoid the masking/codegen blocker. Correct call —
  even the Array version turned out perf-neutral, so the larger Ref
  rework would have bought nothing while costing a codegen+effects lane.
- **Growth detection signal.** Rejected threading a `grew: Bool` through
  the recursive `unify_env` (high churn on a `Option[Subst]` return);
  rejected pointer-identity (no array pointer-equality contract). Used
  `array_length` comparison + counter comparison. Sound because
  `array_grow` always changes length and the two counters are the only
  by-value wrapper state.

## Structural surprises the brief did not anticipate

1. **The Ref masking blocker** (infer.kai:11195) — the plan assumed
   Subst→Ref was a clean swap; it is gated on a `ref_*` bare-builtin
   lowering that does not exist.
2. **The `row_fresh` by-value bump on the unify path** — the plan and
   the abstract review both assumed array growth was the only thing
   `st_unify` had to propagate; `bind_row_tails` bumping `row_fresh`
   is a second, easy-to-miss correctness requirement.
3. **The bundle-vs-modules profiling trap** — measuring against
   `build/bundle.kai` silently aborts and under-counts; the real
   self-compile input is `main.kai`. This is almost certainly the source
   of the discredited "1.4B" figure in the prior perf doc.
4. **The premise itself** — the headline surprise: the named bottleneck
   accounts for 0.013% of the metric.

## Fixtures added and coverage gaps

- `examples/perceus/match_backtrack_unify.kai` (+ `.out.expected`,
  output `93`), wired as `test-match-backtrack-unify` into stage2
  `test` and tier1. Kept because it is genuinely useful coverage of
  `st_restore_entries` branch isolation + heterogeneous generic
  instantiation + both-flex row-tail unification (`bind_row_tails`),
  and it passes against the unmodified compiler. It is **not** a Front A
  regression test (there is no Front A change to regress).

## Real cost vs estimate

Estimated: a focused typer micro-opt + gate. Actual: most of the time
went to the analysis the brief asked for up front — classifying every
`Subst {` site for Ref aliasing, finding the masking blocker, and then
the instrumentation that refuted the premise. The "implementation" was
small and is gone; the **measurement** is the deliverable.

## Follow-ups for next lanes

- **If RC churn is to be attacked**, it is a Perceus-adjacent structural
  lane: borrow/no-dup analysis on hot argument passes (`apply_ty` /
  `apply_tys` recurse and dup every node), or a different RC scheme for
  typed-AST nodes during inference. Not a typer micro-opt.
- **Retire the InferState-rebuild hypothesis.** Do not re-open it; this
  doc is the record that it is 0.013% of the metric.
- **`ref_*` bare-builtin lowering** remains a real, separable piece of
  work (would unblock Ref-local masking generally), but it buys nothing
  for RC churn on its own.
- **Profiling hygiene:** always profile `main.kai`, never
  `build/bundle.kai`, and confirm non-zero C output before trusting any
  RC count.
