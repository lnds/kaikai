# Lane experience — native `==` dispatch use-after-free (#858)

## Scope as planned vs as shipped

**Planned:** fix the native-backend use-after-free in the polymorphic `==`
dispatch (`kai_op_eq`) — a correctness crash on the DEFAULT backend over
real code (`set_basic` and ~57 parity fixtures), reproduced by an 8-line
recursive `mycontains`.

**Shipped:** exactly that, plus the regression fixture. The fix is a
3-function addition to the KIR list-match lowering (`kir_lower_match.kai`)
and a one-flag thread through `lower_list_arms` (`kir_lower_walk.kai`). No
runtime change, no oracle change.

## Root cause (with evidence)

The repro:
```
fn mycontains(xs: [a], x: a) : Bool = match xs {
  [] -> false
  [h, ...t] -> if h == x { true } else { mycontains(t, x) }
}
```
`main` traverses the same `[1,2,3]` three times. Native SIGSEGV'd on the
THIRD call; C ran clean.

ASAN gave the exact frame: `kai_is_int` ← `kai_op_eq` ← `kaix_eq` ←
`mycontains`, deref of address `0x8` with `a = NULL`. A traced runtime
(`kaix_variant_arg` + `kaix_eq` + `kai_free_value` printing addresses)
showed the smoking gun: at the end of `mycontains([1,2,3], 2)`, the cell
`cons[2,3]` was FREED — but `cons[2,3]` is the SHARED tail of `cons[1,2,3]`,
still live in `main` for the third traversal. On the third call,
`proj xs.1` returned that freed/recycled cell (tag corrupted to a
reused-immortal `tag=5 rc=INT32_MAX`), `proj.0` gave garbage `0x4`, and
`kaix_eq(0x4, …)` deref'd `0x4 + 0x4 = 0x8` → SEGV.

**Why C ran clean and native didn't — the asymmetry.** The C-direct
oracle's `emit_pat_binds_list` binds cons head/tail as `is_alias=true` and
emits a STRUCTURAL `kai_incref`:
```c
KaiValue *kai_h = kai_incref(_scr->as.cons.head);
KaiValue *kai_t = kai_incref(_scr->as.cons.tail);
... kai_decref(_scr);   // owned-match exit drop
```
So each recursion level OWNS its own scrutinee reference; the exit decref
balances that level's incref and never frees the parent's shared cell.

The KIR/native emitter omitted that structural incref: `KProj` →
`kaix_variant_arg` BORROWS (`runtime_llvm.c:443`, "#747 — borrow (no
incref)"), and the bind is `KLet name = proj` (a pure slot alias, "KIR
KLet never increfs"). Perceus runs over the same AST and — correctly for
a borrow-free model — MOVES the once-used `t` binder onto the self-tail
back-edge (no `__perceus_dup`). With the structural incref missing, `t`
is a pure borrow that the back-edge moves onward as the next scrutinee;
the next level's owned-match `__pcs_scr_drop` then over-frees the shared
parent cell.

The incref is the EMITTER's, structurally — NOT a Perceus KRC decision
(verified: the C-emitted `mycontains` has `kai_incref(tail)` at the bind
and NO `__perceus_dup` around it; perceus moved `t`). So the fix mirrors
the oracle structurally in the lowering, not in perceus — duplicating it
in perceus would create a second RC source of truth that desyncs from
emit_c (a rejected bug-class).

## The fix

`kir_lower_match.kai`:
- `list_arm_alias_binders(p)` — the head `PBind`s + `...rest` binder +
  whole-list catch-all binder (the set the oracle increfs).
- `lm_dup_alias_binders(names, pos, st)` — emits one `KRC(KDup …)` per
  binder into the arm's body block, mirroring the oracle's `kai_incref`.
- `arm_body_uses_reuse(body)` — true iff the arm consumes its scrutinee
  through a `__perceus_reuse_*` call (perceus's reuse recogniser stamp).

`kir_lower_walk.kai` (`lower_list_arms`, now threaded `owned`): after
`lm_emit_arm` binds the arm, emit the structural dups gated on
`owned ∧ ¬arm_body_uses_reuse(body)`.

### The two-way gate (the trap asu flagged)

The oracle's incref is NOT unconditional. Verified by emitting C for three
shapes:
- `mycontains` / `list_nth` / `myfilter` (owned, NON-reuse arm) → binder
  gets an UNCONDITIONAL structural `kai_incref`. **This is Case 1, #858.**
- rb-tree `ins` (owned, arm-top-reuse arm) → binder bound WITHOUT incref
  (move-from-slot), incref CONDITIONAL on the shared token
  (`if (_arm_ru == null) kai_incref(...)`), and `_scr = NULL` kills the
  exit drop. **This is Case 2 — the KIR already handles it via
  `kaix_variant_reuse_at`.**

So the dup is suppressed on reuse arms: a structural dup there would
double-incref the kept children on the shared-token path (rb-tree leak).
The gate derives `¬reuse` from the SAME `__perceus_reuse_*` marker the
reuse lowering reads — single source of truth, not a parallel compute.
Perceus's TRMC/goto-move binders keep their move semantics; this dup only
restores the incref the owned non-reuse arm always pays in the oracle.

## How the gates caught it

- **Repro (8 lines):** native SIGSEGV (exit 139) → after fix, native == C,
  exit 0, `c1/c2/c3: true`.
- **ASAN** on the linked native object (legacy `runtime_llvm.c` link, no
  bitcode, `-O0 -g`): pre-fix SEGV at `0x8`; post-fix clean.
- **KAI_TRACE_RC** on the repro: native cons balance now matches C
  (`free_total`/`decref_total` aligned; pre-fix native never cascaded
  decref→free, the #860 under-decref — distinct, still open).
- **Case 2 guard:** `llvm_arm_top_reuse_shared` (forces the SHARED token
  branch) runs native == C, ASAN-clean; `rb_tree_bench` `reuse_in_place`
  count IDENTICAL native vs C (6301528) — the fix does not touch the
  variant-reuse path (rb-tree matches on variants, not list patterns).
- **SERIAL ratchet** (`BACKEND_PARITY_JOBS=1`): the decisive gate. The
  PARALLEL ratchet false-greens this crash (its flaky-recheck marks the
  ~57 deterministic failures as flaky). Serial run confirms 0 deterministic
  fail.

## Structural surprises

- The `lldb` path on macOS hung on `run` (no debug entitlement); the
  productive route was linking the emitted native object against
  `runtime_llvm.c` under ASAN with `-I stage2` FIRST (the object is built
  against `stage2/runtime.h`, the tagged-Int runtime — `-I stage0` first
  left `kai_nullary_fast`/`kai_variant_reuse_at` undefined).
- The bug is OVER-decref (frees too early); #860 is UNDER-decref (cons leak,
  `free_total=0`). Same native RC domain, opposite direction, distinct
  sites. This lane does not touch #860.
- The fix lives in `lower_list_arms` (one site) rather than threading a
  flag through the whole `lm_emit_arm → lm_emit_cells → lm_bind_name`
  chain — the dups are emitted into the body block after the binds, which
  keeps the bind helpers untouched and the change to two files.

## Coverage / fixtures added

- `examples/perceus/native_eq_dispatch_uaf_858.kai` + `.out.expected`:
  the recursive `mycontains` over a shared list, traversed 3×, with both
  an Int-head and a `[String]`-head variant (the String head exercises the
  HEAP head binder, where the Int-head bug hid behind tagged immortals).
  Walked by the backend-parity harness (native vs C) — it is NOT in the
  explicit rawsafe/borrowsafe/issue118 lists, so the `.out.expected` is a
  stdout golden, not a dump golden.

## Variant / record arms — the slot-kind discriminant (the key subtlety)

The same KProj-borrow gap exists on the VARIANT-switch / default-arm paths
(`kir_lower_bind.kai`): a `PVariant` positional sub-binder is bound `KLet
name = KProj scrv idx` (a borrow), and the regexp `Ok(ast) -> Ok(R(
compile_nfa(ast)))` UAF (`regex_subsume`, ASAN: `kai_op_field` on a freed
record alloc'd in `compile_nfa`, freed in `compile`) is the same bug class.

**First attempt — a BLUNT all-slot variant dup — regressed 12 fixtures**
(serial 3 → 15). Root cause, found by reading the oracle's
`emit_pat_binds_variant`: the variant-arg incref is NOT uniform across slots.
The oracle increfs a slot ONLY when its kind is POINTER (`k==0`,
`is_alias=true`); an **Int slot (k==1) is read RAW** (`int64_t kair_<n> =
slot.i64`, no RC), a **Real slot (k==2)** is a fresh owning box
(`is_alias=false`, no incref), an **enum slot (k==3)** is an immortal
singleton. A blunt dup over an Int/Real slot over-increfs a value the KIR
keeps unboxed — that was the 12-fixture regression.

**The fix that landed restricts the dup to POINTER slots.**
`variant_arm_alias_binders` reads the ctor's payload types from the variant
registry (`LowerSt.vs`) and a local `ls_slot_kind` (mirror of emit_c's
`variant_slot_kind`) excludes Int (1) / Real (2) slots — exactly the slots
the oracle reads unboxed. Record-FIELD binders are exempt entirely (they bind
via the incref-ing `kai_op_field`). The same `owned ∧ ¬arm_body_uses_reuse`
gate as the list path applies. Measured: with the slot-kind filter,
`set_basic` / `binserialize_sum` / `map_basic` (variant-shape #858) now pass,
the rb-tree reuse arm is untouched (`llvm_arm_top_reuse_shared` clean), and
the `regex_subsume` record-UAF is gone under ASAN (no more use-after-free;
a separate regex logic/loop bug remains — out of scope).

## Follow-ups left for next lanes

- **`regex_subsume` / `result_collect` / `uuid_basic` / `huffman` / `list_*`
  / `option_collect` / `poly_ord_containers`** — still failing serial AFTER
  this fix, and ALL were already failing on pre-fix main (verified by `git
  stash` + rebuild, `comm -23` vs the 58-fixture main baseline = empty: zero
  regressions). These are distinct from #858: `regex_subsume` is a regex
  engine logic/loop bug (the binder-UAF is now fixed), `result_collect` /
  `option_collect` are timeouts (the #860 leak under no cap), `uuid_basic` is
  a v4 logic divergence (ASAN-clean, not RC). CI tier1-native is green on
  main with all of them present (the parallel ratchet's flaky-recheck
  tolerates them).
- **#860** (native cons/list RC leak, under-decref) is the real perf
  residual and remains open — orthogonal to this crash.

## Serial ratchet is load-sensitive (a gotcha for the next lane)

The serial ratchet (`BACKEND_PARITY_JOBS=1`) is NOT perfectly deterministic
on macOS: under low load one run showed 3 failures, under higher load
another showed 47 — the difference is the #860 leak pushing more fixtures to
OOM/timeout when memory is tight. The RELIABLE signal is the SET RELATION
(`comm -23 my_fails main_fails` = regressions), measured in comparable
conditions, plus DETERMINISTIC per-fixture rechecks (the #858 repro + the
variant-shape fixtures pass 3/3 in isolation). Do not read a single serial
count as gospel; compare sets and recheck individually.
