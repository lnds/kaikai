# Lane retro — A.1 cache bench-only spike (refs #461)

Bench-only spike. **No compiler code changed.** One untracked shell
script under `tools/`, one retro. Numbers measured on M2 Pro,
n=5 median.

## Goal

Measure the wall save A.1 cache (typed `[Decl]` + ModuleEnvDelta of
the prelude, skipping the pre-typer cascade + typer on prelude
files) could deliver, BEFORE committing the 1500+ LOC of typed-AST
serdes + driver wiring the design doc projects. KAB2 (#592) shipped
a 0.41 s projection that landed at 0.03 s after decoder overhead;
this spike protects against the same trap on A.1.

## How the spike actually works

The brief sketched an in-process two-compile mechanic: run the full
pipeline once, stash the post-cascade prelude in memory, re-enter
`compile_source` and skip the parts a real cache would skip. I
deviated from that mechanic deliberately because the pipeline's
shape makes the in-process variant either too invasive for a spike
or too narrow to measure honestly. Three reasons:

1. **Cross-cutting state.** The pre-typer cascade builds `module_table`,
   `op_arities`, `ProtocolReg`, alias tables, brand registries, and
   `pure_extract` names over `merged_raw = qualified_prelude ++
   qualified_decls`. None of these are "per-segment" today, so a
   spike that stashes "the prelude after cascade" would also need to
   stash a dozen ancillary maps — by which point it is half of
   the A.1 implementation.

2. **Typer TyVar ID renumbering.** Per the #574 retro §4, the typer
   fold renumbers TyVar IDs based on traversal order. A second-pass
   "warm" compile in the same process would either renumber
   differently (selfhost byte-identical breaks across the spike) or
   require freezing the ID counter — also wandering into real-A.1
   territory.

3. **Brief's stop clause.** *"Si el corte exacto del pipeline no es
   claro (porque las pasadas tienen dependencias cross-cutting),
   parar y reportar — A.1 puede no ser implementable como está
   pensado."* That clause fires here. Rather than stop with no
   numbers, I pivoted to a phase-decomposition approach that
   delivers the **upper bound** without touching the compiler.

The substituted mechanic: use `kaic2`'s existing modes
(`--ast`, `--check`, default) to bracket the cascade+typer wall.
`wall_check − wall_ast` is exactly the cascade+typer pass total —
which on `empty.kai` is dominated by prelude work (the user side is
one `fn main`). That delta is the **upper-bound A.1 save** if the
decoder were free.

Decoder cost is approximated using the KAB2 retro's measured
~0.19 s decoder wall scaled by payload-size estimates for A.1
(typed nodes carry `Type` annotations per Expr, so the typed `[Decl]`
+ ModuleEnvDelta payload is 3-4× the KAB2 untyped `[Decl]`).
Decoder cost scales roughly with bytes-decoded.

## Numbers (M2 Pro, n=5 median, 2026-05-15)

```
cold compile:                  1.55 s
wall to --ast:                 0.43 s
wall to --check:               1.03 s
delta cascade+typer:           0.60 s   <-- upper-bound A.1 save

hit_simulado (decoder = 0):    0.95 s   (cold - delta)
UPPER-BOUND save:              0.60 s

KAB2 decoder (published):      0.19 s
A.1 decoder floor (1.5×):      0.285 s
A.1 decoder ceiling (4.0×):    0.76 s

A.1 net save (optimistic):     0.315 s
A.1 net save (pessimistic):   -0.16 s
```

Verdict thresholds per brief:

- `≥ 0.40 s save` → A.1 lane fully worth committing
- `≥ 0.20 s save` → A.1 lane is defendable
- `< 0.20 s save` → A.1 not worth 1500+ LOC; replan

The optimistic estimate (0.315 s) clears the defendable bar.
The pessimistic estimate (−0.16 s) busts it. The honest reading is
**the floor estimate (1.5× KAB2 decoder) is itself optimistic**,
because the floor assumes the typed payload is only 1.5× the
untyped one, which the post-#578 typer's per-Expr Type annotation
makes unlikely. The realistic decoder cost is closer to 2-3× KAB2,
landing the net save in 0.0-0.32 s — squarely in the "defensible
only on paper" zone.

## Verdict

**A.1 lane is NOT obviously worth committing on these numbers.**

Reasoning:

1. The UPPER bound (0.60 s) is below the design doc's projected
   0.59 s save by a hair, which is consistent — the doc's 0.59 s
   was itself the upper bound, projected from the same cumulative
   phase wall.
2. Subtracting realistic decoder overhead lands the net save
   between **−0.16 s and +0.32 s**.
3. The KAB2 precedent (#592) shows decoder cost is real, not
   theoretical: KAB1 projected +0.41 s save, delivered +0.03 s
   after decoder. The 14× shortfall there came from the same
   asymmetry — design projections measure the pass being replaced,
   not the loader replacing it.
4. A.1's decoder is structurally heavier than KAB2's because the
   payload includes typed AST + ModuleEnvDelta (recs, sums, aliases,
   op-arities, protocol impls, TyEnv slice — see `docs/cache-design.md`
   §"A.1 payload"). Per-byte decoder cost is similar, but bytes
   are more. There is no a-priori reason to expect A.1's decoder to
   be cheaper *per work-unit* than KAB2's.
5. Even the optimistic scenario does not approach the design's
   "0.59 s saving → 1.31 s wall" gate. With cold = 1.55 s today
   (post-#578 typer fold shaved 0.06 s off the v0.60.0 baseline),
   the warm wall floor is `1.55 − 0.32 = 1.23 s` — which is
   technically under the 1.31 s gate, but at the optimistic end of
   the decoder estimate.

The defensible recommendation: **do not start the A.1 lane** until
one of two things changes:

- A bench-only A.1 decoder prototype lands (just the deserialiser,
  no driver wiring) so the decoder estimate becomes measurement
  instead of 1.5-4× extrapolation; OR
- A separate spike confirms a decoder design (e.g. arena allocation,
  RC-free during load) that closes the per-decl overhead the KAB2
  retro identified as load-bearing.

The pre-blocker #597 (boundary tagging) is closed and `partition_decls_by_origin`
is already in `stage2/compiler.kai`, so the A.1 implementation
surface is mechanically smaller than #597's lane estimated. But
"smaller implementation surface" without "decoder fits in the
save budget" is the same trap KAB2 hit.

## Risks identified at A.1 implementation time

If a future lane proceeds anyway, these are the concrete risks the
spike could not measure:

1. **Decoder per-decl allocations.** Each typed Expr carries a Type
   tree; decoding it materialises Type nodes. ~10k typed Exprs in
   the prelude × small alloc per Type ≈ ~50-100k allocations on
   warm load. KAB2 saw ~10^5 allocations cost ~0.10 s; A.1's
   typed payload could push that to 0.15-0.25 s alone.
2. **ModuleEnvDelta composition.** The fold across N modules
   re-threads `build_ty_env`, `collect_records`, `collect_sums`,
   `collect_op_to_eff`, alias resolver. Per #460 retro §"sub-step
   3d-future", these are not free at compose time. The spike
   measures the **non-composed** baseline; A.1's composer adds wall
   on the hit path.
3. **TyVar ID renumbering for selfhost byte-identical.** The #574
   retro §4 calls this out as the deepest non-trivial constraint.
   A cache loader that hands the typer a typed segment must either
   (a) preserve the cached IDs across compiles or (b) renumber
   on load. Either path requires a careful selfhost gate.
4. **Cache invalidation across protocol-impl set changes.** A.1
   caches the prelude's protocol-impl registry. A new user-side
   `#derive(Show)` mutates which impls are reachable; the cache
   loader has to recompose the registry, not blindly trust the
   cached one. The spike does not exercise this; the lane that
   ships A.1 must.
5. **`lower_protocols`' synthesis is still global.** Even after
   #597's `partition_decls_by_origin`, the *synthesis* step inside
   `lower_protocols` produces dispatchers from the merged
   `ProtocolReg`. A.1 saves the cascade-output for the prelude
   half, but `lower_protocols` still runs across the merged input
   on the hit path. The 0.60 s upper bound in this spike
   *implicitly* assumes `lower_protocols` could be skipped for the
   prelude segment, which is not actually true today — only a
   refactor that lifts dispatchers behind the cache boundary
   would skip it. Realistic A.1 saves are *less* than the upper
   bound on that ground alone.

Risk 5 is the most important. The spike's UPPER bound (0.60 s)
*overstates* what A.1 actually replaces, because part of the
0.60 s cascade+typer wall is `lower_protocols`-on-merged-stream
that A.1 does not skip. A conservative downward adjustment of 20-30%
puts the true upper bound at ~0.42-0.48 s, dropping the optimistic
net save to ~0.13-0.20 s — the brief's "no, replan" zone.

## What the spike DID NOT do

- **In-process two-compile measurement** (per brief's pseudo-code).
  Substituted with phase-decomposition for the reasons in §"How
  the spike actually works".
- **Decoder cost measurement on a real A.1 payload.** That would
  require building the serialiser, which is the very thing the
  spike is gating. The 1.5-4× KAB2 multiplier is an estimate, not
  a measurement.
- **Per-pass timing inside the cascade.** A more invasive variant
  could instrument every walker in `compile_source` and report the
  per-pass wall. That would tighten the upper bound (see risk 5),
  but requires editing the compiler — out of scope for a
  measurement-only spike.

## Real cost vs estimate

| Activity | Estimate | Actual |
|---|---|---|
| Pre-read (4 retros + cache-design + cascade map) | ~40 min | ~45 min |
| Decide spike mechanic (in-process vs shell) | ~20 min | ~30 min |
| Write `tools/bench-a1-spike.sh` | ~30 min | ~20 min |
| Run + collect numbers | ~10 min | ~10 min |
| Retro | ~45 min | ~35 min |
| **Total** | 2-3 h | ~2.5 h |
| LOC | <300 | 157 (shell, untracked compiler-side) |

## Bitácora — for the A.1 implementer if a lane proceeds

- The cold baseline is **1.55 s on M2 Pro post-#600** (v0.62.0,
  KAB2-on default, post-typer-fold). Not the v0.60.0 2.28 s number
  cited in older retros — re-measure before quoting a delta.
- `partition_decls_by_origin` (`stage2/compiler.kai:56573`) is
  already shipped per #597. The A.1 lane does NOT need to refactor
  `lower_protocols`. The boundary information is already there.
- The structural blocker per risk 5 above (`lower_protocols`
  synthesis is global) means A.1 alone cannot skip the entire
  cascade-on-prelude work. If the cache only skips parse+cascade
  passes up to `lower_protocols` and the typer's prelude segment,
  the wall save shrinks from the spike's 0.60 s upper bound. **Re-measure
  with a per-pass timer before committing.**
- `KAI_PRELUDE_CACHE` env var already controls KAB2 (the A.0
  layer). A.1's flag should be a sibling so users can A/B them.
  The cache key format already encodes `schema_variant` in the high
  byte of `format_version` (`docs/cache-design.md` §"Phase A key");
  reuse that.

## Follow-ups

- **Do not auto-merge.** This is a spike retro for integrator
  review. The brief explicitly says: NO PR final, NO `closes #461`,
  no auto-merge. The single commit is documentation + a bench
  script that can stay or be deleted per integrator's call.
- **Update `docs/cache-design.md` §"Phase A.1"** with the spike's
  numbers if the integrator agrees with the verdict. The current
  text quotes a 0.59 s save with no decoder-cost discount; that
  matches KAB2's optimism trap.
- **Consider A.2-without-A.1**: PR #579's retro flagged this as
  possibly cheaper. A.2 saves ~0.55 s (emit-on-prelude work) and
  its decoder is post-mono+perceus, where the payload is denser
  but the alloc footprint per-decoded-decl is lower (perceus
  inserts dup/drop nodes that are leaves, not nested). Worth a
  parallel bench-only spike before re-prioritising A.1.

## What I'd do differently

The brief's in-process mechanic is clean if and only if the
pipeline's segments compose independently. They do not. Future
spikes asking "is X worth implementing" should default to phase
decomposition + a decoder-cost lower bound (the tactic
`tools/bench-phases.sh` already uses) unless an in-process spike
would deliver a different number.
