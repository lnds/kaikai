# Lane experience — TRMC for the builtin cons (`[h, ...recur(t)]`)

## Scope as planned vs. as shipped

**Planned (brief):** add an `EList` arm to `trmc_rewrite_kind` so the
idiomatic list-build syntax `[elem, ...self_call(args)]` is recognised as a
modulo-cons site and rewritten to the `__kai_trmc|` step sentinel, exactly
as the `ECall`-of-user-ctor path does today. The C-direct backend is the
target; native/LLVM cons-TRMC parity is an explicit follow-up.

**Shipped:** the cons-TRMC rewrite, but recognising **two** AST shapes (the
brief assumed one), with a soundness fix the brief did not anticipate (a
scrutinee-leak in the match-arm shape), gated to the C-text emitter only,
plus a regression fixture isolated from a **separate pre-existing bug** the
brief's headline repro conflates with this one (recursive whole-spine free).

Files touched:

- `stage2/compiler/emit_c.kai` (+~210 LOC): recognisers
  `trmc_list_cons_modcons` / `trmc_reuse_cons_modcons`, the cons step
  `emit_trmc_cons_step`, the shared `trmc_make_step_call`, the
  `emit_trmc_step` dispatch (renamed the variant body to
  `emit_trmc_variant_step`), and `cons_ok` threaded through the
  gate/rewrite chain (`tcrec_walk_tail`, `trmc_walk_modcons`,
  `trmc_rewrite_kind` + their helpers + `tcrec_rewrite_decls`).
- `stage2/compiler/driver.kai` (+13 LOC): `cons_ok = not (use_llvm or
  use_kir or use_native)` passed to `tcrec_rewrite_decls`.
- `examples/perceus/trmc_list_build_large.kai` + `.out.expected`: new
  regression fixture (1M-deep build + map, O(1) stack).
- `stage2/Makefile`: `test-trmc-list-build` target (C-only, 8 MB stack),
  wired into `test` / `test-fast` / `TEST_LIGHT_TARGETS`.
- `tools/backend-parity-skips.txt`: the large fixture is C-only (LLVM lacks
  cons-TRMC), skipped with reason.

## Root cause (confirmed, not assumed)

`trmc_rewrite_kind` (and the eligibility gates `tcrec_walk_tail` /
`trmc_walk_modcons`) only recognised a modulo-cons site behind
`name_first_is_upper(callee)` — a USER constructor. The builtin cons has no
uppercase callee, so `[h, ...recur(t)]` fell to the catch-all
`__kai_trmc_apply` (terminal leaf) and the recursion grew one C frame per
element. Confirmed: `kaic2 --emit=kir … | grep -c trmc-step` = 0 before;
the 2M build overflowed (exit 139) under `ulimit -s 8192`; the corpus had
**zero** modulo-cons firings because the only enabling path was user-ctor
and the dominant idiom is the cons literal.

## Structural surprise #1 — there are TWO shapes, not one

The brief described `EList([ElPlain, ElSpread(self_call)])`. That is only
the **raw-EList** shape (`build`: an `if` with no scrutinee). When the list
is built inside a **cons-pattern match arm** (`map_inc`:
`[h, ...t] -> [g(h), ...recur(t)]`), perceus's `pcs_try_reuse_cons` has
**already** rewritten the body to
`__perceus_reuse_cons(__pcs_scr, head, self_call)` before TRMC runs (TRMC is
a post-perceus pass). So the rewrite must also recognise that `ECall` shape.
Both desugar to a cons whose cdr (slot 1) is the single self-call; both
build via `kai_cons`. Missing the reuse-cons shape would have left the most
common case (mapping/filtering with a cons pattern) untouched.

## Structural surprise #2 — cons is NOT a registered variant

The existing `emit_trmc_step` builds the node via `kai_variant_u_fast` +
`kai_var_slots` and reuses the `_arm_ru` donation token. A cons cell is a
distinct runtime tag (`KAI_CONS`, `as.cons.{head,tail}`), not a
`KAI_VARIANT`. So the step needed a dedicated `emit_trmc_cons_step` that
builds `kai_cons(head, NULL)` and opens the hole at `&node->as.cons.tail`.
The cctx machinery (`kai_cctx_extend_linear` / `kai_field_addr_create`) is
generic — it just takes a `KaiValue**` hole and a child — so only the node
build + hole address differ from the variant path.

## Soundness decision #1 — why the ctor-only gate existed, and the new gate

The original gate was conservative for a reason: the variant TRMC step's RC
discipline relied on `try_arm_top_reuse`, which captures the consumed
scrutinee as a reuse token (`_arm_ru`) — and that helper **only handles
PVariant** arms. For a cons arm it returns `None`, so the arm keeps the
legacy incref-bind and the goto **skips the match-exit `kai_decref(_scr)`**.

That is exactly the leak this lane had to fix. Without it, the reuse-cons
case (`map_inc`) leaked one input cons cell per level (verified under
`KAI_TRACE_RC`: `tag=cons allocs=10 frees=5 LEAK`). The fix: the cons step
distinguishes the two shapes via the sentinel marker — `__kai_cons` (raw
EList, no scrutinee) vs `__kai_cons_s` (reuse-cons, has `_scr`) — and the
`_s` step decref's `_scr` after the rebuild has captured the incref'd
`kai_h`/`kai_t` copies. Post-fix: `tag=cons allocs=10 frees=10 live=0`
(both build's input AND map's output balance). ASAN+UBSan clean at small N.

A full Koka reuse-token donation (extending `try_arm_top_reuse` to PList so
the cons cell is reused in place, zero alloc per level) is the **perf**
follow-up. This lane ships the **correct** fresh-alloc-and-drop; the perf
lane is orthogonal and gated separately.

## Soundness decision #2 — C-only, gated by `cons_ok`

`tcrec_rewrite_decls` runs once for ALL backends (post #706, `allow_trmc =
true` unconditionally) before the backend split. The cons step sentinel is
lowered only by the C-text emitter; the LLVM/KIR/native step builders
construct a variant node (`evar_find_tag(cname)`, `kaix_variant_at`,
`kaix_field_addr` indexing `kai_var_slots`) with no cons cell path. So cons
sentinels must NOT reach those backends. A `cons_ok` flag (`not (use_llvm or
use_kir or use_native)`) gates the cons shapes off there; those fns fall
back to ordinary recursion (correct values, stack-growing — small fixtures
do not overflow). User-ctor TRMC stays on for all backends, unchanged.

This mirrors the historical `allow_trmc = not use_llvm` gate, but scoped to
the cons shapes only (variant-modcons stays cross-backend per #706).

## Structural surprise #3 — the brief's 2M repro hits a SECOND, separate bug

The brief's exact repro (`build(2M); map_inc; list_length`) still exits 139
after the fix — but **not** because construction overflows. The
construction is now O(1) (proven: `build(2M)` + a borrowing/consuming
traverse exits cleanly under 8 MB). The residual overflow is the **runtime's
recursive cons-spine free**: `kai_free_value`'s `KAI_CONS` case does
`kai_decref(v->as.cons.tail)`, which recurses one frame per cell. Freeing a
~50K+ list overflows the stack — independent of how the list was built.

Proof it is pre-existing and orthogonal: the accumulator build
`build(n, acc) = build(n-1, [n, ...acc])` — which has ALWAYS been O(1)
construction via plain TCO, never needing TRMC — **also** exits 139 at 2M
when `list_length` frees the result. This is a runtime concern (`kai_free_
value`, two runtime.h copies), out of this frontend lane's scope per "one
worktree fixes one thing".

The regression fixture therefore isolates the construction TCO (the bug this
lane fixes) from the free recursion: it builds 1M via both shapes and reads
the result via a tail-recursive descent that frees cell-by-cell (O(1)
stack), never triggering the deep recursive free. The fixture runs under an
explicit 8 MB stack and pins the value.

## Fixtures added

- `examples/perceus/trmc_list_build_large.kai` (+ golden): 1M-deep build
  (raw EList) + map (reuse-cons), O(1) stack, value pinned. Wired into
  `test-trmc-list-build` (C-only, 8 MB stack, asserts BOTH `goto
  _kai_build_entry` and `goto _kai_map_inc_entry` are planted so a rewrite
  regression fails loudly, not silently). On the backend-parity skip list
  (LLVM cons-TRMC follow-up).
- The existing `examples/perceus/trmc_modcons_llvm.kai` (small `[10,20,30]`
  lists) is the C↔LLVM **value-parity** guard — now actually exercises C
  cons-TRMC (`map_inc` fires) while LLVM falls back; both print identically.

Coverage gap: no fixture exercises a cons-modcons fn with a `/ Mutable` or
suspendable effect row — those are rejected by `trmc_row_is_affine` (REmpty
only), the same gate the variant path uses, so the path is shared and
already covered by the affine-row discipline. A negative fixture proving a
non-affine cons-modcons falls back to ordinary recursion would tighten this.

## Gates

- 1M-deep build + map under 8 MB stack: exit 0, value pinned (C-direct).
- tier0: selfhost deterministic (`kaic2b.c == kaic2c.c`), demos baseline,
  arena gate — all green. The selfhost is NOT false-green: the compiler's
  own source has 169 raw-cons + 19 reuse-cons (scrutinee-drop) steps in its
  emitted C, so the new path is heavily exercised AND byte-deterministic.
- backend-parity (`pass=413, fail=0`): the large fixture skipped, the small
  value-parity fixture matches on both backends.
- `KAI_TRACE_RC`: cons cells balance (allocs == frees) for both shapes.
- ASAN+UBSan: clean on the cons-TRMC path (small N, to avoid the unrelated
  recursive-free overflow).
- Native (`--emit=native` / KIR): cons-TRMC correctly disabled (0 sentinels);
  `examples/native/` has no TRMC-shaped fixture (per brief gate #4).

## Real cost vs. estimate

The brief framed this as "add an EList arm + helper". The EList arm itself
was small; the cost was in the three structural surprises — the reuse-cons
second shape, the cons-vs-variant runtime layout, and especially the
scrutinee leak (caught only by `KAI_TRACE_RC`, not by any value test). The
2M-repro conflation with the recursive-free bug cost a detour to prove the
two are independent and to design an isolating fixture.

## Follow-ups left for next lanes

1. **Recursive cons-spine free** (runtime, separate lane): `kai_free_value`'s
   `KAI_CONS` case recurses per cell and overflows freeing a >~50K list,
   regardless of how it was built. This is what blocks the brief's EXACT 2M
   repro from exiting cleanly. An iterative tail-spine free (walk the cdr
   chain, free each unique cell, stop at a shared/non-cons cell) fixes it,
   but touches both runtime.h copies + the poison/recycle/trace epilogue and
   needs its own ASAN/RC-trace gate. NOT done here (out of lane).
2. **Native/LLVM cons-TRMC parity**: lower `__kai_cons` / `__kai_cons_s` on
   the LLVM text + KIR/native backends (a `kaix_cons` build path + a
   `kaix_cons_field_addr` returning `&node->as.cons.tail`). Removes the
   backend-parity skip and gives those backends O(1) list construction.
3. **Cons reuse-in-place**: extend `try_arm_top_reuse` to PList so the cons
   step donates the consumed cell (Koka uniform reuse) instead of
   fresh-alloc + drop. Pure perf; the current path is already RC-correct.
