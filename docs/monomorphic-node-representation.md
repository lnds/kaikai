# The path to 1.0×C: monomorphic node representation

> **Status (2026-06-02):** design note / not started. Documents the
> structural redesign required to reach C-parity on the Okasaki rb-tree
> (and pointer-heavy functional code generally), after the uniform-
> representation levers were exhausted and measured to a ~2.85–2.96×C
> floor. Tier 1 #2 ("runtime-efficient") path. Not an incremental lever —
> a multi-week design lane in its own right.

## Where we are (and how we got the floor)

The rb-tree bench (1M random inserts, M4 Pro, vs `clang -O2` intrusive C)
moved across one session from **5.84×C → 2.96×C** wall, **2.30×C → 1.83×C**
RSS, via three shipped, sound, selfhost-deterministic lanes:

| lane | commit | effect |
|---|---|---|
| enum-as-int slot (`Color` as immediate tag in a ctor slot) | `76a1661` | deref-only, ~marginal here, general |
| **FAM variant block** (header + inline slots, one allocation) | `85a9182` | **the big one: ~1.8× (cache locality + one pool op)** |
| direct Int order-comparison (skip boxed `kai_op_lt/gt`) | `0cec095` | ~3%, general |

Two further levers were prototyped and **measured to be inert on wall**,
which is what pins the floor:

1. **Borrow inference / RC reduction.** A no-op-RC experiment (compile the
   bench with `kai_incref`→return-arg, `kai_decref`→no-op) left the wall
   *identical* (~0.75s) and the profile *identical*. RC traffic
   (48 ops/insert) is not on the wall critical path — incref/decref on
   pointers are a branch + an `rc++` on hot memory, ~free. Borrow would be
   sound but inert, exactly like the balance reuse-token before it.

2. **FAM-2 (slim the node header).** Removing `variant_name` (8 B) from the
   variant substruct + a `tag→name` table built clean through the whole
   bootstrap — and left `sizeof(KaiValue)` **unchanged at 40**,
   `offsetof(inline_slots)` **unchanged at 40**, wall **unchanged**. The
   reason is structural: the value `union as` is 32 B, and its size is set
   by `rec` (record: `n_fields + fields + names + head_type_tag`) / `clo` /
   `arr` — **not** by `var`. Shrinking `var` does not shrink the union, so
   it does not shrink the variant FAM block (`sizeof(KaiValue) + n·slot`).
   Reverted (inert, breaks byte-identity, touches all three stages).

The conclusion is empirical, not opinion: **the uniform `KaiValue`
representation has a ~2–3×C floor on this workload.** A kaikai `RBNode` is
a 40 B generic header (whose size a recursive variant cannot reduce while
it shares the `union as` with records/closures/arrays) + `n·8` inline
slots; C's node is a flat `struct { Color color; int key, val; RBNode
*l, *r; }` with fields in registers and `is_red` = one load + compare.

## Why C wins, precisely

| | kaikai (uniform) | C (monomorphic) |
|---|---|---|
| node | 40 B generic header + inline slots; metadata (`variant_tag`, `n_args`, `slot_mask`) C doesn't carry | flat struct, exactly its fields |
| field read | `v->as.var.slots[i].ptr` through the generic header | `node->left`, a fixed offset |
| `is_red` | `v->tag==KAI_VARIANT && v->slots[0].i64==11` (≈3 loads/compares) | `node->color==RED` (1 load) |
| `match` | test-cascade extracting slots | `switch` on a 1-word tag |

Koka reaches 0.28×C (faster than intrusive C) because its block *is* the
flat node — header packs rc+tag+scan in one word, fields inline, match
lowers to a switch, scalars are immediate. kaikai got the "one allocation,
inline payload" half via FAM; the remaining gap is the **generic header +
generic match**, which the shared `union` representation cannot shed.

## The redesign

**Monomorphic node representation: a flat C struct per concrete sum type,
emitted post-monomorphization, taken OUT of the shared `union as`.**

Sketch (not a spec — the spec is the lane's first deliverable):

- After monomorph, each concrete sum type `T` with constructors
  `C0..Ck` gets an emitted C `struct Kai_T { int32_t rc; int32_t tag;
  <fields of the widest ctor, or a per-ctor union> }`. Nullary-only sums
  are already handled (enum-as-int); this is for payload-carrying sums.
- Codegen reads fields by name/offset (`node->left`), not
  `v->as.var.slots[i]`. `match` lowers to `switch (node->tag)`.
- RC stays (Perceus is orthogonal): `rc`/`tag` live in the flat header;
  `kai_incref`/`decref` dispatch on a shared header prefix (the first two
  words are layout-compatible across all `Kai_*` structs, so the generic
  RC walker can still drop children by reading a per-type child-offset
  table — or codegen emits a per-type `kai_drop_T`).
- The FAM block pool generalizes to a per-(type,ctor) size class.

### The hard parts (why it's multi-week, not a lever)

1. **Generic runtime paths.** `kai_to_string`, `kai_op_eq`, `kai_deep_copy`,
   the immortal/nullary caches, the reuse recognisers, the region arena —
   all walk `as.var.slots` generically today. Each needs a per-type path or
   a layout-compatible header prefix + child-offset table.
2. **Bootstrap.** The representation is the runtime ABI; stage0/stage1/
   stage2 must agree. stage1 emits variant access too (the FAM-2 attempt
   already showed a single field removal touches all three stages).
3. **Perceus reuse soundness.** Same-arity reuse becomes same-(type,ctor)
   reuse; the token machinery must key on the concrete layout.
4. **Byte-identity is lost** (codegen changes); the gate becomes selfhost
   determinism + the full RC/ASAN/leak suite, per concrete type.
5. **The win must be proven incrementally.** Start with a single
   hand-monomorphized type (RBTree) behind a flag, measure, then generalize
   — the same prototype-first discipline that caught borrow and FAM-2 as
   inert before they cost a lane.

### Acceptance

rb-tree `kaikai/C ≤ 1.7×` (Hanga Roa DoD #4 stretch) on the same bench,
selfhost deterministic, ASAN clean, `leaked == 0`, tier1 green. 1.0×C or
better (Koka territory) is the aspirational target the redesign unlocks.

## Pointers

- Session retros: `docs/lane-experience-fam-variant-block.md`,
  `docs/lane-experience-enum-as-int-slot.md`.
- The uniform-rep struct: `stage2/runtime.h` `struct KaiValue` (the
  `union as`, the FAM `inline_slots[]`).
- Bench: `examples/perceus/rb_tree_bench.kai` + `rb_tree_bench_c.c`.
