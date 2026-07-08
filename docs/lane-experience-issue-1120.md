# Lane experience — issue #1120: borrow annotations on read-only Array prims

## Scope as planned vs. as shipped

**Planned (issue #1120):** mark `array_get` / `array_length` as borrowing
their container, add a runtime variant that skips the container decref,
and elide the adjacent dup/drop of the element when consumed in-place.
Target: the `Array[Int]` bulk-read gap from ~9× Koka down toward it, by
removing the ~2 rc-ops-per-access container tax.

**Shipped:** the container half in full. The per-access container
incref+decref is eliminated on both backends (C oracle + native) from a
single Perceus rewrite that both strips the caller dup and renames the
call to a `_borrow` runtime variant. The element incref (step 3 of the
proposal) was measured to already be handled: Perceus cancels the
element dup/drop when the value is consumed in the same expression
(`acc + a[i]`), so the element incref that remains is the value genuinely
escaping the array — ownership the caller must hold, which the boxing gap
(a separate lane) governs. No element-side change was needed or made.

## The cause, verified first (the brief warned it might be inverted)

Method from the issue: slope N=1,2,3 with `KAI_TRACE_RC` on a
tail-recursive `a[i]` sum, native backend.

| shape | before (main) | after |
|---|---|---|
| thread array, no read (N=0) | 0 incref / 0 decref | 0 / 0 |
| `a[i]` per access | +1 container incref, +1 container decref | **0 / 0** |
| sum 10M `Array[Int]`, `incref_total`/`decref_total` | 20 000 000 / 20 000 000 | **10 000 000 / 1** |

The `decref_total=1` after is the final result string; the residual
`incref_total=10M` is the boxed element escaping the array (a `1` boxed
by `array_make`). The container per-access traffic — the entire target —
is gone. N=0 costing 0/0 confirmed the issue's claim that threading the
array is already free; the tax was purely the per-access borrow.

Wall (native, sum 10M): 0.08 s → 0.07 s. C: 0.05 s. Marginal because the
workload is memory-bound and native was already fast; the issue's 0.18 s
was C on 0.99.0. The RC-op collapse is the honest headline, not the wall.

## Design decisions and alternatives

**Where the borrow lives.** Perceus already has a full interprocedural
borrow system keyed by callee-name + param position (`bmap`,
`pcs_strip_borrow_args`, the `pcs_max_paths_b_*` discount). The prims
join that machinery: `perceus_pass` seeds the borrow map with
`array_get`→[0] / `array_length`→[0], but **only where no user fn shadows
the name** (asu's soundness catch — a user `fn array_get` owns its arg;
borrowing it would be a UAF in user code). The same non-shadowed gate
drives both the caller-dup strip and the callee rename, so a
strip-without-rename (double free) or rename-without-strip (leak) is
structurally impossible.

**Runtime variant, not a mutated original.** `kai_prelude_array_get`
still consumes (other call shapes need it); `kai_prelude_array_get_borrow`
skips only the container decref (the index is still consumed, the element
still escapes with +1). Both runtime.h copies (stage0 + stage2) plus the
`kaix_` native shim carry it, so C and native inherit the borrow from the
same KIR.

**The borrow-only container stays consuming (the real design pivot).**
The classic borrow rule is "no dup, drop after the call". For a container
whose *only* reads are borrowed slots and which is never re-threaded to a
consumer (`fn get_at(a) = if ... else a[i]`), there is no later use to
keep it alive, so its ref would leak — and the drop-after-call wrap resets
a raw sibling param's unbox scope in the C emitter (the `kair_v` vs
`kai_v` mismatch). Rather than fight the wrap, such a container keeps ALL
its accesses consuming — identical to pre-borrow behaviour, the runtime
decref reclaims it. The borrow WIN is the loop shape, where the array is
re-threaded through the consuming recursive call, so `total > borrowed`
and the container is *not* borrow-only. This is `pcs_collect_borrow_last_set`
(the `total>0 && borrowed==0` predicate) + the `keep_consuming` gate in
the strip.

## Structural surprises the brief did not anticipate

1. **The unbox classifier re-scans post-perceus.** `is_mutable_intrinsic_name`
   (fnreg.kai) lists `array_get`/`array_length` so a fn using them is
   classified effectful → boxed signature. After perceus renames to
   `array_get_borrow`, a re-scan saw a non-effectful body and flipped the
   signature to raw (`int64_t kair_v`) while the body still emitted boxed
   `kai_v` — a hard C-compile error that only surfaced at selfhost. Fix:
   add the borrow variants to `is_mutable_intrinsic_name`. This was the
   single subtlest failure; native hid it (native was already correct),
   only the C oracle + selfhost byte-id caught it.

2. **A preexisting C-backend bug on raw TCO-loop indices.** `array.to_list`
   already did **not** compile under the C backend on `origin/main` (same
   `kair_i` vs `kai_i` shape). The borrow lane did not introduce it and
   does not fix it (out of lane); the `is_mutable_intrinsic_name` fix keeps
   the borrow variants on the same boxed-signature path as the originals,
   so the lane neither widens nor narrows that preexisting gap.

## Fixtures added

- `examples/perceus/array_borrow_read_1120.kai` (+`.out.expected`) — the
  `Array[Int]` tail-recursive sum, the issue's canonical shape.
- `examples/perceus/array_borrow_record_1120.kai` (+`.out.expected`) — the
  `Array[Record]` `a[i].v` shape.
- Gates `test-perceus-1120-array-borrow` (C oracle) and
  `test-perceus-1120-array-borrow-native`: assert `incref_total < 150`
  over 100 reads (vs ~200 pre-borrow) AND exact output, on both backends.

Coverage gap: no negative fixture for a user `fn array_get` shadow (the
non-shadowed gate). The gate is exercised implicitly by every real
program, but a dedicated shadow fixture would pin the soundness edge.

## Verification

- Selfhost byte-id: OK on **both** C and native (native self-host gate
  COMPILE+LINK+RUN+SELF-COMPILE closes).
- Serial backend parity (`BACKEND_PARITY_JOBS=1`): perceus subset
  pass=105 fail=0.
- rb-tree bench: unaffected — `rb_tree.kai` uses no arrays, so the emit is
  byte-identical (confirmed by selfhost byte-id); 0.499 s native, no
  regression possible.

## Follow-ups left for next lanes

- **User-param borrow.** The prim table is closed by design (asu:
  hard-coded borrow only for prims). Extending borrow inference to user
  function params reading an array only through `a[i]` is the natural
  next step but a separate lane.
- **Element unboxing.** The residual per-access incref is the boxed
  element. Value-immediate `Int` (Tier 3, post-MVP) closes it; until then
  `Array[Int]` reads pay one element incref the `&[i64]` Rust path does
  not.
- **Preexisting C-backend raw-TCO-index bug** (`array.to_list`): worth an
  issue on its own; the native backend (default) is unaffected.
