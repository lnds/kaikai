# Lane experience — native `variant_match` super-linear collapse

**Scope:** diagnose and fix the super-linear time collapse of the native
backend on the `variant_match` bench (docs/native-codegen-perf-plan.md §3.4),
a defect distinct from the linear all-boxed boxing gap.

**Issue:** #855 (opened + closed by this lane's PR).

## Scope as planned vs. shipped

- **Planned:** find why native build+match of variants scales super-linearly
  while C stays flat (N=40K → 0.9 s, N=80K → >70 s, RSS near-constant), fix the
  root cause in the native runtime/codegen, gate on linear scaling + parity +
  selfhost + ASAN.
- **Shipped:** root cause was the runtime's **immortal-variant cache**, not the
  match path the plan and the candidate list pointed at. Fixed with a one-line
  runtime change (the load-bearing fix) plus a KIR match-exit-drop change (a
  latent native leak the cache had been masking). N=80K went from **>70 s to
  0.35 s** — scaling is now flat, matching C. Two regression fixtures added.

## Root cause (with evidence)

The bench rebuilds `tree(i)` each round — a fixed 5-node `Expr` tree whose
leaves carry the round index `i` (a variable Int). The work per round and the
live set are both constant, yet native time grew ~100× for a 2× increase in N.

The investigation followed the plan's tools (`KAI_TRACE_RC`, IR dump, an
allocator read) but the signature kept contradicting the candidate causes:

1. **Bisection** (build+drop vs eval-of-a-reused-tree vs full) isolated the
   collapse to the **build path**, not eval/match: evaluating a pre-built tree
   N times was flat; rebuilding-and-dropping was not.
2. **RC trace** showed native `free_total=1, decref_total=1, leaked≈allocs` —
   the native backend freed almost nothing, while C freed (decref ≈ 520k at
   N=40K). First read: a missing match-exit drop.
3. The **KIR + IR** *did* emit the scrutinee drop once added, yet
   `decref_total` stayed 1. Instrumenting `kai_internal_drop` revealed it was
   **called 55001 times but `kai_decref` short-circuited every time** — the
   dropped cells had `rc == INT32_MAX` (immortal).
4. The cells were immortal because of the **immortal-variant cache**
   (`kai_variant_u` → `kai_slots_all_immortal_ptr`). A tagged-Int *immediate*
   slot was counted as "immortal" (line: "An immediate value (tagged Int) has
   no header and is never freed — it counts as immortal"). The native all-boxed
   codegen boxes every Int field and routes it through the `mask==0` cache
   path, so `Lit(i)` for **arbitrary** `i` was interned — one cache entry per
   distinct `i`. The cache is a fixed 262144-bucket open-addressing table; an
   unbounded set of `Lit(i)` saturates it, degrading its linear probe to
   **O(n) per insert/lookup** → quadratic total. RSS stays flat because the
   table is pre-allocated. N=40K is below saturation (fast); N=80K crosses it
   (collapse). This matches the observed threshold exactly.

The C backend never hits this: it builds `Lit(k)` via `kai_variant_u_fast` with
a **typed `.i64`** slot (the Int unboxed in the slot), bypassing the `mask==0`
cache path. The divergence is a representation divergence, not a missing drop.

## The fixes

1. **`stage2/runtime.h` `kai_slots_all_immortal_ptr` (load-bearing).** A
   tagged-Int immediate slot now **disqualifies** the variant from
   immortalisation. The cache is for variants of *bounded cardinality*
   (nullary ctors, all-immortal-pointer cells); a variant containing a variable
   tagged Int has unbounded distinct identities and must not be interned. One
   line (`continue` → `return 0`) plus the rationale comment. Closes the whole
   class (any variant with an Int payload, not just `Lit`).

2. **`stage2/compiler/kir_lower_walk.kai` match-exit drop (defense + parity).**
   The native KIR match-lowering declared `__pcs_scr_drop` (the owned
   scrutinee) but never emitted its `KDrop` — it treated every scrutinee as
   borrowed. With immortalisation disabled, the rebuilt-each-round `Lit(i)` is
   `rc=1` and must be freed, so the exit drop is now required for the live set
   to stay constant. The fix mirrors the C oracle's `emit_match_default_owned`:
   an OWNED scrutinee (not wrapped in `__perceus_borrow`) is decref'd in the
   match continuation block (the post-dominator of every non-tail arm); a
   BORROWED one is not. The owned/borrowed signal is lexical:
   `__pcs_scr_drop` is declared only for an owned match (`match_preamble`), so
   `match_finish` drops it exactly when present. The `variant_match` recursion
   is `eval(a) + eval(b)` — NOT a self-tail — so the scrutinee always flows
   through the continuation and is dropped there.

Both fixes are needed: #1 stops the cache saturation, #2 reclaims the cells #1
no longer immortalises. `live_peak` is now constant in N (12 at both N=5K and
N=40K) where it grew linearly before.

### Rejected sub-attempt: planting the drop before a self-tail back-edge

asu flagged (correctly, in the abstract) that a self-tail arm seals its block
before the continuation, so it would skip `match_finish`'s exit drop and leak
an owned scrutinee. I first added a `plant_scr_drop_for_tail` that emitted the
drop before `KTcrecGoto` / `KTrmcStep`. **This double-freed the cons-modulo
(`KTrmcStep`) path** (`map` / `filter`): the TRMC reuse + the goto's dropmask
ALREADY consume the scrutinee's cons cell, so the extra exit drop freed it
twice — corrupting the list spine, which `list_sum` then walked into freed
memory, manifesting as a **stack overflow** (`closure_lifecycle_298` and
`filter_heap_bounded_817` crashed at exit 139/133 *after* printing correct
output — the give-away that it was a teardown UAF, not a logic bug). The
serial native-vs-C check caught both; the parallel parity ratchet had masked
them as flaky build-race fails. The plant was removed. The residual leak — a
self-tail arm over an *owned, non-reused* scrutinee — is a pre-existing native
limitation (the all-boxed backend "already never cascades decref→free" per the
existing `nemit_tcrec` comment); fixing it precisely needs the C oracle's
`pmask` raw/boxed param distinction and belongs to the codegen typed-slot
follow-up, not this lane. It is unrelated to the immortalisation collapse.

**Lesson:** the parallel parity ratchet's "0 gaps (flaky held)" is NOT a
sufficient gate for a change that touches RC discipline — a real teardown
double-free hid inside the flaky-fail set. A serial native-vs-C pass over the
touched shapes (perceus / list-recursion / closures) is the gate that catches
it. Always run it for an RC-touching native change.

## Scaling, before vs after (measured, timeout 70 s)

| N    | native before | native after | C     |
|------|---------------|--------------|-------|
| 10K  | 0.30 s        | 0.31 s       | 0.03 s |
| 20K  | 0.30 s        | 0.32 s       | 0.04 s |
| 40K  | 0.86 s        | 0.32 s       | 0.05 s |
| 80K  | **>70 s (timeout)** | **0.35 s** | 0.05 s |

Native is now **flat** in N (was >200× at N=80K), matching C's shape. The
residual ~7× native-vs-C gap is the pre-existing *linear* all-boxed boxing gap
(cause #1 in the plan), out of scope for this lane. Outputs are identical
native == C at every N.

RC after the fix is balanced and **constant** in N: `live_peak=12` at both
N=5000 and N=40000 (`leaked=3`, a fixed constant — the program-name string
etc., not growing).

## Structural surprises the brief did not anticipate

- **The candidate-cause list was a red herring for the proximate mechanism.**
  The plan listed "non-amortised slab/free-list", "RC bookkeeping that grows",
  "reuse-token interaction". The actual O(n) structure was the immortal-variant
  cache's linear probe — an *interning* table, not the allocator's free path.
  The "time super-linear, RSS flat" signature pointed there once the immortal
  short-circuit (`decref_total=1` despite 55k drop calls) was found.
- **Two runtime.h copies bit twice.** The native binary links
  `stage2/runtime.h` (tagged Ints, the immortal cache), not `stage0/runtime.h`.
  Instrumentation in the wrong copy produced no output and cost a detour. The
  fix belongs only in stage2 — stage0 has no tagged-Int representation, so its
  `kai_slots_all_immortal_ptr` cannot hit this.
- **The native bitcode (`runtime_llvm.bc`) shadows runtime source edits.** With
  the P2 bitcode-link present, editing `stage2/runtime.h` has no effect until
  the `.bc` is regenerated (a kaic2 rebuild) — a runtime edit alone is silently
  stale.
- **A `KDrop` is a `KRCOp`, not a `KStmt`.** The first attempt emitted
  `ls_emit(st, KDrop(...))`; the KStmt wrapper is `KRC(KDrop(v), pos)`. stage1
  did not reject the shape at compile time — it surfaced as a runtime
  "non-exhaustive match" in kaic2's own lowering. Worth remembering: stage1's
  exhaustiveness is weaker than stage2's, so a malformed KIR node can reach
  runtime.

## Fixtures added

- `examples/perceus/variant_int_payload_no_immortal.kai` (+ `.out.expected`) —
  a variant with a variable Int payload, rebuilt each round. Picked up by the
  backend-parity harness (native == C output `135450`). A regression
  (re-immortalisation or a dropped exit-decref) shows as a parity diff or, with
  `KAI_TRACE_RC`, an unbounded live set.
- The existing `tools/native-perf/benches/variant_match.kai` is the perf gate
  for the scaling (left untouched — the bench shape is correct and flat in C).

Coverage gap: the TRMC-owned-scrutinee path (the `lower_tcrec` /
`lower_trmc_seal` plant) is exercised manually in this lane but the perceus
fixture above is variant-switch-shaped, not list-TRMC-shaped. A follow-up could
add a list-TRMC-over-an-owned-list fixture; the manual run (`build(5,[]) |>
inc_all |> sum` per round) was `free=30000, leaked=1, live_peak=5` + ASAN-clean.

## Follow-ups left for next lanes

- **Codegen typed-slot parity (the deeper root).** The native all-boxed path
  should build a variant with an Int payload via the typed `.i64` slot
  (`kaix_variant_typed` / the `_fast` path the C backend uses), not box the Int
  and route it through `mask==0`. That eliminates the representation divergence
  at the source; the runtime fix then stands as permanent defense-in-depth.
  asu's call: ship the runtime fix first (this lane), do the codegen parity as
  a separate lane — do not invert the order (the all-boxed KIR model is still
  live on other paths, so the runtime guard must land first).
- The residual linear native-vs-C boxing gap (~7× here) is the P1/P2 plan's
  territory, unchanged by this lane.
