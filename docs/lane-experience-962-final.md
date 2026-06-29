# Lane experience — #962 final: typecheck-once evaluated, NOT built; umbrella closed

## Scope as planned vs as shipped

**Planned (brief):** implement typecheck-once (reuse the core's partial
`TypedProgram` across files, the `asu` blueprint validated in Lane 4),
wire `--batch-list` into the rejection-fixture runner (`test-negative.sh`,
the ~271 typecheck-only `.err.expected` fixtures that do not link), measure
before/after, and `closes #962`.

**Shipped:** no compiler code. This lane is a **measure-then-decline**
result. Two findings, both measured on this tree, redirected it:

1. **typecheck-once is sound and implementable** (`asu` PUNTO 1, verified
   against the live `infer.kai`) — but
2. **its only intended consumer in this lane (the rejection runner) gains
   nothing**, because that runner was already parallel. The umbrella's real
   value was already banked by Lanes 1–4. typecheck-once does not pay its
   complexity, so it was not built. #962 closes on that honest basis.

## The measurement that decided the lane

The brief's premise for the win was: the rejection fixtures are
typecheck-only (no `cc`/link), so amortising the core typecheck across
them with `--batch-list` would pay. Measured on an M-series mac, C-only
`kaic2`, over the 133 effective negative goldens
(`examples/negative/**`, `silent_contract/` excluded):

| Run | Wall-clock |
|---|---|
| `test-negative.sh` SERIAL (`KAI_TEST_JOBS=1`) | **21.42 s** |
| `test-negative.sh` PARALLEL (default `xargs -P$(ncpu)`, 14 cores) | **2.47 s** |

The runner **already fans out over `xargs -P$ncpu`** (one isolated `kaic2`
per golden, each writing its own keyed errfile — `tools/test-negative.sh`
lines 104–107, 222–227). The ~8.6× between serial and parallel is the
core parallelism, already captured.

A `--batch-list --check` would amortise the core typecheck (271× → 1×)
but runs **serial inside one process** — it competes against 2.47 s on a
single core and loses. The amortised core load (~0.07 s lex+parse +
a fraction of the ~0.08 s typecheck, per a `0.15 s` standalone `--check`)
cannot recover the lost N× of parallelism. Sharding the batch into N
parallel processes only rebuilds `xargs -P` with added surface (per-entry
stderr separation, marker protocol or errfiles, re-parsing) for a
marginal core-load saving inside each shard. `asu` (PUNTO 2) reached the
same conclusion independently and recommended **not** wiring the batch in.

## Why typecheck-once is sound but still not worth building (asu PUNTO 1)

`asu` verified the four `all_program_decls`-consuming paths in the core
bucket's typecheck and confirmed the partial core `TypedProgram` is
byte-identical across distinct user files:

| Path | Function | How it reads `all_program_decls` | Verdict |
|---|---|---|---|
| `rk_table` | `row_kind_slots_of` | name lookup, first-match | inert unless name collision; core wins |
| `alias_res` | `inf_find_alias` | alias-name lookup, first-match | inert unless collision; core wins |
| `check_default_coverage` | iterates the bucket; `effect_default_op_map_get` first-match | core body only emits its own effect keys | inert unless collision; core wins |
| `check_field_privacy` | fires on `obj.ty = TyCon(Some(decl_mod), …)` | core never produces an `obj.ty` pointing at a user module (directional visibility) | totally inert |

`all_program_decls = flatten_module_decls(modules)`; `partition_decls_by_home`
emits buckets in first-appearance order with `__builtin__`/core/prelude
**before** the target (user) bucket, which is last. Every lookup is
first-match, so the core always wins any name collision — the user's
entry sits after the core's and never shifts the core's inference. The
sole non-invariance vector is exact name collision, and that is
*shadowing of the user bucket* (not reused), never the core. Lanes 1
(`root_fns` cut) and 2 (proto-ops cut) closed the two remaining
contamination paths; the Lane 3 oracle guards the invariant.

So typecheck-once **could** ship correctly, gated by `.c` byte-id
batch-vs-fork. It was not built because there is no consumer where it
moves the needle:

- **Rejection runner** — already parallel (above); a serial batch loses.
- **Link+run harness** — `cc`/link is ~78% of a `0.93 s` build (Lane 4's
  cost decomposition); the core typecheck is ~0.07 s. Amortising it is
  in the noise. Lane 4 already declined to convert this harness for that
  reason.
- **Emit-only batch (Lane 4, `--batch-list`)** — already shipped, already
  amortises parse, measures 1.60×. typecheck-once would add the core
  typecheck on top, but the standalone core typecheck is a small slice of
  emit-only cost and a vanishing slice of a full build. The ceiling is a
  modest bump on a path the CI barely uses.

## What #962 actually delivered (banked in main)

The umbrella's value was real and is shipped — it was **correctness**,
not the headline ~20× (which the title assumed and measurement refuted):

- **Lane 1 (#969)** — `root_fns` isolated to the target bucket: a real
  user→core contamination cut + regression fixture.
- **Lane 2 (#967)** — `lower_protocols` user→core protocol-op
  contamination cut.
- **Lane 3 (#973)** — differential independence oracle + `--dump-typed-core`;
  the soundness gate, running in tier1.
- **Lane 4 (#979)** — `--batch-list` parse-once mode, 1.60× emit-only,
  byte-id 50/50, with `batch-isolation.sh` in tier1.

The title's "kills tier1's ~12min redundant re-typecheck" rested on the
hypothesis that the per-file overhead was the typecheck. Measured (Lane 4
+ this lane), the overhead is ~0.06 s parse + ~0.07–0.08 s typecheck, and
both are dwarfed by `cc`/link in any real build, while the one
typecheck-only consumer (the rejection runner) was already parallel. The
hypothesis was wrong; the correctness work it motivated was not.

## Decision

typecheck-once is sound, implementable, and unjustified. Building it is
surgery in two F-monoliths (`infer.kai` 19.6k LOC, `driver.kai` 4.6k) with
real byte-id risk, for a ceiling the measurements bound to "modest on a
path the CI barely uses." #962 closes on the value already in main; the
typecheck-once lever is **declined by measurement**, not deferred to a
phantom follow-up.

## Fixtures added

None. No compiler change, so no behavioural fixture is warranted; adding a
typecheck-once fixture would assert code that does not exist. The standing
isolation gates (`batch-isolation.sh`, the Lane 3 independence oracle)
already guard the invariant any future typecheck-once attempt would rely on.

## Real cost vs estimate

Investigation + two independent measurements (this lane and the `asu`
consult). No implementation. The cost was almost entirely the work needed
to know **not** to implement — which is the cheap outcome relative to
landing a risky reuse path through two F-monoliths for a marginal,
CI-invisible gain.

## Follow-ups

If a future need arises for a **typecheck-only corpus gate** (a
`--batch-list --check` pass with no codegen and no `cc` in the per-file
cost) that is NOT already parallel, typecheck-once becomes worth
revisiting — the `asu` blueprint (partial core `TypedProgram` PRE-`resolve_protocol_calls`,
gated by `.c` byte-id) is validated and recorded here and in
`lane-experience-962-lane4.md`. Until such a consumer exists, it stays
declined.
