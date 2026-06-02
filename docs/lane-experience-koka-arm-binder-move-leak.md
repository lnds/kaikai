# Lane retro — Koka arm-binder move + linear reuse-token (rb-tree leak)

Base: `15c05b5` (#741 follow-up, rb-tree ~6.2×C wall / 38.6×C RSS).

## Scope as planned vs as shipped

**Planned:** reach Koka parity (1×C wall, ~1.02×C RSS) on the canonical
Okasaki rb-tree by transcribing Koka's Perceus/Parc, with Koka as the sole
source of truth. The session's load-bearing finding (verified against Koka
3.2.3 source + generated C, via asu): **Koka does NOT use borrow (`^`) on this
benchmark** — `is-red`, `balance-left/right`, `ins` are all owned. The leak is
killed by `is_unique`-gated dup/drop at the match arm top (Parc), not by
inter-procedural borrow. The retro of #741 cited the wrong Koka file
(`perf/sets/rbtree-okasaki.kk` lookup path, which does use `^`).

**Shipped (safe, gated):**
- `kai_reuse_free` (runtime) = Koka `kk_block_drop` — frees a captured
  reuse-token's CELL only (children already moved into owned binders), zeroing
  `n_args` so `kai_free_value` does not cascade. The linear-token disposal
  ParcReuse needs when a tail does not consume the token.
- `emit_arm_top_reuse` (emit): removed the `arm_all_tails_consume_token` gate;
  the captured token is now disposed LINEARLY — consumed by a downstream
  `kai_variant_at` (TRMC step) or freed by `kai_reuse_free` on a tail that does
  not rebuild (the balance tail). `_scr = NULL` after the token steal so the
  match-exit `kai_decref(_scr)` does not double-dispose. This unblocks the
  Black arm of `insert_loop`.
- Borrow-AWARE per-path use count (`pcs_max_paths_b_*`): a use of `nm` landing
  in a BORROWED callee slot (`is_red(l)`) counts 0 — Koka's `Borrowed` env,
  consumed by the dup decision. Without it `is_red(l) + insert_loop(l)` sum to
  2 on the descent path and `l` fails the `mp <= 1` skip → a spurious dup keeps
  the spine shared → the ~8.8-nodes-per-insert leak.
- Per-arm move (`pcs_collect_arm_skip_binders` + `pcs_arm_self_modulo_cons`):
  pattern binders of SELF-RECURSIVE MODULO-CONS arms (`self(...)` in a ctor
  slot — the shape tcrec lowers to a TRMC step, which emit disposes via the
  borrow-model) transfer ownership raw. Restricted to that shape because a
  non-self-rec arm keeps the legacy emit (incref bind + recursive
  `decref(_scr)`); moving a binder there double-frees (the stdlib/regexp
  `nb_add_transition` UAF).

**Measured (rb-tree N=1M, M4 Pro):**
| | baseline | shipped |
|---|---|---|
| wall | 1.573s (6.2×C) | **1.104s (4.4×C)** (−30%) |
| leaked | 19.9M | 16.7M (−16%) |
| RSS | 1920 MB | 1614 MB (−16%) |
| reuse_in_place | 5.6M | 8.2M (+46%) |

Gates: selfhost byte-identical, ASAN clean (rb-tree + regexp), demos 34/34,
tier0 green.

## The wall (load-bearing finding) — and the proven path past it

A **wider** move (any arm binder with `mp<=1 ∧ cv ∧ ln`, not restricted to
self-rec-modulo-cons) was MEASURED to reach **leaked=14, RSS 114 MB
(2.29×C)** — essentially Koka's behaviour — but UAF'd stdlib/regexp. Root
cause (confirmed by backtrace + asu): in a NON-reuse match arm, emit's
`emit_pat_binds` does an unconditional `kai_incref` bind AND the match-exit
does a RECURSIVE `decref(_scr)`; moving a deconstructed child there
double-disposes it. `nb_add_transition`/`nb_set_transitions` FIELD-READ their
`b` arg (they do not consume it), so a moved binder passed there is freed under
the callee.

asu (Koka as truth) confirmed the fix is the **consume-map** — the Koka
`Borrowed` dual: a caller may MOVE a binder only into a CONSUMING callee slot
(deconstructs / stores); a non-consuming slot (field-read callee) must keep the
caller's ref. The consume-map (`pcs_build_consume_map`, `pcs_param_consumes`,
`pcs_uses_only_consuming_slots`) was DESIGNED and WRITTEN this lane but NOT
wired — threading `cmap` through the collector via a blunt global
`, bmap)` → `, bmap, cmap)` replace over-propagated into `pcs_max_paths_b_*`
(which take only bmap) and broke the build. Reverted to the safe
self-rec-modulo-cons gate. **The consume-map is the next lever**: wire it
(surgically, NOT a global replace) to replace the self-rec gate with the
consume-aware filter — it admits `balance` (ctor slots = consuming) and excludes
regexp (`nb_add_transition` = non-consuming), recovering the 2.29×C peak
without the UAF.

## Design decisions

- **Koka is the source of truth, verified, not assumed.** asu read Koka 3.2.3
  source + generated C; the "Koka uses borrow here" claim from #741's retro was
  refuted by the actual `bench/rbtree.kk` (no `^`, no fip).
- **The reuse-token is LINEAR** (ParcReuse): consumed by a `Con@reuse` or
  dropped (`kk_block_drop` / `kai_reuse_free`) on the non-consuming path. This
  replaced the conservative `arm_all_tails_consume_token` gate.
- **perceus SIGNS, emit TRANSCRIBES** held: `arm_moved_binders` reads the move
  off the post-dup AST (bare use vs `__perceus_dup`); recomputing diverges.

## Structural surprises

- The borrow-model (borrow bind + `is_unique`-gated shell dispose) is
  UNIVERSAL in Koka — every deconstructing match arm uses it, not just TRMC
  arms (asu, citing Parc.hs/ParcReuse.hs). kaikai's `emit_pat_binds` (incref +
  recursive decref) is the legacy/incorrect path. Generalising the borrow-model
  to all deconstructing arms is the right end state but needs the acc_decl /
  fn-wide `_arm_ru` plumbing lifted out of the TRMC-only path; attempted, broke
  codegen (`_arm_ru` local shadowing), reverted.
- `make kaic2` hides `pub`-type-leak privacy errors; selfhost caught
  `BorrowEntry` needing `pub`. The hard gate earned its keep again.

## Follow-ups for next lanes

1. **Wire the consume-map** (surgical cmap threading through the
   `pcs_collect_arm_skip_*` family + `pcs_branch_aware_skip_locals_b`, NOT a
   global replace) → replace the self-rec-modulo-cons gate with the
   consume-aware filter → recover leaked≈14 / RSS 114 MB (2.29×C) with regexp
   intact. The consume-map code is written (this lane) but commented out /
   removed; re-add `pcs_build_consume_map` etc. and the `cons` clause in
   `pcs_branch_aware_skip_locals_b`.
2. **Generalise the borrow-model to every deconstructing arm** (lift acc_decl /
   `_arm_ru` out of the TRMC-only path) — the true Koka end state; gets balance
   and any record-rebuild arm reusing, toward 1×C wall.
3. Remove dead `arm_all_tails_consume_token` from emit_c (cosmetic).
4. rb_get borrow (read-only lookup path) — wall not RSS; revisit after the move
   levers land.
