# Lane experience — issue #1180: lazy ranges as a runtime representation

## Scope as planned vs as shipped

Planned (brief): add a `KAI_RANGE` tag mirroring the `KAI_VEC` precedent —
a `{from, to, step}` generator node under the `[Int]` type — make the
sequence consumers iterate it lazily, provide a safe materialising
fallback for cons-only paths, and keep RC trivial.

Shipped exactly that, plus two things the brief did not anticipate:

1. **Zero-stage terminal fusion.** The brief assumed `sum` was a C runtime
   loop ("sum_loop, kai_prelude_reduce…"). It is not: `sum`/`foldl`/
   `product`/`length` are kaikai match-loops in `stdlib/core/list.kai`,
   monomorphised per element type. A match loop over a lazy range peels
   one range node per element (O(1) live memory, but O(n) pooled allocs) —
   correct, yet short of the "0 allocs for the range" acceptance for
   `[1..n] |> sum()`. The existing #1134 terminal-fold machinery closed
   the gap: `pipe_fusion_as_terminal_fold` now accepts a bare `ERange`
   head (zero stages, identity composed automatically by the existing
   stage composer) and — a second pre-existing gap — the `|> sum()`
   zero-arg **call** spelling, which was never recognised (only bare
   `|> sum`), so even staged chains ending in `sum()` had been missing
   the fused path.

2. **The stage0 runtime was left untouched.** The brief said "add the tag
   in runtime.h"; there are TWO runtime.h files. The compiler bundle
   (BUNDLE_SRCS) contains no range literal, so no KAI_RANGE value can
   exist while the bootstrap (kaic1-emitted, old match lowering) compiler
   runs. Changing only `stage2/runtime.h` keeps the entire stage0/stage1
   world byte-identical and removes the bootstrap-miscompile risk class
   entirely.

## Design decisions

- **One-cell in-place normalisation (`kai_seq_norm`)** instead of
  whole-list forcing at borders. A `KAI_RANGE` node rewrites in place to
  ONE `KAI_CONS` cell (head = minted Int, tail = fresh range node), or to
  `KAI_NIL` when exhausted. Because the rewrite changes representation
  and never value, it is sound under arbitrary sharing — every alias
  still sees the same list, and rc/identity are untouched. A match walk
  over a 50M range therefore holds O(1) memory at all times; nothing ever
  materialises as a whole. Full forcing was rejected because a shared
  range pattern-matched once would have re-created the 50M-cell list the
  lane exists to kill.

- **Border placement.** C backend: the emitted list-pattern tests
  (`emit_pat_test_list`/`_loop`) wrap the scrutinee in `kai_seq_norm(v) &&
  v->tag == …` — norm is NULL-preserving so the guard shape is unchanged,
  and it runs per nesting level, so `[a, b, ...t]` and nested list heads
  are covered by construction. Native backend: the KIR list decision tree
  discriminates exclusively through `kaix_is_cons_b`/`kaix_is_nil_b`
  (kir_lower_match), so norming inside those two prims (plus
  `kaix_cons_head/tail` defensively) covers every KProj/KProjBorrow slot
  read that follows. No KIR lowering change was needed at all.

- **A generic cursor (`KaiSeqIt`)** for the C prelude walkers. It walks
  cons cells first and switches to arithmetic generation at a range node,
  copying `{from,to,step}` into the cursor — so a closure that norms the
  same shared range mid-iteration cannot disturb an in-flight walk. All
  hot prelude consumers (reduce, map, filter, flat_map, each, vec-from-
  list) plus eq and show ride it; elements are minted as tagged-immediate
  Ints, so the range phase allocates nothing.

- **`reverse` of a pure range is O(1)**: `{last, from, -step}` where
  `last` is the last actually-generated element. `length` of a range is
  arithmetic. Both use unsigned span math so INT64 extremes don't UB.

- **`list_append` went iterative** (forward build through a tail slot)
  as a by-product: the old per-cell recursion would have needed a
  separate range path anyway, and it overflowed the C stack on deep
  spines.

## Alternatives considered

- **Range as a type** — forbidden by the brief, and rightly: every
  `[T]` signature in stdlib would have needed a duplicate.
- **Destructive peel when rc==1 at match borders** (zero-alloc match
  loops without fusion). Rejected: rc==1 does not imply the scrutinee is
  dead (borrowed scrutinees, as-patterns), and value-changing mutation
  needs the Perceus dead+unique analysis, not a runtime rc check. The
  reuse machinery could power this later (noted for #1181).
- **Prelude-routing stdlib `sum`** through `reduce`: regresses the cons
  case (monomorphised static-dispatch loop with raw accumulator becomes a
  closure call per element).

## Structural surprises

- `kai_op_eq` compares tags before structure, so `[1..3] == [1, 2, 3]`
  would have been silently `false` — the kind of wrongness byte-id
  selfhost cannot catch. The seq-aware early path in `kai_op_eq` (cursor
  walk, no allocation, no mutation) covers range-vs-cons in both
  directions.
- `kai_deep_copy_out`'s default arm is `kai_incref` — sharing a range
  node across a fiber/region copy-out boundary would let a later norm
  mutate across heaps. Ranges now deep-copy by value (three ints).
- Byte/String/Fiber list walkers (net send, argv build, Spawn.select,
  the property-test shrinkers, string concat-all) walk `.cons` guarded
  by `tag == KAI_CONS` — unreachable by ranges ([Int]-only) or safely
  degrading (shrinkers return "no shrink"). Audited, left unchanged.
- The trace report's `kai_rc_alloc_by_tag[16]` cannot show tags ≥ 16
  (VEC, FOREIGN, and now RANGE share this blindness). Range allocations
  still count in `alloc_total`, which is what the fixture gates.

## Fixtures

- `examples/perceus/range_lazy_1180.kai` — a range value through every
  consumer shape: match fallback (single and multi-element patterns, the
  recursive cons-only walk), steps `[1..10..2]`, descending `[10..1..-1]`,
  empty `[5..1]`, reverse/append/length/map/filter, `==` against a cons
  list, show, and a 1M reduce. Gated on the C oracle, native, and
  ASAN+UBSan (`test-perceus-1180-range-lazy{,-native,-asan}`).
- `examples/perceus/range_step_zero_1180.kai` — `[1..10..0]` traps with
  "zero step in range" (checked inside the range-lazy target).
- `examples/perceus/range_lazy_sum_1180.kai` — the perf gate:
  `[1..1M] |> sum() / reduce / length` and a step variant at
  `alloc_total < 100` on both backends
  (`test-perceus-1180-range-sum{,-native}`).

## Measured results (macOS arm64, native backend, best-of-5)

| shape (N = 50M)        | before | after  | ratio |
|------------------------|--------|--------|-------|
| `[1..N] \|> sum()`     | 1.10 s | 0.085 s | 13.0x |
| `[1..N] \|> reduce(+)` | 1.17 s | 0.20 s | 5.9x  |

Allocations for `[1..1M] |> sum() + reduce`: ~2,000,008 → 10.
`live_peak` for the unfused match walk over a 1M range: 9 nodes.

## Follow-ups left

- #1181 (noted in the brief): the eager `range_map_foldl` vs
  `range_step_map_foldl` duplication in stdlib is now cosmetic — both
  are loop-shaped and allocation-free — but they could share one
  stepped implementation.
- Destructive peel at match borders via the reuse/dead-scrutinee
  analysis would make *unfused* match loops over ranges allocation-free
  as well (today: O(1) live, O(n) pooled).
- `stage0/runtime.h`'s `kai_range` still materialises (harmless: only
  stage0/1-compiled code, which contains no ranges, links it). Unifying
  the two runtimes is existing debt, not widened by this lane.

## Cost vs estimate

Single session. The brief's runtime-consumer inventory was the only
mis-estimate (sum/foldl are stdlib kaikai, not C) — it converted into
the fusion extension rather than extra runtime surface.
