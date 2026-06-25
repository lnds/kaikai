# Lane experience — issue #901 (self-compile RAM peak)

## Scope as planned vs as shipped

**Planned (issue #901):** replace the emitter's immutable-String concatenation
with an efficient `Array[Byte]` accumulator, on the premise that assembling the
~12 MB of C output by cascading immutable `String` concats was the cause of the
≥8 GB self-compile RAM peak.

**Shipped:** the premise was measured to be **wrong**, and the lane pivoted (with
the user's explicit go-ahead) to fix the real cause while still landing a clean
StringBuilder. Two changes:

1. **`SB` deferred string builder in `util.kai`** — a rope over a doubling
   `Array[String]`; `sb_append` stores chunk references (O(1)), `sb_build` joins
   once via the same `string_concat_all`. `fn_bodies` migrated to it.
2. **`fns_prefer_module` memoised by module-origin** (`FnsByMod` cache in
   `emit_shared.kai`) — the real lever for the RAM peak.

## The measurement that flipped the diagnosis

`./kaic2 main.kai` (C-direct backend), output 12.6 MB. Profiled with
`/usr/bin/time -l` (RSS) and `KAI_TRACE_RC` (`alloc_total` churn + `live_peak`
live objects):

| State | RSS peak | live_peak | alloc_total |
|---|---|---|---|
| Baseline | 9.34 GB | 161.4M | 500.9M |
| + StringBuilder (`fn_bodies`) | 8.71 GB | **161.6M (no change)** | 501.6M |
| + memoise `fns_prefer_module` | **5.25 GB** | 98.3M | 438.6M |

The StringBuilder migration of `fn_bodies` left `live_peak` flat — proof the
output rope is **not** the RAM source. Two facts forced the re-diagnosis:

- **`KAI_MAX_HEAP` is a cumulative alloc counter** (`kai_heap_committed`, never
  decremented on free), so the "≥8 GB" abort measures total *churn*, not live
  memory, and aborts at any ceiling. The real RAM metric is RSS / `live_peak`.
- **The runtime is RC with free-at-last-use**, so deep concat trees are *not*
  O(n·depth) in live memory — RC reclaims each intermediate as it's consumed.
  The blow-up is structural retention, not concat cascade.

Root cause: `fns_prefer_module` rebuilt the full ~5400-entry EFn table once per
fn body (~5400 bodies), with an internal O(n·k) `list_has` prune → ~2n² cons
cells. The rotation depends only on `(fns, mo)` and `fns` is invariant across a
run, so memoising by module-origin (tens of modules, not thousands of bodies)
collapses it.

## Paths migrated vs left

- **Migrated to `SB`:** `fn_bodies` (the per-decl body assembly).
- **Left on `concat_all`/`catN`:** the rest of the emitter. Once measurement
  showed the rope is not the RAM source, threading `emit_expr`'s ~70-function
  chain to a builder (high byte-id risk) was not worth it for this lane. The SB
  stays as clean, selfhost-ready infrastructure and reduces alloc churn on the
  body path.

## Follow-ups left for next lanes

- **~38M live objects still added by emission** (mono 60M → emit 98M, peak at
  t=53s of 54s — i.e. during emission). Not located by fast probes (ruled out:
  the 554 lambdas, the decls-filter helpers, `efn_resolve` whose allocation is
  pure churn RC frees instantly). Needs deeper per-phase live-object
  instrumentation; deferred rather than chased (disproportionate to this lane).
  Closing this would push RSS toward the ~4 GB floor.
- **Native/KIR backend has the same un-memoised `fns_prefer_module`** at
  `ls_enter_fn_in` (kir_lower.kai). The native self-compile would benefit from
  the same `FnsByMod` cache; out of scope here (C-direct was the measured path).

## Cost vs estimate

The implementation is small (+~70 lines across 3 files). The lane's real cost
was **measurement**: the issue's prescribed mechanism (StringBuilder) did not
address the cause, and only `KAI_TRACE_RC` `live_peak` vs `alloc_total` profiling
surfaced that. Lesson: when an issue prescribes a *mechanism*, verify it against
the *measured* cause before committing the implementation.

## Selfhost byte-id preservation

`sb_build` calls the same `string_concat_all` over the same chunk sequence, so
the emitted bytes are identical by construction. The memoised table is the
idempotent result of the same `fns_prefer_module`, so resolution is unchanged.
`make selfhost` (kaic2b.c == kaic2c.c) stayed green at every step; the SB was
also exercised under ASan + a ≥2-grow / reused-chunk fixture.
