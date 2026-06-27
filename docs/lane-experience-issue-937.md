# Lane experience — issue #937: O(N²) `list_append`-in-a-loop on the default compile path + two list-op traps

## Scope as planned vs as shipped

Planned: fix three program-scale O(N²) accumulators on the default
compile path (kir_lower_fns, protos rename, monomorph spec-dedup) plus
two stdlib list-op traps (`windows`, `uniq`). All five shipped.

Shipped exactly as planned in mechanism, with two corrections the brief
did not anticipate (both surfaced by measurement, see below):

1. **`kir_lower_fns.lower_fns`** — prepend + `list_reverse` once at the
   `[]` base. One-line change; mirrors the C-emitter's `fn_bodies_loop`.
2. **`protos.rename_proto_calls_decls_mo_loop`** — prepend + reverse
   once; `r.cache` (ShadowByMod) threading untouched.
3. **`monomorph` spec-dedup** — the `tuple_in` linear membership scan
   over the monotonically growing spec set replaced with a hash index.
4. **`list.windows`** — thread a decrementing remaining-count `Int`
   instead of re-walking `length(xs)` per step.
5. **`hashset.uniq[T: Hash]`** — new O(n) order-preserving dedup, the
   fast path beside the Eq-only `list.uniq`.

## Each rewrite + how byte-id was preserved

### #1, #2 — prepend + reverse-once

Both loops built their result with `list_append(acc, [x])` per element,
copying the whole `acc` spine each step (∑k = O(N²)). The fix prepends
`[x, ...acc]` (O(1)) and reverses once at the base. Order is preserved
because the consumer walks the list in order and the only ordering
operation is the single final `list_reverse`. The `_mo` variant's
non-`_mo` sibling already used `map` (order-preserving), so the two now
agree.

### #3 — monomorph spec-dedup index

The hot scan was `tuple_in(existing, mt)` in `discover_call_tuple`, run
once per polymorphic call-site in *every* spec body against the full
`all_tuples` set — O(M²·t) where M is the spec count and t the per-tuple
structural-compare cost. Two more scans (`rewrite_bare_call`,
`rewrite_qualified_call`) added O(M²) on the rewrite walker.

**Design — reuse, don't reinvent.** The brief suggested "the same
engineering as `NameIndex`/`ImplIndex`". On inspection the existing
generic `NameIndex[a]` (`infer_index.kai`, #914) is *exactly* the right
structure — a hash-bucketed AVL whose consumers re-apply the exact
predicate over the returned chain. So no new index file was authored;
`monomorph` imports `infer_index` and instantiates `NameIndex[MonoTuple]`
inside a thin `TupleSet { items, idx }` wrapper. The list field is kept
only because `generate_specs_iter` returns it (the caller discards it);
all membership now goes through the index.

**Key design (the byte-id-critical part).** The index key is
`mt_mo_key(mo) ++ "|" ++ mono_mangle_name(name, tys, units)`. The mangle
is *lossy* (it drops a `TyCon`'s module slot, which `tuple_eq` compares
via `module_slot_compat`), so the index is a **candidate filter, not the
decision**: `ts_mem` re-runs the exact `tuple_in`/`tuple_eq` scan over
the bucket chain. Soundness rests on one implication —
`tuple_eq(a,b) ⟹ tuple_key(a) == tuple_key(b)` — which holds because the
mangle only ever conflates *more* than `tuple_eq` (it never distinguishes
what `tuple_eq` calls equal): equal `mo` ⟹ equal `mt_mo_key`; `ty_eq`
finer-than-mangle on the types; `unit_eq` and `mono_mangle_unit` share
the same canonical-unit display. No false negatives ⟹ no missed member
⟹ identical membership ⟹ byte-identical output.

The conversion threaded `TupleSet` through the discovery walker
(`existing: TupleSet`) and the rewrite walker (`tuples: TupleSet`) —
read-only in both, touched only at the three leaf `tuple_in` sites, which
became `ts_mem`. The growth boundaries (`initial_ts`, `emit_spec`'s
`updated_tuples`, `generate_specs_iter`'s `next_all`) build the index
incrementally with `ts_add_all` (O(new) per boundary).

### #4 — `windows`

`windows_loop` called `length(xs)` per recursive step purely to test "are
there still ≥ n elements left", which is O(remaining) each step → O(L²)
on top of the intended O(n·(L−n+1)). Now `length` is computed once up
front and a decrementing `rem: Int` is threaded; the stop test is
`rem < n`. Results identical (verified against the prior behaviour in the
regression fixture: count, first/last window, edge cases).

### #5 — `hashset.uniq[T: Hash]`

The Eq-only `list.uniq` linear-scans `seen` per element → O(n²). The new
`hashset.uniq` builds a working `HashSet` and emits each element on first
sight, preserving first-occurrence order, in O(n) amortized. It lives in
`hashset.kai` (not `core/list.kai`): `core/*` is the bootstrap-safe,
kaikai-minimal layer that `make selfhost` compiles with `core/*` as the
*only* prelude — it has no protocols, no effects, and cannot import
`collections`, so a `[T: Hash]` `/ Mutable` function cannot live there.
`list.uniq`'s doc now points at the fast path. The existing Eq-only
`uniq` signature is untouched (additive, no breaking change).

## Structural surprises the brief did not anticipate

1. **Self-compile throughput is flat, and that is correct.** The brief
   framed #3 as "the biggest win" measured by self-compile throughput,
   with an explicit "investigate if flat" gate. It *is* flat — because
   the compiler's own source emits only **23** `__mono__` specs
   (it is written in concrete-typed + protocol-dispatch style, not
   user-generic style), so M≈23 and O(M²) is noise. #1 (`kir_lower_fns`)
   is on the **native** KIR path, which the C-direct self-compile does
   not exercise at all. So a flat C-direct self-compile is the *expected*
   result, not a regression: the no-regression gate (38.21s → 38.4s
   best-of-3, within noise) holds. The win is real but lives on
   monomorph-heavy *user* programs, proven below.

2. **Multi-module same-name mono collision (pre-existing, #893 family).**
   A program calling *both* `list.uniq` and `hashset.uniq` at `Int`
   panicked `non-exhaustive match` — the two `uniq_loop` helpers
   collided in the mono-tuple dedup (same `(name, tys)`, mo-tagging
   collapsed them). Reproduced on the **baseline** binary, so it is
   pre-existing, not introduced by the index. Sidestepped by renaming
   the hashset helper `uniq_loop` → `uniq_seen_loop` (unique name). The
   public `hashset.uniq` API is unchanged. (The deeper fix belongs to the
   #893/#898 multi-module-collision lane, out of scope here.)

3. **Anonymous-record literal needs the type prefix.** `TupleSet { ... }`,
   not bare `{ ... }` — a bare brace after `=` parses as a block. Caught
   immediately by the bundle parser.

## Throughput before/after

Self-compile (C-direct, best-of-3 wall, the brief's benchmark):

| build    | wall  | emitted C | specs |
|----------|-------|-----------|-------|
| baseline | 38.21s| 117187 L  | 23    |
| after    | 38.4s | 117287 L  | 23    |

Flat (within noise) — expected, M≈23 (see surprise #1). No regression.

Monomorph-heavy synthetic (N distinct record types through a `[T]`
generic chain; baseline binary vs new binary, byte-identical output):

| N    | specs  | baseline | after  | speedup |
|------|--------|----------|--------|---------|
| 600  | 10226  | 1.83s    | 1.47s  | 1.24×   |
| 1200 | 20426  | 6.95s    | 5.46s  | 1.27×   |
| 2000 | 34026  | 21.28s   | 17.00s | 1.25×   |

The absolute time removed grows with M (0.36s → 4.3s), confirming a
super-linear term was eliminated. Output byte-identical at every scale.

## Byte-id correctness gate

- `make selfhost`: stage1 + stage2 determinism OK (kaic2b.c == kaic2c.c).
- Same-input identity: the new binary and the baseline binary compile
  **331 stdlib + effects fixtures byte-identically (0 diffs)**, plus the
  monomorph-heavy synthetics above. Same input ⟹ same output: the index
  selects exactly the membership the linear scan did.

## Fixtures added

`examples/stdlib/list_windows_uniq_scale.kai` (+ `.out.expected`) —
`windows` on 5000 elements (4998 windows, endpoint checks, n>L and n≤0
edges) and `list.uniq` vs `hashset.uniq` on a 5000→1000 dup-heavy input
asserting element-for-element agreement and first-occurrence order.
Auto-globbed by `make test-stdlib`; in the `examples/stdlib` parity-walk
so it runs native-vs-C. Passes under both backends.

Coverage gap: the compiler O(N²) fixes are covered by selfhost byte-id
(the compiler is the large input) and the monomorph-heavy synthetics, not
by a checked-in fixture — the synthetic generator is throwaway, not a
golden, because its size is the point and a golden would be brittle.

## Follow-ups left for next lanes

- The #893/#898 multi-module same-name `[T:P]` mono collision is still
  open; this lane only sidestepped one instance by renaming a helper.
- A native self-compile throughput harness would let #1's win be measured
  directly rather than argued from the C-emitter precedent.
