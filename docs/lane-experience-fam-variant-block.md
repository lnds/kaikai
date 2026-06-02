# Lane retro — FAM variant block (header + inline slots, Koka mould)

Base: `76a1661` (post enum-as-int, rb-tree 5.84×C wall / 2.30×C RSS).

## Result (rb-tree N=1M, M4 Pro)

| metric | baseline | FAM |
|---|---|---|
| **wall** | ~1.35s (5.84×C / 2.95×C official run) | **~0.74s (2.95×C)** — **~1.8× faster** |
| **RSS** | 2.30×C | **1.83×C** |
| alloc_total / incref / reuse | unchanged | unchanged |
| leaked | 14 | 14 |

The single biggest wall move of the whole rb-tree series: **5.84×C → 2.95×C**,
and RSS dropped too (one block per node instead of two halves the allocator
overhead). FAM is a **runtime-only** change — the emitted codegen is
byte-identical, so selfhost stays byte-identical to `make kaic2`'s own output
(the determinism gate passes; the one codegen tweak below is what changes the
emitted text).

## What FAM is

Each variant node was two heap blocks: a fixed `sizeof(KaiValue)` header
(recycled via the size-uniform `kai_cell_pool`) plus a separately-malloc'd
`slots[]` array (recycled via `kai_slot_pool[n]`, keyed by arity). FAM
collapses them into ONE contiguous block — `sizeof(KaiValue) + n*sizeof(slot)`
— with the payload slots stored in a trailing C99 flexible array member
(`KaiVarSlot inline_slots[]`), and `as.var.slots` pointing at `inline_slots`.

The win was bigger than the "fewer mallocs" framing predicted: the cell-pool +
slot-pool already recycled both blocks (system malloc was rarely hit), so the
~1.8× came from **cache locality** (header and payload share one line, one
pointer chase instead of two) plus **one pool op instead of two** per
construct/free. asu's pre-write warning — "measure variant/total alloc ratio
first, a fattened base struct could be net-negative" — was the right gate:
the rb-tree is 99.9997% variant allocs, so FAM-1 (variant-only `kai_alloc_var`,
non-variant tags untouched at the base size) hits everything that matters and
the FAM trailing member adds 0 bytes to `sizeof(KaiValue)` (C99 §6.7.2.1p18),
so cons/int/str/closure pay nothing.

## Pieces

- `struct KaiValue` gains a trailing `KaiVarSlot inline_slots[]` (FAM, after
  the `as` union — 0 bytes to sizeof).
- `kai_alloc_var(tag, n)` allocates `sizeof(KaiValue) + n*slot`, drawn from a
  new per-arity `kai_var_block_pool[MAXN+1][CAP]` (replaces cell_pool+slot_pool
  for variants). Non-variant tags keep `kai_alloc` + the size-uniform cell_pool.
- `kai_variant_u` cold + immortal-args paths use `kai_alloc_var` and set
  `slots = inline_slots`. Nullary path unchanged (no slots; stays on cell_pool,
  interned). Arena (`region {}`) ctor unchanged (its own bump model).
- `kai_free_value` VARIANT case drops the `kai_slots_free` (no separate array);
  the recycle step routes a VARIANT block to `kai_var_block_free(v, n_args)`
  (its real-arity pool) and every other tag to the cell_pool.
- `kai_reuse_free` (unconsumed reuse-token): must return the block to its
  REAL-arity pool, not pool[0]. It now sets `slot_mask` to all-INT (so the
  decref loop sees no PTR slots → no cascade) while KEEPING `n_args`, so the
  block recycles to the right pool. (First attempt zeroed `n_args` → polluted
  pool[0]; second duplicated the trace tail and used trace-only counters
  unguarded → compile error. The mask trick is the clean one.)

## Soundness — why reuse-in-place is FAM-safe

The three reuse recognisers (`kai_variant_at`, `kai_variant_reuse_at`,
`kai_reuse_or_alloc_variant`) all gate on `n_args == n` (same arity) before
rewriting a donated block's slots. A reused block is therefore always the right
byte size for its rebuild — FAM never rewrites a node to an arity its inline
storage can't hold. asu confirmed this invariant holds at every reuse site; no
cross-arity in-place rewrite exists. ASAN clean on rb-tree (the heaviest reuse
path) + gap3 + the perceus fixtures.

## The one codegen change (scalar-wildcard elision)

After FAM removed the malloc bottleneck, the `is_red` hot path's leftover
`kai_decref(kai_int(slot[i].i64))` for its wildcard k/v fields (immediate under
tagged-Int, but two calls × ~9M) became visible. `emit_pat_binds_variant` now
elides the slot read entirely when a scalar slot's sub-pattern binds nothing
(`pat_binds_nothing`: PWild or `_`-prefixed PBind) — an unboxed scalar has no RC
to release. Marginal on wall (the calls were already near-free) but a correct,
general codegen cleanup. This is the only emitted-text change; selfhost stays
deterministic (kaic2b.c == kaic2c.c).

## Cost vs estimate

asu split FAM into FAM-1 (slots-ptr kept, byte-identical, cheap gate) and FAM-2
(packed header, breaks byte-id, the deeper cache lever). FAM-1 ALONE delivered
~1.8× — far above the "modest" expectation — so FAM-2 was not needed to bank a
large win. The slots-ptr is now redundant (always == inline_slots) but harmless;
removing it + packing rc/tag is the FAM-2 follow-up.

## Follow-ups (toward 1.0×C)

1. **RC traffic is now the bottleneck.** Post-FAM the profile is `insert_loop` +
   `is_red` (pure tree logic over boxed `KaiValue*`), with 48 RC ops/insert
   (22.7M incref + 25.0M decref). malloc is off the profile top. Koka's 0.28×C
   comes from borrow inference eliminating most of those on the unique descent.
   The next lever is **wider borrow** (the #599 / borrow-inference-design lane,
   partially shipped in #741) — reduce the 11 `kai_internal_dup` in insert_loop.
2. **FAM-2**: drop the redundant `slots` ptr (`slots = (KaiVarSlot*)(v+1)` via a
   macro), drop `variant_name` ptr (tag→name table), pack rc+tag. Breaks
   byte-id (touches the emitter); gated on selfhost determinism + wall.
3. Force-inlining `is_red` was tried (force `always_inline` in the generated C)
   and did NOT help — the 139 samples are the node-read work, not call overhead.
   Inlining is not the lever; RC reduction is.
