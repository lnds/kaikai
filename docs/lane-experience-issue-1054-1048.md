# Lane experience — issues #1054 + #1048 (coupled: native variant Real-slot UAF + record scope-exit leak)

## Why the two issues are one lane

#1048 (native leaks heap RECORD values leaving scope) and #1054 (native self-host
SIGSEGV 139 = a use-after-free) are coupled, not independent. The #1048 fix is a
pair of scope-exit `KDrop`s in the KIR lowering; those drops *shorten the life of
the value they reclaim*, and one of them tips a latent borrow into a live free —
the #1054 UAF. So the drops are only sound on top of the borrow/owned fix for
#1054. Shipping #1048 alone is exactly what PR #1049 did, and the 139 it uncovered
forced the revert #1056. This lane ships them together, gated by the one check the
#1049→#1056 cycle lacked: ASAN on the full self-compile.

## Scope as planned vs as shipped

**Planned (brief):** the #747 UAF was hypothesised to be a POINTER-kind
variant slot that `pat_alias_binders` failed to descend — some nested arm shape
`#1049`'s drops exposed. The brief explicitly predicted "Int/Real **no**": the
missing binder would be a pointer, and the dead-let drop would be *safe* (the
UAF would be 100% the EField base-drop's).

**Shipped:** neither prediction held. Step 0 (ASAN-on-selfhost with ONLY the
dead-let drop applied) showed:

1. The **dead-let drop ALSO fires the UAF** — it is not safe in isolation. So
   the hole is not specific to the EField base-drop; both drops expose the same
   latent borrow.
2. The missing binder is a **Real slot**, not a pointer. The exact ASAN stack:

   ```
   READ of size 4 in kai_decref   (arm-exit __pcs_scr_drop cascading into the slot)
   freed by  kai_decref <- kaix_prelude_real_to_string   (real_to_string consumes its arg)
   allocated kaix_real <- build   (the Real box in the ERealN ctor)
   ```

   The shape is `match x { ERealN(r, _) -> real_to_string(r) }`: a variant with a
   `Real` payload slot, the Real binder consumed by a CONSUMING builtin.

## Root cause — an oracle-vs-native representation mismatch on Real slots

The C-direct oracle reads a Real variant slot as a FRESH owning box:
`kai_real(kai_var_slots(scr)[i].r)` bound `is_alias=false` — a brand-new
`KaiValue*` (rc=1) decoded from the raw `.r` double. `real_to_string` decref's
that fresh box; the scrutinee's slot is untouched.

The native reads the SAME slot as a BORROW. The native codegen builds every
variant with `slot_mask == 0` (all payloads boxed as pointers), so a `Real`
payload is a boxed `KaiValue*` in the slot, and `bind_slot_pattern` reads it via
`KProj` → `kaix_variant_arg` → (mask 0) → `.ptr`, **no incref**. That is a borrow
of the scrutinee's own box. `pat_alias_binders` then EXCLUDED Real slots (kind 2)
on the strength of the oracle's fresh-box semantics — so no structural dup
protected the borrow. `real_to_string` decref'd the shared box to zero and freed
it; the arm-exit scrutinee drop then re-decref'd the freed slot → UAF.

Why Real and not Int: `kai_decref` is a no-op for a tagged immediate
(`v->rc == INT32_MAX` early return), and Int slots are tagged `kai_int`. A Real is
always a heap box, so its decref is real and the borrow is fatal.

## The fix — descend Real slots, do NOT reshape KProj or touch the runtime

`pat_alias_binders` now descends Real slots via a new `pa_slot_needs_dup(k)` =
`k == 0 or k == 2`. The owned match arm's existing `lm_dup_alias_binders` then
structurally dups the Real binder exactly as it dups a pointer slot, giving it an
owned ref the arm-exit drop balances — the native analogue of the oracle's
fresh box. The KProj stays a pure borrow (it is NOT given a flag, which would be
a second source of RC truth); the arm that consumes the borrow is what
compensates, mirroring the oracle. The runtime `kaix_variant_arg` is untouched:
its borrow-for-mask-0 contract is correct; the lowering, not the runtime, owed
the dup.

This is why the two prior attempts failed on the wrong axis: (1) a global incref
in `kaix_variant_arg` broke #872's reuse arms; (2) an owned-only fix on direct
pointer slots missed the Real slot #1049's drops exposed. The correct axis is the
per-arm structural dup, filtered by slot kind, in the single collector.

## The SECOND UAF — the EField base drop's timing, not the Real slot

With the Real slot dup'd, the dead-let block-exit drop reapplies cleanly. But the
EField base drop (#1049's second drop) uncovered a DISTINCT use-after-free that
the Real fix does not touch. ASAN-on-selfhost with the Real fix + both drops:

```
UAF READ  kaix_internal_dup <- region__rw_expr        (dup of a freed AST node)
freed by  kai_free_value    <- emit_c__expand_ta_expr  (a pass drops the subtree)
alloc     kaix_variant_reuse_at <- modules__rqc_kind   (a reuse-in-place rebuild)
```

Isolation: revert ONLY the EField drop → ASAN clean. So the EField drop is the
trigger. But it cannot simply be removed — it closes the #1048 field-read leak.
An intermediate attempt (`field_base_is_ephemeral`: drop the base only when it is
NOT a bare variable, mirroring perceus's `pcs_consumes_kind` scoring a field read
of a variable as non-consuming) closed the leak AND the Real fixture but the reuse
UAF persisted — so the offending base is ephemeral, and dropping it is not the
problem.

Root cause (asu): it is NOT a double-drop. It is ONE drop in the WRONG TEMPORAL
position. The native emits `KDrop(bv)` as a SEPARATE KIR statement; the scheduler
can place it AFTER a `kaix_variant_reuse_at` that rewrites the base's cell
in-place. The reuse-at frees the cell's old fields; the late `KDrop(bv)` then
re-frees the reused cell. The C oracle never sees this because it emits the drop
INLINE (`_f = kai_op_field(_b); kai_decref(_b)`), clamped to the read's evaluation
point — before the reuse. `kai_op_field` is borrow-base (it increfs the field, not
the base); the oracle compensates with an external `kai_decref(_b)` in the blob.

The fix: a CONSUMING field-read primitive. `kai_op_field_consume` /
`kai_op_field_at_consume` incref the field AND decref the base in ONE runtime call.
The native lowering emits EField to a single `KPrim("kai_op_field_consume", …)`
and NO separate `KDrop(base)`. The drop now rides the primitive, clamped to the
read exactly as the oracle's inline blob — the scheduler cannot move it past the
reuse. The oracle path (emit_c) is untouched, so the C byte-id self-host holds; the
new prims are additive thunks in `runtime_llvm.c` (+ both `runtime.h` copies).
Detection-in-AST (skip the drop when the base flows to a reuse) was rejected: it
would need the EField lowering to know a SIBLING reuse's plan — a second RC source
of truth with no clean local signal.

Lesson: porting the oracle's "drop the base after the field load" as a separate
KIR statement is unsound in the presence of reuse-in-place. The oracle's atomicity
(read+drop as one expression) is load-bearing, not incidental. A ported drop must
preserve that atomicity — hence the consuming primitive.

## The gate that would have caught #1049 — ASAN on the full self-compile

The 139 does not fire in a small repro under the normal build: the cell pool
recycles the freed box into the next same-size alloc, so the double free hands
back a live-looking cell. It fires in the FULL self-compile (the native-built
kaic2 compiling main.kai, ~128k lines) where the recursion shares the slot's rc.
The gate that catches it: build the compiler's runtime under
`-fsanitize=address -DKAI_LLVM -DKAI_NO_CELL_POOL`, link the native-compiled
`main.o` (bitcode hidden so `runtime_llvm.c` is instrumented) + shim + libLLVM,
and run that native-built compiler self-compiling main.kai. With the pool off,
ASAN's redzone catches the second free. This is the check #1049→#1056 lacked; it
is now the lane's binding gate (~20 min local, run before push).

The bounded companion `native_variant_real_slot_uaf_747.kai` reproduces the same
ASAN stack in ~30 s (no 20-min self-compile), wired into both the rc-detector
corpus and a standalone tier1-native gate, so the regression is caught fast.

## Fixtures added

- `examples/perceus/native_variant_real_slot_uaf_747.kai` (+`.out.expected`) — the
  bounded #747 UAF: a variant Real slot consumed by `real_to_string`. Gate
  `test-perceus-747-native-real-slot-uaf` (ASAN + no-cell-pool + output parity),
  and added to `tools/rc-detector-corpus.txt` (Family A). Without the dup, ASAN
  aborts on the UAF.
- `examples/perceus/native_record_scope_leak_1048.kai` (+`.out.expected`) —
  recovered from the reverted #1049. Gate `test-perceus-1048-native-record-leak`
  (`free_total >= alloc_total-10` over 4000 records).

Both gates wired into `.github/workflows/tier1-native.yml` (now a required check,
so a broken fix cannot merge).

## The five closing gates (all green simultaneously)

1. CRASH-FREE: native self-host gate (compile+link+run+self-compile) + 04_effect.
2. LEAK #1048: `test-perceus-1048-native-record-leak`.
3. REUSE #872: `test-perceus-882/995` (no Real slots, so the dup is inert for them).
4. BYTE-ID: selfhost C byte-id (the C oracle is untouched).
5. ASAN-ON-SELFHOST: full self-compile under ASAN + no-cell-pool, 0 diagnostics.

## Follow-ups

- The oracle-vs-native Real-slot divergence is now bridged by a dup, not by making
  the native mint a fresh box like the oracle. If a future case needs the raw
  `.r` read (e.g. an unboxed-Real arm-body fast path, the `kaix_variant_arg_f64`
  prim that exists but is unused by the lowering), the slot representation, not the
  dup, is the lever — the dup is correct for the boxed-slot representation the
  native builds today.
