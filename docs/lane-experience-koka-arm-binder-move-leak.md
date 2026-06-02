# Lane retro — Koka arm-binder move + consume-map (rb-tree RSS 38.6×C → 2.29×C)

Base: `15c05b5` (#741 follow-up, rb-tree ~6.2×C wall / 38.6×C RSS).

## Result

| rb-tree N=1M (M4 Pro) | baseline | shipped |
|---|---|---|
| **RSS** | 1920 MB (38.6×C) | **114 MB (2.29×C)** |
| **leaked** | 19.9M nodes | **14** |
| wall | 1.573s (6.2×C) | ~1.31s (5.4×C) |
| reuse_in_place | 5.6M | 19.6M |
| free_total | 1.7M | 7.6M (= alloc) |

Gates: selfhost byte-identical, ASAN clean (rb-tree + gap3 + regexp), perceus
examples 0 crashes, demos 34/34, tier0 green.

The RSS — the load-bearing number — went from 38.6×C to **2.29×C**, near memory
parity with C. The spine discarded by every insert is now freed (free_total ==
alloc_total); `live_peak` == the final 1M-node tree.

## The chain (each link verified, Koka as the sole source of truth)

1. **Koka uses NO borrow (`^`) on bench/rbtree.kk** (verified against Koka 3.2.3
   source + generated C via asu). The leak is killed by `is_unique`-gated
   dup/drop at the match arm top (Parc), not inter-procedural borrow. The #741
   retro cited the wrong file (`perf/sets/rbtree-okasaki.kk` lookup path uses
   `^`; the insert bench does not).

2. **The leak = spurious dups.** kaikai counted `is_red(l)` + `insert_loop(l)`
   as 2 uses of `l` on the descent path → dup'd `l` → the descent tree stayed
   shared (rc ≥ 2) → reuse inert → the spine (~8.8 nodes/insert = tree height)
   never reached rc=0.

3. **Fixes, all transcribed from Koka's Parc/ParcReuse:**
   - `kai_reuse_free` (runtime) = `kk_block_drop`: frees a captured reuse-
     token's CELL only (children already moved), zeroing `n_args`.
   - emit `emit_arm_top_reuse`: dropped the `arm_all_tails_consume_token` gate;
     the token is disposed LINEARLY (consumed by a TRMC `kai_variant_at`, or
     freed on the balance tail). `_scr = NULL` after the steal so the match-exit
     decref does not double-dispose. Unblocks insert_loop's Black arm.
   - **Borrow-AWARE per-path count** (`pcs_max_paths_b_*`): a use landing in a
     BORROWED callee slot (`is_red(l)`) counts 0 (Koka's `Borrowed` env). This
     is what lets `l`/`r` pass `mp <= 1`.
   - **Consume-map** (`pcs_build_consume_map`, the Koka `Borrowed` DUAL): a
     caller may MOVE a binder only into a CONSUMING callee slot (deconstructs /
     stores). A field-read callee (`nb_add_transition(b)` reads `b.states`) is
     non-consuming → moving a binder there is blocked (the regexp UAF).
   - **Per-arm move** restricted to reuse-shaped arms:
     (a) self-rec modulo-cons (`self(...)` in a ctor slot → TRMC arm-top
     reuse, insert_loop) — sound at any depth; (b) cons-overwrite (rebuild the
     same ctor in tail → emit's in-place `kai_check_unique` overwrite,
     balance) — sound ONLY at the fn-body's TOP-LEVEL match (`tl` gate).
   - **emit non-bijective reuse** (`emit_match_arm_reuse_variant`): when a new
     ctor slot is a nested ctor embedding a pointer child, OWN (incref) the
     children + decref the old slots before overwrite, instead of borrow-bind
     (which aliases → cycle).

## The two traps that cost the most

- **regexp UAF** (cons-overwrite moved a binder into a field-read callee).
  Fixed by the consume-map's `cons_ok` clause + higher-order (lambda) guard.
- **gap3_balance_minimal cycle** (cons-overwrite reused the INNER scrutinee of a
  NESTED match — `match l { Node(ll,..) -> match ll {..} }` — but the result
  replaces the OUTER node `l`; the in-place overwrite built a self-referential
  cycle → `size` stack-overflows). The decisive fix: the **`tl` (top-level
  match) gate** on cons-overwrite. balance matches its PARAM `l` at the top
  level (the reused cell IS the cell the result replaces) → reuses; gap3's
  problematic reuse is in a nested match over a binder → excluded. This is the
  load-bearing distinction that reconciles the rb-tree RSS with correctness.

## Structural surprises

- The borrow-model (borrow bind + is_unique-gated shell dispose) is UNIVERSAL in
  Koka — every deconstructing match arm, not just TRMC (asu, Parc.hs).
- `make kaic2` hides `pub`-type-leak privacy AND the gap3/desugar cycle (the
  cell-pool masks it; only `-O2` without TRACE_RC surfaces it, and selfhost
  catches it byte-for-byte). The hard gates earned their keep repeatedly.
- A blunt global `, bmap)` → `, bmap, cmap)` thread broke the build by over-
  propagating into `pcs_max_paths_b_*`. Thread new params surgically, never
  with a global text replace.

## Follow-ups

1. **wall is still 5.4×C** — the RSS lever (reuse) is in; the remaining wall is
   RC traffic (rb_get's incref(l)+incref(r)+decref per lookup level) + per-op
   boxing. rb_get borrow (Koka's `member` lookup path DOES use `^`) is the next
   wall lever; tagged-Int already shipped (#741).
2. Remove dead `arm_all_tails_consume_token` + helpers from emit_c (cosmetic;
   `-Wno-unused-function` tolerates it, selfhost is byte-identical).
3. The consume-map is a one-pass approximation (consume = deconstructing match
   scrutinee OR ctor slot). A full B3 worklist fixpoint would widen it; not
   needed for the rb-tree/regexp/stdlib set that gates today.
