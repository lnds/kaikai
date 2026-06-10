# Lane experience — KIR native walk, TRMC backend (the cctx goto-loop)

KIR Lane 1.2 Parte B. The native backend half of TRMC
(tail-recursion-modulo-cons): lowering `KTrmcStep` / `KTrmcApply` to a
constructor-context (cctx) goto-loop so `[h, ...self(t)]` list builders
run in O(1) stack instead of one frame per element. Pairs with the
frontend lane (PR #800, `fix(kir): defer the self-call in TRMC step
lowering`), which enabled `cons_ok` for the KIR/native path and reshaped
`KTrmcStep` to carry `head_vals` + `assigns`.

## Scope as planned vs shipped

Planned: lower the two TRMC terminators the frontend now emits, mirroring
the C-direct `emit_trmc_cons_step` oracle. Shipped exactly that, plus a
runtime shim and an arg-arity helper.

## The sequencing trap (load-bearing)

PR #800 (frontend) was merged FIRST, alone — and it broke `tier1-native`
on main. Enabling `cons_ok` for native means the stdlib core functions
(linked into EVERY native program) now contain `KTrmcStep`, which the
native backend aborted via `nemit_unsupported`. One aborted node makes the
WHOLE module fail to emit, so `arith_exit` — which uses no TRMC in its own
code — failed too, along with all 13 fixtures. The frontend reshape and
the backend lowering are COUPLED (the node shape and the emitter must
agree); splitting them into two PRs left a window where main's native path
was fully broken. **Lesson: a node-reshape + its sole consumer should land
together, or the consumer first.** This backend lane RESTORES `tier1-native`
to green — it is a fix for the gate #800 opened, not a new feature.

## The mechanics (mirrors emit_trmc_cons_step)

A new module `stage2/compiler/emit_native_trmc.kai` (A++ 98.6, 134 LOC):

- **cctx accumulator allocas** (`nemit_trmc_allocas`): a fn whose body has
  a `KTrmcStep` gets two entry-block allocas — `_kai_acc_res` (the spine
  root, ptr) and `_kai_acc_hole` (the open tail address, ptr-to-ptr) — both
  seeded NULL. Same allocas-in-entry discipline as the effect frames.
- **`KTrmcStep`** (`nemit_trmc_step`): build `kaix_cons(head, NULL)` (head
  from `head_vals`, tail the open hole), `kaix_cctx_extend(res, hole, node)`
  to plug it into the old hole and get the new root, recompute the new hole
  with the NEW shim `kaix_cons_tail_addr(node)`, store both back, then
  rebind the loop params from `assigns` (the deferred self-call's args) and
  branch to the loop header. The recursive call is never made.
- **`KTrmcApply`** (`nemit_trmc_apply`): `kaix_cctx_apply(res, hole, leaf)`
  plugs the terminal leaf into the open hole and returns the spine root,
  then `ret`. An empty cctx (first level, hole NULL) returns the leaf.

## Runtime shim added

`kaix_cons_tail_addr(node) -> KaiValue**` in `stage0/runtime_llvm.c` —
the address of a `kai_cons` cell's tail slot. The existing `kaix_field_addr`
indexes `kai_var_slots[holeslot]`, which is the VARIANT path; a builtin cons
is not a registered variant, its tail lives at `node->as.cons.tail`. Without
the dedicated shim the hole would point at the wrong word. Mirrors the C
oracle's `kai_field_addr_create(&_trmc_node->as.cons.tail)`.

## Bugs found

- **`ncall_sig2` arity (mine, trivial).** The cctx shims take THREE args
  (`res, hole, child`); I called the two-arg `ncall_sig2`, producing
  `too many arguments to function call, expected 7, have 9`. Added
  `ncall_sig3` (a sibling of the sig0/1/2 family).
- **SOUNDNESS RISK #1 (anticipated).** `ntrmc_head_val` returns a `Handle`
  (the cons head Value*); listed in stage1's `native_handle_fns()` so the
  type-blind kaic1 Perceus does not decref it. The step/apply emitters
  return Unit, so only that one helper needed listing.

## The four soundness risks (per the TRMC design) and how they were handled

1. **Hole aliasing** — the hole address is recomputed against THIS step's
   cell (`kaix_cons_tail_addr`) and stored back to the alloca every step,
   never hoisted. The reload-per-iteration discipline.
2. **Dropmask** — carried on `KTrmcStep`, threaded into the rebind exactly
   like `KTcrecGoto` (the subset-1 arithmetic TCO). The hole-slot arg is
   never materialised, so it has no ref to drop.
3. **Terminal leaf** — `KTrmcApply` ALWAYS emits `kaix_cctx_apply`, so the
   list never ends with a NULL hole (the easy-to-forget edge that
   truncates/segfaults if missed).
4. **i64-inline** — N/A: the native walk is all-boxed (per the lane
   strategy), so the cons cell is a real `kai_cons` with boxed head/tail;
   no kind-1 slot layout to reconcile.

## RC note (the "leak" that wasn't)

`KAI_TRACE_RC` on the fixture shows `leaked=8000, free_total=0`. This is
NOT a TRMC bug: a non-TRMC list program (build+lenacc, no map_inc) leaks
4000 on native, and the SAME program under the C-direct oracle leaks 4006.
Both backends behave identically — the fixture `exit()`s without running
main's RC teardown, so the built list is never dropped. My TRMC is
RC-NEUTRAL versus the oracle (the delta is exactly the 4000 cells map_inc
builds, in parity). The real soundness gate — ASAN UAF/overflow + C-direct
value parity — is clean. (The pre-existing native-cons arg-pass leak is
tracked separately; it predates this lane.)

## Fixtures added

`examples/native/trmc_list.kai` — `map_inc` over a 4000-element list,
exits with `count % 256` (160). The pre-TRMC stack-growing lowering would
overflow at ~65K; at 4000 it runs either way, so the fixture proves VALUE
parity (native rc == C-direct rc), and the O(1)-stack property is verified
separately at 200K (overflows pre-fix, runs post-fix).

## Gates (all run, all green)

- Native parity: **14 passed, 0 failed** (incl. trmc_list rc=160; restores
  the green tier1-native broke by #800-alone).
- TRMC O(1) stack: map_inc over 200000 elements — native exit 0 (was 139),
  parity with C-direct.
- Selfhost byte-id: OK (kaic2b.c == kaic2c.c).
- ASAN+UBSan on trmc_list: 0 errors, exit 160.
- KAI_TRACE_RC: RC-neutral vs the C-direct oracle (see RC note).
- `km`: emit_native_trmc.kai A++ (98.6, 134 LOC), cogcom avg 1.0/max 2;
  emit_native_ops.kai A (92.4); emit_native_term.kai A+ (95.4).

## Follow-ups for next lanes

- **`emit_native_term.kai` is at 398 LOC** (target < 400). This lane put
  the TRMC logic in its own module precisely to avoid crossing it, adding
  only the import + the two terminator arms + the allocas call. The NEXT
  subset touching this file (KTailCall, or reuse tokens) MUST split the
  terminators or seed helpers into their own module BEFORE growing it past
  400 — it has no headroom left.
- **Reuse-in-place during the TRMC step** is not done: the step
  fresh-allocates each cons cell (`kaix_cons`), where the C/LLVM oracles can
  rebuild INTO a donated unique scrutinee shell (`%_arm_ru` /
  `kaix_variant_at`) for zero-alloc map. That is a perf lane, not a
  correctness gap — values + stack behaviour are already correct.
- **KDropReuse / KFreeToken** (the reuse-token RC ops) and **KTailCall**
  (non-recursive tail call) remain the only genuine native-walk gaps after
  this lane.
