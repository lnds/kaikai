# Lane experience — Issue #440: Phase 4 Option A variant field unboxing

**Branch:** `issue-440-variant-unboxing`
**Span:** single session, 2026-05-14
**Outcome:** layout shipped, acceptance gate NOT met — REOPEN per
RISK #5 of the issue body, follow-up lane required for the extract
refactor that closes the remaining gap.

## Scope as planned

Per the lane brief (and the issue body):

- Variant cells with primitive payloads (`Int`, `Bool`, `Char`,
  `Real`) inline the primitive in the struct C/LLVM payload. Pointer
  payloads stay boxed.
- Constructors take raw scalars for primitive slots, pointers for
  pointer slots.
- Field access returns raw scalar for primitive slot, `KaiValue *`
  for pointer slot.
- RC discipline: `kai_incref` / `kai_decref` no-op on primitive
  slots; the free path walks only pointer slots.
- Acceptance gate: RB-tree benchmark drops from 16× C to ≤ 10× C
  under `-O2`. KAI_TRACE_RC shows incref/decref counts dropping
  ≥ 50%. Cons-list ≤ 1.5× C. Compute ≤ 2× C.
- Two new fixtures (`phase4_unbox_payload.kai`,
  `phase4_payload_rc.kai`).
- Selfhost stage 0 → 1 → 2 byte-identical.
- `docs/perceus-honesty-targets.md` Phase 4 row updated.

## Scope as shipped

**Phase 1 — slot abstraction (commit `394cd33`):** mechanical rename
`as.var.args[i]` → `as.var.slots[i].ptr` across runtime header,
runtime LLVM shim, stage 0 emit.c, stage 1 compiler.kai, stage 2 C
backend match arms and `!` desugar. `KaiVarSlot` is a one-word union
binary-compatible with `KaiValue *`; a new `slot_mask: uint32_t`
sits beside `slots` but is always written 0 in Phase 1. Zero
semantic change. Selfhost byte-identical, demos baseline holds.

**Phase 2 — typed payload codegen (commit `b9eba07`):** stage 2
emitter detects variants whose surface declaration spells `Int` /
`Real` in a payload position, encodes the per-slot kind in
`slot_mask` (2 bits per slot, LSB-first, 0=ptr 1=Int 2=Real, max 16
slots before fallback to mask=0), and emits a new runtime
constructor `kai_variant_u(tag, name, n, mask, slots[])`. Match-arm
extraction and pattern tests read the typed slot. Runtime walkers
(`kai_free_value`, `kai_op_eq`, `kai_to_string`,
`kai_reuse_or_alloc_variant`) branch on `slot_mask == 0` for the
legacy hot path and decode `kai_var_slot_kind` only when typed
slots are present. `kai_take_int` / `kai_take_real` consume a boxed
temporary returning the raw scalar — used when packing a primitive
payload into the typed slot at construction time.

`EVar` extended from `EV(String, Int)` to `EV(String, Int,
[TypeExpr])` so the codegen knows each ctor's payload shape; six
EV sites updated mechanically.

**NOT shipped in this lane (follow-up):**

- **Primitive-slot extract that survives without a fresh `kai_int`
  allocation.** Match-arm bindings on Int slots still emit
  `kai_int(_scr->as.var.slots[i].i64)` which allocates a boxed
  temp. The Perceus pass drops the temp at the right place
  (validated by selfhost byte-identical), but the per-extract
  alloc is what holds the bench above the 10× gate. See the bench
  numbers below.
- **Drop specialisation (#384).** Out of scope per the brief.
- **Reuse-in-place over the new typed layout.** Phase 1 reuse
  predicate ignores typed cells (`slot_mask == 0` required); the
  follow-up extends the recogniser.
- **LLVM backend typed construction.** The LLVM backend still emits
  `kaix_variant(...)`. Bench numbers are C-backend only.
- **Cons-list / compute formal bench.** `make tier0` covers the
  smoke side (demos and compute benchmarks pass with no
  functional regression); absolute wall numbers were not
  collected.

## Acceptance gate result

**NOT MET.**

Workload: 1 M random insert into red-black tree, LCG-seeded keys,
median of 5 runs, M4 Pro / macOS 14, `bin/kai build` (-O0 emitted
+ default cc) against `clang -O2` reference.

| | Wall (median) | × C |
|---|---:|---:|
| Pre-#440 baseline (per `docs/benchmarks/rb_tree_breakdown_2026-05-09.md`) | 3.665 s | 15.93× |
| **Post-Phase-2 (this branch)** | **3.177 s** | **12.6×** |
| C reference | 0.252 s | 1× |

Phase 2 delivers ~21% wall reduction. The acceptance gate of
≤ 10× C is not met. Per RISK #5 in the issue body, this lane
REOPENS #440 with diagnosis before merge.

## Diagnosis — why the gate is missed

The breakdown in `docs/benchmarks/rb_tree_breakdown_2026-05-09.md`
predicted three structural wins from Phase 4 Option A:

1. **Alloc drop**: 3 boxed Int allocs per `RBNode(c, l, k, v, r)`
   eliminated. Realised at construction time. ✅
2. **RC traffic drop**: 5 incref/decref pairs per Node → 2 (only
   the pointer slots `l` and `r`). Realised. ✅
3. **Cascade free drop**: `kai_free_value` walks 5 slots → 2.
   Realised. ✅

Despite all three landing, the bench only moved 21%. Two reasons:

- **The extract reverses the alloc saving.** Match-arm bindings on
  Int slots allocate a fresh boxed temporary via `kai_int(...)`.
  The RB-tree workload's hot loop is `balance`, which destructures
  ~5 Node cells per `balance` call and extracts every Int slot.
  Net allocations per balanced rebuild: -3 at construction, +3 at
  extract = 0 saved per cell visit.
- **Construction is dominated by the `kai_alloc` cost of the cell
  itself, not the Int payload allocs.** Per-cell alloc cost
  measured at ~23 ns in the breakdown's profile; the 3
  primitive-Int allocs eliminated were small-int singletons or
  cache-hits more often than the LCG-keyed `k`/`v` values would
  suggest, so the realised alloc saving is smaller than the
  back-of-envelope.

The structural fixes (free cascade, RC walk, layout) are real
wins for any workload that allocates a Node and never destructures
it back to scalars. RB-tree is the worst case: every insert
matches every cell along the spine, so the extract-side alloc
dominates.

The follow-up lane must:

1. Emit primitive-slot extract that does **not** allocate a fresh
   box. The bind should stay as a raw `int64_t` C local when the
   subsequent uses are all primitive operators (`+`, `<`, `==`,
   passed to a UFn with primitive signature). This is essentially
   Phase 3 unboxing (#383) extended to variant-extract bindings.
2. Failing that — when a use forces boxing — recover the singleton
   path: small Ints (`-128..127`) box via the static pool with
   zero alloc. The current `kai_int` constructor already hits the
   pool, but the bench Ints are out of pool range (LCG keys).
   Singleton extension is its own decision.

Neither item is mechanical; both need scope of their own.

## Structural surprises

**The brief's `STOP` on stage 1 emitter changes was wrong.** The
brief said: *"If the layout opt-in machinery requires changes to
the stage 1 emitter, stop — overlap bootstrap discipline."* The
audit at the start of the lane showed stage 1 directly emits
`_scr->as.var.args[i]` indexing of variants — and changing the
runtime `KaiValue.as.var` struct requires updating that emit site
or the next bootstrap stage refuses to compile.

The resolution was to rename `args` → `slots` in the runtime header
*and* update all three emitters in lockstep. Phase 1 ships that
change with mask=0 forced (no semantic shift) so the chain
remains byte-identical across stages 0 → 1 → 2. Without that
groundwork, Phase 2's typed construction in stage 2 would have no
slot abstraction to write into.

**Conclusion:** the brief's STOP condition was a misread of the
bootstrap layout, not an actual constraint. The lane proceeded
through it deliberately, and the lockstep rename held selfhost
across both Phase 1 and Phase 2 commits.

**The mask uses 2 bits per slot, not 1.** Naive layout had a single
bit per slot ("is primitive?"). That doesn't tell the runtime
walker *which* primitive — needed for `kai_to_string` boxing and
for the free path's read width. Two bits gets us 4 kinds in 32
bits = 16 slots before fallback to mask=0. Kaikai variants in
this codebase max out at 5–7 slots (`RBNode` is 5, the largest
stdlib variant is the `TyFn` arm at 3); the 16-slot ceiling is
not load-bearing.

**Stage 1 cannot parse `|` or `bit_shl`.** Building the mask in
stage 2 with `bit_or(acc, k << (2*i))` would compile fine in
stage 2 (since `bit_or` is a stage 2 intrinsic) but kaic1 — the
stage 1 binary that consumes `stage2/compiler.kai` — does not
recognise those names and emits a parse error. The mask is built
with plain `+` and `*` on disjoint 2-bit windows (no carry
between windows, so `+` == `|`). This pattern reappears whenever
stage 2 source touches bit math.

**Builtins (`Some/None/Ok/Err`) cannot be unboxed.** Their payload
type is polymorphic (`Some(a)` where `a` is whatever the call
site decides). Phase 2 inspects the surface `TyKind`; `TyVar` or
unresolved polymorphic args fall through to pointer slots. The
`EVar` entries for builtins ship with empty `[TypeExpr]` so the
emitter never even considers the typed path. RB-tree's `Tree =
RBLeaf | RBNode(Color, Tree, Int, Int, Tree)` is concrete on
declaration, so its Int slots qualify.

## Fixtures added

- `examples/perceus/phase4_unbox_payload.kai` — `Box = BLeaf |
  BNode(Box, Int)`, asserts construction + match destructure +
  recursion across mixed pointer/Int slots produces correct depth
  and label sum.
- `examples/perceus/phase4_payload_rc.kai` — `IntTriple = ITR(Int,
  Int, Int)`, asserts an all-primitive-slot variant builds and
  destructures correctly. With `KAI_TRACE_RC` the cells produce
  one alloc + one free each, no payload cascade.

Both fixtures execute under `bin/kai run` and produce the expected
output. Neither is wired into a tier — adding them to the demos
golden harness would require restructuring (they sit in
`examples/perceus/`, which the demos sweep doesn't pick up). The
follow-up lane should wire them into tier 1.

## Selfhost discipline

Across the two commits the chain reaches a fixed point:

```
make tier0
  → kaic0 (stage 0 native, runtime + emit.c updated)
  → kaic0 stage1/compiler.kai > stage1.c → kaic1
  → kaic1 stage2/compiler.kai > stage2.c → kaic2
  → kaic2 compiler.kai > stage3.c → kaic3
  → kaic3 compiler.kai > stage4.c
  → diff stage3.c stage4.c = empty (exit 0)
```

Verified at the end of each commit. Selfhost is the load-bearing
correctness gate for this lane — the runtime ABI changes are not
defensible without it.

## Real cost vs estimate

| | Estimate | Actual |
|---|---|---|
| Brief estimate | 1–2 days (quirúrgico) or 4–5 (deeper refactor) | — |
| Issue body estimate | 10–12 days | — |
| **This lane (single session)** | — | **~one focused session** for both phases + retro |

The actual cost was much lower than the issue body estimate
because (a) the rename was genuinely mechanical and (b) Phase 2's
codegen change is contained to one emitter site
(`emit_call_expr_default`'s `else` branch) plus the match-arm
extract. The reason this is shorter than 10–12 days is the same
reason the gate is missed: the lane did not attack the
match-arm extract refactor, which is where the remaining lift
lives. That work *would* land in the 10–12 day budget.

## Follow-ups left for next lanes

1. **Primitive-slot extract that stays raw (#440 reopen).**
   Required to close the ≤ 10× C gate. Likely 3–5 days. Should
   re-use the Phase 3 unboxing (#383) infrastructure for raw
   bindings; the new wrinkle is that the binding originates from
   a match-arm extract rather than a call boundary.
2. **Drop specialisation (#384).** Re-evaluate priority after the
   extract refactor lands and the bench re-runs. Linus's
   call-count arithmetic predicted a clean win post-Phase-4; if
   the gate closes from item 1 alone, #384 may drop in priority.
3. **Reuse-in-place over typed cells (#118 + #209 follow-on).**
   Phase 1 reuse predicate refuses typed cells. Extending the
   recogniser to know the new layout is its own lane.
4. **LLVM backend typed construction.** The LLVM backend still
   emits the legacy `kaix_variant`. C-backend-only optimisation
   means LLVM-only builds see no win. Migration is mechanical
   once the C backend stabilises.
5. **Wire fixtures into tier 1.** Both Phase 2 fixtures should
   join the regression harness so the slot layout stays
   protected.

## Acceptance gate honesty

Per the lane brief: *"If RB-tree bench does NOT improve ≥ 30%
post-fix, parar — predicción incorrecta, REOPEN."* The realised
improvement is **~21%**, below the 30% threshold. The honest
read is that Phase 2 ships a correct, structurally-sound layout
change, but the bench-side gate falls short of both the issue's
target (≤ 10× C) and the brief's stop-condition (≥ 30%
improvement). This retro is the REOPEN diagnosis; the PR is
opened as draft and #440 stays open for the follow-up.
