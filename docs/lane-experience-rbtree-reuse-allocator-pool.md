# Lane experience — rb-tree to ≤10× C: allocator free-list + 1:1 variant reuse

**Date:** 2026-05-31
**Branch:** `rbtree-raw-slot`
**Outcome:** SHIPPED. rb-tree 1M random inserts goes from **16.62× C → ~9.7–10.0× C**
(3805 ms → ~2.30 s; the ratio rides the 10× line by C-run variance, kaikai itself
is stable at 2.25–2.32 s). Two files: `stage0/runtime.h`, `stage2/compiler/emit_c.kai`.

## Scope as planned vs as shipped

**Planned (start of session):** chase the documented "value-immediate Int" lever
(75% of allocs are Int boxes) believed to be the dominant cost.

**Shipped (after measurement refuted the plan):** the lever that actually moved
rb-tree turned out to be **allocator-level cell/slot reuse**, not Int unboxing.
Measured at fixed tree size (64k inserts): killing Int boxing ENTIRELY moves only
15.8×→13.5× (~15%) — Int incref/decref is cheap (add/branch); the dominant wall
cost is the 22.6M variant `malloc`/`free` of the functional spine rebuild. So the
shipped work targets that:

1. **Fixed-size allocator free-lists (Koka/mimalloc model)** in `runtime.h`. Every
   freed `KaiValue` cell and every variant `slots[]` array is recycled by the next
   same-size allocation, without compiler-level token threading — exactly the
   size-keyed implicit heap reuse Koka gets for free. `balance`'s discarded spine
   cells get recycled by the next constructor of the same size. This is the bulk
   of the win (16.62×→~10×).
2. **1:1 variant reuse-in-place arm** in `emit_c.kai` (`try_reuse_variant_arm` +
   `emit_match_arm_reuse_variant`), the cons dual-path generalised to N typed
   slots. `reuse_in_place` 0→1M on rb-tree.
3. **Aliasing guard** in all three `kai_reuse_or_alloc_{cons,record,variant}`:
   store-new-then-decref-old-only-if-different. A latent double-free (the rebuild's
   new slot may alias the old one when the body reused that cell in place) that
   libc malloc masked and the free-list's immediate recycle exposed.

## Design decisions and alternatives considered

- **Allocator free-list vs reuse-token pool (asu's recommended model).** asu
  designed a Koka-style `kk_reuse_t` token pool (drop yields a cell as token, ctor
  consumes by shape). I prototyped its runtime primitives (`kai_variant_at`,
  `kai_drop_reuse_variant`) but they require cross-function token threading
  (`insert_loop` → `balance`) — a calling-convention change with C/LLVM parity
  risk. The free-list achieves the same *effect* (size-keyed recycle) with zero
  compiler threading, because libc-style same-size reuse is exactly what the
  functional rebuild needs. Token primitives were removed as dead code.
- **Overwrite-in-place reuse (naive `has_prim` guard relax) — REJECTED.** Tested:
  double-frees (SIGSEGV/ASAN). The rotation cases of `balance` are non-1:1 (match
  three cells, reconstruct three reordered) — overwrite leaks/double-frees the
  intermediate cells. Only the 1:1 recolor/default arm is overwrite-safe.
- **Pool cap 262144 vs 1Mi.** 262Ki oscillated 9.9–10.3× (spills to libc past
  peak free-list depth, destabilising at the target); 1Mi (8 MB pointers, lazy
  .bss, RSS +~10 MB) holds a stable 9.7–9.9×. Chose 1Mi for a defensible margin.
- **Disabled under KAI_TRACE_RC/PROFILE_RC** (poison + exact malloc/free pairing
  for leak attribution would break). Kill switches `KAI_NO_CELL_POOL` /
  `KAI_NO_SLOT_POOL` for bisection.

## Structural surprises the brief did not anticipate

- **The `bump` benchmark inverted the lever ranking.** A list-transform where
  reuse fires at 100% still runs 28× C — entirely Int boxing. That made it look
  like Int was the lever. But rb-tree's reuse does NOT fire (the real bottleneck),
  so on rb-tree the alloc/free of variant cells dominates and Int is only ~15%.
  The same lever has opposite weight on the two workloads. Only measuring both at
  fixed scale disentangled it.
- **The bug that cost the session: pattern-binder shadowing in the new reuse arm.**
  `emit_match_arm_reuse_variant` passed `cx` (not `arm_cx`) to the new-arg
  expressions, so a binder named like a stdlib fn (`tail` in `EBlock`) resolved to
  the GLOBAL `list.tail` closure instead of the local bind → emitted
  `dsg_map_opt_expr(kai_closure(&_kai_list__tail_thunk,...))` → "non-exhaustive
  match" panic in selfhost (kaic2, the most complex program). Latent under libc
  malloc; the pool's immediate recycle exposed it. lldb backtrace at
  `kai_prelude_panic` localized it to `dsg_map_opt_expr ← dsg_map_expr_kind ←
  lower_narrow_expr`. Fix: emit arg expressions under `arm_cx` (lcs extended with
  pat_binders). Lesson: any new emit path that builds expressions referencing
  pattern binders MUST thread the binder-extended ctx, exactly like the cons arm.
- **A local `type` decl in a stage2 module is not free.** An early version declared
  `type RvCtor` for the reuse extractor; its runtime variant tag collided with the
  compiled program's own variant tags → "field access on non-record" on every
  variant match. Removed the type, inlined the match using the existing
  `reuse_ctor_name`. Same class as the `import compiler.X` name-collision lesson.

## Fixtures added and coverage gaps

- No new dedicated fixture. The 1:1 variant reuse is exercised by the existing
  perceus regression suite (`test-perceus-issue118` reuse_variant_basic /
  reuse_variant_no_misfire, issue350 gap3_*) which all pass, and the allocator
  free-list is exercised by **every** tier0 program (it is the default allocator).
  rb-tree itself (`examples/perceus/rb_tree_bench.kai`) is the perf fixture, not
  tier-wired (too slow). **Coverage gap:** no fixture pins the aliasing-guard
  double-free shape directly; it is covered transitively by selfhost (which
  panicked without the fix) + the perceus suite under the pool. A targeted
  `examples/perceus/reuse_alias_*` fixture would make the regression explicit —
  left as a follow-up.

## Real cost vs estimate

No time estimate was given (project rule: gate by soundness, not hours). The lane
spanned the full arc: measure baseline → refute the Int lever → prototype + reject
overwrite-reuse and token-pool → land the allocator free-list → debug two latent
bugs the pool exposed (binder shadowing, aliasing double-free) → tune the cap.

## Follow-ups left for next lanes

1. **value-immediate Int** (tagged-pointer `KaiValue*`, bit-0 tag) — the OTHER
   75%-of-allocs population (68M Int boxes) + the 6.4× RSS. Orthogonal to this
   lane; Koka source read (kklib integer.h: `4n+1`, 62-bit, dup/drop no-op on
   smallint) confirms the design. Would push rb-tree well under 10× and toward the
   ≤2× Hanga Roa stretch.
2. **Full-spine reuse via TRMC + context-hole** — Koka inlines `balance` into a
   tail-rec-modulo-cons loop with a zipper hole, so the whole spine reuses (kaikai
   only gets ~1M/22.6M = 4% reuse without it). The real lever for ≤2×. Multi-day,
   cross-backend.
3. **Aliasing-guard fixture** — pin the double-free shape explicitly.

See `docs/benchmarks/rb_tree_2026-05-28.md` for the baseline profile.
