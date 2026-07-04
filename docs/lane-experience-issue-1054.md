# Lane experience — issue #1054 (native self-host SIGSEGV 139 = variant Real-slot UAF)

## Scope as planned vs as shipped

**Planned (brief):** #1054 and #1048 were framed as one coupled lane — the
#1048 record scope-exit drop tips a latent borrow into a live free (the #1054
139), so the drops are only sound on top of a borrow/owned fix. The #747 UAF was
hypothesised to be a POINTER-kind variant slot; the brief predicted "Int/Real
**no**".

**Shipped:** the lane DECOUPLED into two independent bugs.

- **#1054 = a variant REAL-slot UAF** (this lane): shipped, fixed, gated.
- **#1048 = a record scope-exit leak** whose fix (a dead-let / EField base drop)
  UNCOVERS a SEPARATE, pre-existing use-after-free in perceus's reuse-in-place —
  a bug BOTH backends inherit (the C oracle also corrupts under ASAN). That is
  not #1054 and not the Real slot; it is split to its own issue. #1048 stays
  blocked on it.

## Root cause (#1054) — an oracle-vs-native representation mismatch on Real slots

The C-direct oracle reads a Real variant slot as a FRESH owning box:
`kai_real(kai_var_slots(scr)[i].r)` bound `is_alias=false` — a brand-new
`KaiValue*` (rc=1) decoded from the raw `.r` double. `real_to_string` decref's
that fresh box; the scrutinee's slot is untouched.

The native reads the SAME slot as a BORROW. The native codegen builds every
variant with `slot_mask == 0` (all payloads boxed as pointers), so a `Real`
payload is a boxed `KaiValue*` in the slot, read via `KProj` → `kaix_variant_arg`
→ (mask 0) → `.ptr`, **no incref**. That is a borrow of the scrutinee's own box.
`pat_alias_binders` then EXCLUDED Real slots (kind 2) on the strength of the
oracle's fresh-box semantics — so no structural dup protected the borrow.
`real_to_string` decref'd the shared box to zero and freed it; the arm-exit
scrutinee drop then re-decref'd the freed slot → UAF.

The exact ASAN stack (bounded fixture + self-compile, identical):

```
READ  kai_decref            (arm-exit __pcs_scr_drop cascading into the slot)
freed kai_decref <- kaix_prelude_real_to_string   (real_to_string consumes its arg)
alloc kaix_real <- build / parse   (the Real box in the ERealN ctor)
```

Why Real and not Int: `kai_decref` is a no-op for a tagged immediate
(`v->rc == INT32_MAX` early return), and Int slots are tagged `kai_int`. A Real
is always a heap box, so its decref is real and the borrow is fatal.

## The fix — descend Real slots, do NOT reshape KProj or touch the runtime

`pat_alias_binders` now descends Real slots via a new `pa_slot_needs_dup(k)` =
`k == 0 or k == 2`. The owned match arm's existing `lm_dup_alias_binders`
structurally dups the Real binder exactly as it dups a pointer slot, giving it an
owned ref the arm-exit drop balances — the native analogue of the oracle's fresh
box. The `KProj` stays a pure borrow (NOT given a flag — that would be a second
source of RC truth); the arm that consumes the borrow compensates, mirroring the
oracle. The runtime `kaix_variant_arg` is untouched: its borrow-for-mask-0
contract is correct; the lowering, not the runtime, owed the dup. `km` A+ (94.9).

Two prior attempts failed on the wrong axis: (1) a global incref in
`kaix_variant_arg` broke #872's reuse arms; (2) an owned-only fix on direct
pointer slots missed the Real slot. The correct axis is the per-arm structural
dup, filtered by slot kind, in the single collector.

## The gate that pins it — ASAN on the full self-compile

The 139 does not fire in a small repro under the normal build: the cell pool
recycles the freed box into the next same-size alloc, so the double free hands
back a live-looking cell. Method: build the compiler's runtime under
`-fsanitize=address -DKAI_LLVM -DKAI_NO_CELL_POOL`, link the native-compiled
`main.o` (bitcode hidden so `runtime_llvm.c` is instrumented) + shim + libLLVM,
run that native-built compiler self-compiling. With the pool off, ASAN's redzone
catches the second free. The bounded companion
`examples/perceus/native_variant_real_slot_uaf_747.kai` reproduces the same ASAN
stack in ~30s — wired into the rc-detector corpus and a standalone tier1-native
gate `test-perceus-747-native-real-slot-uaf`.

## The pre-existing bug this lane surfaced but did NOT fix — perceus reuse-in-place

Applying the #1048 scope-exit drop (needed for the record leak) uncovered a
DISTINCT UAF the Real fix does not touch:

```
UAF READ  kaix_internal_dup <- region__rw_expr        (dup of a freed AST node)
freed by  kai_free_value    <- emit_c__expand_ta_expr  (a pass drops the subtree)
alloc     kaix_variant_reuse_at <- modules__rqc_kind   (a reuse-in-place rebuild)
```

Verified: it is a SHARED bug of the front end, not native-only — the C-direct
oracle ALSO corrupts under ASAN-selfhost (heap-buffer-overflow in
`typecheck_module` over a node allocated by `expand_ta_decl` / `kai_variant_u_fast`).
Byte-id never caught it because byte-id compares output, not memory.

Mechanism (asu): `pcs_try_reuse_arm` (`compiler/perceus.kai`) rewrites a variant
via `kai_variant_reuse_at` with Koka MOVE semantics — donate the unique cell,
write the new slots, do NOT incref the old children, assuming the arm exclusively
owns them. But when a walk does `match e.kind { … } → rebuild(e, …)` (the
`rqc_kind` / `expand_ta_expr` / `rw_expr` shape), a SIBLING field read of the same
base takes a second logical ref that perceus does NOT count
(`pcs_consumes_kind(EField(EVar)) = false`). The result: a sub-node with rc=1 and
TWO owners. Any legitimate drop kills it for the other owner (the UAF); without
the drop it leaks (#1048 is the balance of the same missing ref). The commit
`113e2991` ("perf(perceus): reuse-token for typed cells + drop the diagonal
gate") removed the gate that had restricted reuse; `pcs_reuse_is_diagonal`
(perceus.kai) is now orphaned. This wants its own issue + lane: it is
platform-level RC corruption, not a #1054/#1048 detail. Fix direction: reinstate
the diagonal gate, or have the reuse incref a sub-node shared with a live
structure — verified against ASAN-selfhost + byte-id, without regressing the
rb-tree perf `113e2991` targeted.

## Fixtures added

- `examples/perceus/native_variant_real_slot_uaf_747.kai` (+`.out.expected`) —
  the bounded #747 UAF: a variant Real slot consumed by `real_to_string`. Gate
  `test-perceus-747-native-real-slot-uaf` (ASAN + no-cell-pool + output parity)
  and added to `tools/rc-detector-corpus.txt`. Without the dup, ASAN aborts.

## Closing gates (#1054)

1. CRASH-FREE: native self-host gate (compile+link+run+self-compile) + 04_effect.
2. ASAN-ON-SELFHOST: the Real-slot UAF is dead; the reuse UAF stays LATENT (as on
   `main`, masked by the cell pool) — that residual is the separate pre-existing
   bug above, not a regression of this lane.
3. REUSE #872 / #995 intact (the Real-slot dup is inert for them — no Real slots).
4. BYTE-ID: the C oracle is untouched.
