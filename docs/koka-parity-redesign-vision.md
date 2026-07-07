# Koka-parity redesign vision

Architectural design note. Not an implementation lane. Reference: the Koka
runtime under `../koka/kklib` (Daan Leijen's kklib), read against kaikai's
`stage2/runtime.h` and the native/C emitters as of `c95de1f6`.

## The question

The tactical arc is closed. `#1104` brought the front-end RC to Koka
*counter* parity: allocs 1.00/insert (reuse-in-place), RC ops 4.00/insert
(RC-pair fusion), i64-inline construction, `KProjBorrow`, target-features.
The measured residue is not knobs — it is representation and codegen:

| metric (rb-tree, @1M, arm64) | Koka | kaikai-c | kaikai-native |
|---|---|---|---|
| wall | 0.26–0.30 s | 0.35 s (1.35×) | 0.48 s (1.85×) |
| instructions | 1.04 G | 2.25 G (2.16×) | 6.27 G (6.0×) |
| IPC | — | — | 2.9–3.8 (no stalls) |

Native runs at healthy IPC; the wall tracks cycles≈instructions. **Cutting
instructions is the whole game, and the residue is structural.** This note
names the structural redesigns, orders them by leverage, cites Koka's source
for each, and delivers the honest verdict on whether Koka is reachable.

## What is NOT the gap (correcting the brief)

The brief hypothesised that kaikai's uniform `%KaiValue` — "every Int is a
cell, boxed" — is the root. **The source says otherwise: kaikai already has
Koka's exact small-int tagging, and it already works.** This must be stated
plainly so the redesign attacks the real gap and not a solved one:

- Koka's discriminator is the bottom bit: `kk_is_ptr` is `(i & 1) == 0`,
  `kk_is_value` its negation (`kklib.h:994-999`), `KK_TAG_VALUE = 1`
  (`:992`). Small ints live in the word as `4*n+1` (`integer.h:13-19`).
- kaikai's discriminator is *identical*: `kai_is_value(v)` is
  `((intptr_t)v & 1) != 0` (`runtime.h:587`), `kai_tagged_int(n)` is
  `(n << 1) | 1` (`:597-602`), `kai_untag_int` an arithmetic `>>1`
  (`:604-606`). The header comment cites the Koka lines it mirrors.
  `kai_int` is the inline fast path: fits-immediate → tagged, no header
  (`:3552-3553`). `kai_head_tag` returns `KAI_HEAD_INT` for a tagged word
  without ever dereferencing (`:620`).

So an rb-tree key comparison in kaikai does **not** allocate or deref for
the Int — the tagged word carries it. The `68.75M kai_int` heap allocs the
old comment (`:574`) describes were already removed. **Integer boxing is not
the 2×/6× gap.** Three *other* structural axes are — one of which the brief
did not name.

## The three structural axes, by leverage

### Axis 1 (the big rock) — Opaque `%KaiValue` in the native emitter vs. inlined layout

This is the dominant native-vs-C axis and the one that most cleanly explains
the 6.27 G vs 2.25 G split. It is separable from representation and it is the
highest-leverage single change.

**Evidence, side by side.** The same variant-field read, both backends:

- **C** (`emit_c.kai:3087-3091`) emits, inline, with the struct known:
  `kai_int(kai_var_slots(scr)[i].i64)` / `...[i].r` / `...[i].ptr`. That is
  a GEP + load. The tag test at `:3103` is a direct `...[i].i64 == k`
  comparison. `kai_var_slots(v)` is `((KaiVarSlot*)&v->as)` — a constant
  offset. LLVM sees the whole thing and folds it.
- **Native** (`emit_native_ops.kai:104-107`) emits, for the *same* `KProj`,
  `call kaix_variant_arg(v, i)` — an external call into the shim. Slot reads
  (`:340-354`), field reads, `kaix_int` boxing (`:317`), even `kaix_bool_of_i32`
  (`:146`) are all `ncall_*` to opaque `kaix_*` exports. `%KaiValue` stays an
  opaque pointer the emitter refuses to look inside.

Every field read, tag test, and slot store that the C backend lowers to a
load/store/cmp, the native backend lowers to a `call`+`ret` across a function
boundary the optimiser cannot see through (the shim is a separate TU; there
is no LTO on that boundary — see `project_kaikai_llvm_shim_inline_lto` in the
architect's notes and `docs/747-*`). Call/ret is ~2–4 instructions of prologue
plus the lost fold; in a descent loop that touches slots on every node, that
is precisely a 2–3× instruction multiplier stacked on the C baseline. The
native's 6.27 G is roughly `C's 2.25 G × (shim-call overhead on every
access)`.

**The redesign:** de-opaque `%KaiValue` in the native emitter. Teach the
KIR→LLVM lowering the concrete cell layout — the 8-byte packed header
(`runtime.h:446-449`: `rc:i32, tag:i8, var_n_args:i8, variant_tag:i16`) and
the inline slot array at `&v->as` — and emit `getelementptr`+`load`/`store`
directly, exactly as the C backend's textual `->as` / `kai_var_slots` do.
The shim stays only for genuinely-out-of-line runtime (allocation, the
generic drop/copy walkers, bigint slow paths).

**Fronts touched:** native emitter (`emit_native_ops`, `emit_native_struct`,
`emit_native_term`), KIR lowering where projections/tag-tests are structured
(`kir_lower_match`, `kir_lower_variant`, `kir_lower_raw`), the runtime header
as the single source of truth for the layout the emitter now hardcodes. **Not
touched:** typer, Perceus, representation, FFI. This is the reason it is the
big rock: highest leverage, narrowest blast radius, no Tier-1 friction.

**Tier-1 check:** none violated. The layout is already fixed and the C
backend already depends on it; the native emitter merely stops pretending it
does not know it. Memory-safety is unchanged (same reads, same RC). It *is* a
new coupling — the native emitter now duplicates the layout knowledge that
lives in `runtime.h` — which is the standing argument for a common IR that
owns runtime semantics explicitly (KIR, per the architect's
`dual_backend_kir_position` note). This redesign is the forcing function for
finishing KIR: the layout should be encoded once, in KIR's cell model, and
both emitters lower from it. Doing it twice (C already, native now) is the
smell KIR exists to remove.

### Axis 2 (the one the brief missed) — Uniform-size node vs. per-constructor struct

kaikai's `KaiValue` is **one fixed size for every value**: 8-byte header + 5
inline slots = 48 bytes (`runtime.h:430-503`; the union's other members —
cons, rec, clo, arr, ref — all fit inside the same 48). A `Node(l,r)` and a
`Leaf(v)` and a 5-field record occupy the identical footprint; a variant with
>5 fields spills.

Koka does the opposite. Each constructor is its own C struct, sized exactly
to its fields (`kklib.h:206-242`): `struct Node { kk_tree_s _base; kk_tree_t
left; kk_tree_t right; }` is 3 words including header; `struct Leaf { _base;
kk_box_t value; }` is 2. The datatype pointer is `kk_datatype_t` (`:185`), and
nullary constructors are **not allocated at all** — they are enum singletons
tagged in the word: `kk_datatype_from_tag` is `kk_intf_encode(tag,1)`
(`:1112-1115`), `kk_datatype_is_singleton` is `kk_is_value(d.dbox)` (`:1131`).
So `Nil`, `Empty`, `None`, `Red`/`Black` colour tags — zero heap, zero deref,
same tagged-word trick as ints.

Two costs kaikai pays here that Koka does not:

1. **Allocation and cache footprint.** A 2-word `Leaf` costs kaikai 48 bytes;
   an rb-tree of *n* nodes touches ~2.4× the cache lines Koka touches.
   Reuse-in-place hides the *alloc count* (both hit 1.00/insert) but not the
   *bytes moved and cache pressure* per reused cell — the reuse memsets/copies
   a 48-byte cell where Koka copies 24. This is instructions the counter
   parity does not see.
2. **Enum singletons.** kaikai partly has this — `KAI_VAR_SLOT_ENUM`
   (`runtime.h:424`) stores a nullary ctor as its immediate tag *inside a
   slot*, and the `Enum` slot-kind exists. But a bare nullary value in a
   *register* (not a slot) — `None`, `Nil`, a colour — still goes through the
   variant machinery rather than being a tagged word end to end. Koka's
   `kk_datatype_t` makes singleton-ness a property of the value's bit pattern
   everywhere, uniformly.

**The redesign:** move from one uniform node to **per-constructor layout** —
each monomorphised constructor gets a struct sized to its arity, and nullary
constructors of a sum become tagged-word singletons (generalising the existing
`ENUM` slot-kind to the whole-value level, matching `kk_datatype_from_tag`).

**Why this is Axis 2, not Axis 1.** It is real leverage (footprint,
singleton-free enums) but it is *deeper* and it is *coupled to Perceus and to
the whole codegen*. Perceus's reuse-in-place currently works because every
node is the same size — a dropped `Node` cell can back any freshly-allocated
node (the token pool, the `is_unique`-gated reuse, all assume interchangeable
48-byte cells; see the architect's `multicell_reuse_token_pool_design` and
`variant_slab_allocator_design`). Per-constructor sizing means reuse can only
recycle a dropped cell into a constructor of the *same size class* — which is
in fact what Koka does (its reuse is size-class-matched, `Optimize.hs` +
`kklib` reuse), but it is a non-trivial reshape of the reuse recogniser. This
is why it sits below Axis 1: same-or-higher leverage on *footprint*, but it
drags Perceus, both emitters, and the allocator with it, and it must land
coupled (the architect's `coupled_reshape_lands_together` rule).

**Tier-1 check.** Per-constructor layout is Perceus-compatible *by
construction* — Koka proves it (Perceus was designed against exactly this
representation). It stays memory-safe. The one real constraint is **fiber
isolation** (CLAUDE.md "Memory: Perceus … + isolated fibers BEAM-style,
private heap, messages copied"): messages crossing a `spawn` are deep-copied,
and a per-constructor copy routine must exist per type. kaikai's generic
walker (mask-driven) already does this uniformly; per-constructor means
generating a copy/drop routine per type (which Koka does — the `scan_fsize`
in the header, `kklib.h:270-275`, drives it). No Tier-1 is broken; it is more
generated code, which touches **fast compilation** (Tier 1 #3) — more codegen
per type is more work per compile. That is the hidden cost to weigh: Axis 2
buys runtime footprint at the price of compiler output volume.

### Axis 3 — No source-level inliner before Perceus

Koka runs its inliner **before** Perceus (`Core/Optimize.hs:73-81`, already
established in `#1104`). Small functions — `is_red`, `balance`, `rotate` —
are inlined into their callers, so the RC-insertion pass sees the whole
rotation as one body and the reuse becomes intra-procedural: a `Node` dropped
in `balance` is reused by the `Node` allocated three lines down in the same
inlined body. kaikai has no source-level inliner. `is_red`/`balance` stay
out-of-line, RC is inserted at the call boundary, and the reuse token cannot
flow across the call — which is exactly why `#1104` Lane 1 needed
*interprocedural token donation* to fake it.

**The redesign:** a real pre-Perceus inliner (small-function, cost-model
gated, monomorphised bodies), replacing the token-donation patch. The reuse
recogniser then sees inlined bodies and the interprocedural machinery can be
deleted.

**Why Axis 3, last.** It is genuine leverage — it is *the* reason Koka's
reuse is as tight as it is — but its instruction payoff is *conditional on
Axis 1*. Today, even a perfectly-inlined body emits shim calls for every slot
access in native; inlining `balance` into the descent loop multiplies the
same opaque `kaix_*` calls. Inlining pays off in *instructions* only once the
inlined body lowers to loads/stores (Axis 1). On the C backend, where access
is already inline, Axis 3 *does* pay immediately (tighter reuse, fewer RC
boundary ops) — which is why the C backend is 2.16× and not 3×. So Axis 3 is
the lever that closes the **C-backend** residue to Koka, and it compounds with
Axis 1 for native.

**Fronts touched:** a new pass between monomorph and Perceus in the pipeline
(`lex → parse → resolve → infer → monomorph → **inline** → perceus → lower`);
a cost model; the reuse recogniser (simplified, token-donation removed).
**Tier-1 check:** inlining is standard and decidable; it does not touch the
type system. The cost it pays is **fast compilation** (Tier 1 #3) — an
inliner is compile-time work and code-size growth. Koka accepts this;
kaikai's Tier-1 tie-breaker "fast compilation beats generality" means the
inliner must be *cheap and bounded* (small-function threshold, no
speculative/iterative inlining), not a general optimiser. This is a design
constraint on the pass, not a veto.

## Forced order and the verdict

**Forced order: Axis 1 first, unconditionally.** It is the highest leverage,
the narrowest blast radius, touches no Tier-1, and — critically — Axes 2 and 3
*depend on it for their native payoff*. Per-constructor layout (Axis 2) that
still reads through `kaix_*` shim calls captures footprint but not the access
instructions; a pre-Perceus inliner (Axis 3) that inlines bodies full of shim
calls multiplies opaque calls instead of loads. Both need the emitter to
already know the layout. Axis 1 is the enabling substrate.

After Axis 1, **Axes 2 and 3 are independent of each other** and can proceed
in either order:

- If the goal is **closing the C backend to Koka** (the 2.16× → ~1×), Axis 3
  (inliner) is the lever — the C backend already inlines access, so tighter
  intra-procedural reuse is the remaining C residue.
- If the goal is **runtime footprint / cache / singleton-free enums**, Axis 2
  (per-constructor layout) is the lever, and it is the deeper reshape (drags
  Perceus + allocator + both emitters, must land coupled).

Recommended sequence: **Axis 1 → Axis 3 → Axis 2.** Axis 1 unlocks native;
Axis 3 then closes both backends' *reuse* residue at moderate blast radius;
Axis 2 last because it is the deepest reshape and its marginal leverage
(footprint) is smaller than the access-and-reuse leverage of 1+3.

### Magnitude, honest

This is multi-front and multi-month; the honest count of fronts:

- **Axis 1:** native emitter, KIR lowering, runtime header as layout SoT. 3
  fronts, and it is the forcing function to *finish* KIR (encode the cell
  layout once). Narrowest of the three.
- **Axis 2:** typer/monomorph (per-ctor struct generation), **both** emitters,
  Perceus reuse recogniser, allocator/token pool, the generic drop/copy
  walkers, FFI (a per-ctor layout changes what an `extern` sees when a value
  crosses the boundary — the FFI marshalling in `docs/ffi-v2` assumes the
  uniform node). 6 fronts. The deepest.
- **Axis 3:** one new pass, a cost model, the reuse recogniser. 3 fronts, but
  it interacts with monomorph (inline *after* specialisation) and with Perceus
  (order-sensitive: inline before RC). Medium.

### Is Koka reachable, or is there a structural ceiling?

**Reachable on the C backend; reachable-modulo-a-small-floor on native.** The
gap kaikai pays that Koka does not is real but bounded, and none of it is in
the two features the brief worried about (integer boxing — already solved; the
value representation — already tagged):

- The **6.0× native** gap is almost entirely Axis 1 (opaque access). Closing
  it lands native near the C backend's instruction count. There is a residual
  native floor from the in-process libLLVM path and the shim boundary that LTO
  does not fully erase (the architect's `747_llvm_two_necks` note puts the
  honest floor at ~1.5–2× C for the call-heavy shape). So native → ~1.5× Koka
  is the realistic target, not 1.0×.
- The **2.16× C** gap is Axis 3 (inliner → intra-procedural reuse) plus Axis
  2's footprint. Koka reaches hand-written C; kaikai's C backend, with a
  bounded inliner and per-constructor layout, can reach ~1.1–1.3× Koka. The
  last ~10–20% is the price of the pillars kaikai keeps and Koka does not.

**The structural ceiling kaikai pays and Koka does not** — and *should* keep
paying, because they are Tier-1 pillars, not defects:

1. **BEAM-style fiber isolation** (CLAUDE.md, Memory). Koka has one shared
   heap and thread-shared RC via atomic negative refcounts (`kklib.h:103-116`).
   kaikai has per-fiber private heaps with *copied* messages. The copy on
   `send` across a `spawn` is an instruction cost Koka never pays. On
   single-fiber CPU-bound code (rb-tree) this is *zero* — the ceiling only
   bites concurrent workloads, and there it buys fault isolation Koka lacks.
   Not a defect; a different bet.
2. **Effects as one-shot continuations** (CLAUDE.md, Effects — "effects
   compile to one-shot continuations as the zero-cost default"). Koka's
   evidence-passing and kaikai's hybrid dispatch are comparable; the "one-shot
   default" is the zero-cost path and matches Koka's. No structural ceiling
   here for the common case.
3. **The uniform-repr convenience kaikai would be *shedding* in Axis 2.** The
   48-byte node is not a pillar — it is an implementation choice, and Axis 2
   removes it. So it is not a ceiling; it is debt on the redesign list.

**Verdict.** Koka parity is *not* blocked by the value representation — that
is already Koka-shaped and working. It is blocked by (1) the native emitter
refusing to inline the layout it already knows, (2) the uniform node size, and
(3) the missing pre-Perceus inliner. Attack them in that order. The realistic
end state is **C backend ~1.1–1.3× Koka, native ~1.5× Koka** — with the
remaining gap being the deliberate, Tier-1 price of fiber isolation on
concurrent code (zero on this benchmark) and nothing else. There is no
representation-level wall; there is a modest, well-understood floor from the
pillars kaikai chose to keep.
