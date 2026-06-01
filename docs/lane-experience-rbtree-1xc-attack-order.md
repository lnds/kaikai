# Lane experience: rb-tree toward 1×C — reuse soundness + TRMC plan (2026-05-31)

Goal: take the canonical Perceus rb-tree (1M inserts) from ~8.9×C toward ~1×C,
learning from Koka (same algorithm at 0.28×C — 3.6× FASTER than an intrusive
malloc-per-node C reference, because it reuses cells in-place).

## Measured baseline (1M inserts)

| impl              | time    | ratio vs C |
|-------------------|---------|------------|
| C intrusive       | ~0.25s  | 1.0×       |
| Koka (FBIP)       | ~0.069s | 0.28×      |
| kaikai            | ~2.2s   | ~8.9×      |

100k-insert RC trace (kaikai monolithic): `alloc_total≈7.0M` (~70 allocs/insert).

## Koka's three architectural advantages (from emitted-C diff)

1. **Specialized struct per ctor.** `struct kk_rbtree_Node` is ONE block:
   `lchild`/`rchild` pointers, `key` int32 inline, `value` bool inline,
   scan-count=2. ONE malloc/node. kaikai uses a uniform `KaiValue` + a SEPARATE
   `slots` array malloc = TWO mallocs/node.
2. **TRMC** (Tail Recursion Modulo Cons). `ins` spine-rebuild becomes a
   `goto kk__tailcall` loop with a `cctx` constructor-context: it builds the
   Node with a hole, reuses the `t` cell, records the hole address
   (`kk_field_addr_create`), continues. O(1) stack + perfect reuse.
3. **Reuse token.** `kk_datatype_ptr_reuse` extracts the cell ptr; the ctor
   writes in-place. ~0 new spine allocs.

## What this lane SHIPPED (correctness fixes, not the perf lever)

Two genuine correctness bugs in kaikai's existing reuse-in-place recogniser,
both surfaced while porting the Koka-style rb-tree:

1. **Nested sub-pattern → invalid C.** `pcs_try_reuse_variant` recognised an arm
   whose pattern has a NESTED variant sub-pattern (`Node(_, Node(Red,a,ak,b), …)`)
   as 1:1-reusable, but `emit_match_arm_reuse_variant` only binds FLAT slots
   (PBind/PWild) — the inner vars (`a`,`ak`,`b`) were referenced but never
   declared, producing C that fails to compile. Fix: `pcs_all_flat_subs` gates
   the path; nested-pattern arms fall through to the fresh allocator.
   Fixture: `examples/perceus/reuse_nested_subpattern.kai`.

2. **Non-diagonal rebuild → leak.** The shape predicate (mangle_ty) passed any
   same-TyCon rebuild, but the 1:1 lowering overwrites slots WITHOUT dropping the
   old pointer. A rebuild that puts a FRESH node (or another slot's binder) into
   a pointer slot orphans the old child → leak. Fix: `pcs_reuse_is_diagonal`
   requires every boxed slot to be exactly its own binder (unboxed Int/Real
   slots are copy-by-value, any derived expr is safe). Fixture:
   `examples/perceus/reuse_diagonal_guard.kai` (ASAN leak-clean).

Both gated: selfhost byte-identical, tier0 green, ASAN clean.

**Landmine recorded:** any new binder in perceus.kai must NOT be named `args` —
it shadows the prelude `args()` fn and silently resolves to an empty list
(stage1 shadow bug). Cost me an hour: `pcs_ctor_args` returned `Some([])` for a
4-arg ctor. Renamed to `cargs`.

## Why the fixes don't move the perf needle (verified)

The diagonal guard is sound but slightly conservative: it rejects the
non-bijective `balance` rotations, which were leak-free in THIS workload (every
child is used exactly once → linear). asu's verdict: the correct predicate is
AFFINE-LINEARITY (each boxed pattern-binder used exactly once in the new-args),
not strict diagonality — `pcs_collect_uses_expr` (perceus.kai:818) already
computes the use-set. But refining diagonal→linear recovers only `balance`'s
reuse, which does NOT approach 1×C: the bulk of the 7M allocs is `insert_loop`
rebuilding the spine through `balance(c, insert_loop(l,…), …)`, where the
replacement constructor is HIDDEN inside `balance` — unreachable by any
syntactic reuse recogniser.

Empirical proof the mechanism works when the ctor IS visible: a recursive
spine rebuild `Node(incr_left(l), k+1, r)` reuses 100% (20M reuses / 0 misses).
The rb-tree's `balance(…)` call is the only thing in the way.

## NEXT: TRMC is the lever (asu + Explore convergent verdict)

TRMC makes `insert_loop` reuse the `t` cell it consumes, feeding it to the node
`balance` produces — exactly Koka's transform. It is load-bearing GENERAL (map/
fold/parsers/the self-hosted compiler), not rb-tree-specific. Plan:

- Pipeline slot: extend the reuse recogniser in `perceus.kai`; lower reusing the
  `tcrec` goto-loop infra (`emit_shared.kai:2970+`, runs post-perceus in
  `driver.kai:5133` for both backends).
- The `kai_check_unique` rc==1 guard makes it on-by-default safe (OCaml's TRMC
  was opt-in for lack of this).
- Fold the diagonal→affine-linearity refinement into the SAME lane, over the
  final TRMC node shape (avoids double churn).
- Gate: `reuse` rises to ~log(n)/insert, `alloc_total` 7M→~200k, `leaked==0`,
  ASAN clean, selfhost byte-identical.
