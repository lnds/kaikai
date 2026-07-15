# Lane experience — #1229 write-side: stale `mscr` leaks a nested arm's raw scalar into a sibling arm's exit drop

## Outcome in one line

The #1229 CI SIGSEGV was NOT the reuse/drop *emission* the read-side retro
predicted; it was a **KIR-lowering** bug — a nested match's owned-scrutinee
exit-drop register (`<join>$sd`, tracked in `LowerSt.mscr`) leaked into sibling
arms, so a reuse-fork sibling's exit drop freed the wrong register: a raw `i64`
temporary from the nested arm dropped as a pointer. Restoring `mscr` per group
in `lower_variant_groups`/`lower_group_chain` fixes it; the Shape workaround
(`canon_tvars_shapeapp`) is reverted, and native no longer crashes on the x86 CI
runner. #1229 closes.

## Scope as planned vs as shipped

**Planned (brief):** extend the read-side single-source rawness verdict (#1233)
to the WRITE side — make the reuse/drop of a variant slot consult the same
backend-aware verdict, so an `int.unbox` raw temporary written into a ctor Int
slot is never dropped as if boxed. Prove it by reverting `canon_tvars_shapeapp`
and confirming native goes green on CI.

**Shipped:** the *proof* held (revert + green), but the fix is NOT an extension
of the rawness verdict. The verdict was already correct — `native_arm_binder_
raw_safe`, `nemit_store_reg_boxing` (#1156), and `kval_is_raw_scalar` all agree
on which registers are raw. The bug was upstream of all of them, in the arm-
lowering's `mscr` bookkeeping: the register a reuse-fork arm drops was chosen
wrong. So the fix is one restore of `mscr` per group, not a rawness change.

## The x86 disasm — the first deliverable both prior retros demanded

The retro for #1229 said the first deliverable was an **x86 faulting-arm
disasm** (the crash is heap/stack-layout dependent, mac ARM64 does not
reproduce it). Delivered via a temporary `gdb` step in the `build-native` job
(`tools/diag-1229-writeside.sh`, since removed) that links the native-built
kaic2 and runs it under `gdb -batch` over the self-host sample:

```
=> canon_tvars_ty+4639: mov %eax,(%rdi)    ; inlined kai_decref, CRASH
   rdi = 0x7ffff00ad812  (a malloc text address — an i64 raw, not a pointer)
   rax = 264275271       (absurd rc)
   #1 kai_singleton_unit
```

Two paths reach that decref block: a **reuse** path (`movw $0x103,0x6(%r14)`
retag → `%rdi = &kai_singleton_unit`, a no-op decref of the immortal unit) and
a **fresh** path (`mov 0x18(%rsp),%rdi`). The crash is the FRESH path:
`0x18(%rsp)` is a shared stack slot that in another arm held a raw `i64`.

## Root cause — a shared `mscr` register across sibling arms

`str.46119` resolved (via the object's cstring section) to **`TyRefineT`** —
tag 259, a 2-slot ctor with NO Int slot. So the crashing arm has no raw binder
of its own; the raw `i64` came from elsewhere. The real KIR (dumpable via
`./kaic2 --emit=kir --path ../stdlib main.kai` from `stage2/`; the bundle-concat
`--emit=kir` fails on the module-doc guard, the modular path works) showed why:

- Arm **`TyVarT`** (`Var(n) -> Some(b) -> TyVarT(b)`) emits `.t5: i64 = prim
  int.unbox(b)` — a raw `i64` temporary for the rebuilt cell's kind-1 Int slot.
  Its NESTED `match intpair_lookup` runs `match_preamble`, which sets
  `mscr = .t28$sd` (the nested match's own `<join>$sd`).
- Sibling arms **`TyDimT`** and **`TyRefineT`** rebuild via a reuse fork
  (`condbr kaix_check_unique`) with NO nested match — so no `match_preamble`
  resets `mscr`. They inherited `mscr = .t28$sd` from `TyVarT`, and their
  `reuse_neutralise_exit_drop` / exit `drop` operated on `.t28$sd` instead of
  the outer scrutinee `.t1$sd`.

`.t28$sd`'s alloca and the raw `.t5` temporary aliased the same stack slot once
the `TyShapeApp` arm's raw Int binder `sv` perturbed the function's RSpec — so
the sibling arm's `drop .t28$sd` freed the `TyVarT` arm's raw `i64`. The old
`match_preamble` comment claimed "a sibling/outer match re-sets `mscr` in its
own preamble … no explicit restore is needed" — true only when every sibling
arm HAS a nested match. A reuse-fork arm has none, so the claim was false.

## The fix

`lower_variant_groups` now takes `mscr0` (this match's `mscr` as of its
preamble) and restores it before each group; `lower_group_chain` restores it
before each arm of a shared tag. One `ls_set_mscr(st, mscr0)` per arm boundary.
After the fix the KIR shows `TyDimT`/`TyRefineT` dropping `.t1$sd` (the outer
scrutinee) — the correct register — while `TyShapeApp` keeps its own `.t28$sd`.
Native == C, and the reverted `TyShapeApp` arm no longer crashes.

## Why the C backend never saw it

Same KIR, both backends. In C, `.t28$sd` is a function-local `KaiValue *`; its
stale value is a valid boxed pointer (or the unit singleton) under the C RC
layout, so the errant `drop` is a benign no-op or double-decref that the pool
allocator absorbs. Only the native flat register table + LLVM stack-slot reuse
lands a bare `i64` in that slot, and only the x86 runner's layout makes that
`i64` look like a live pointer. Byte-identical C self-host stayed green
throughout — the change is native-observable only, but it is a real lowering
bug that C got lucky on.

## Fixtures

- `examples/perceus/native_reuse_mscr_stale_1229.kai` (+ `.out.expected` = 187):
  a `Ty` walker with a raw-Int-binder nested-match arm (`Var`/`Shape`) beside a
  reuse-fork sibling arm with no nested match (`Dim`/`Refine`). Exercises the
  stale-`mscr` shape the read-side fixture (`native_arm_int_binder_walker_1229`)
  and the #1110 reuse fixture did not. Wired as `test-native-1229-writeside`.

## Gates

- Native self-host gate GREEN **with `canon_tvars_shapeapp` reverted** — the
  proof the underlying hole is closed (this bug reproduces ONLY on the x86 CI
  runner, so CI is the oracle).
- Byte-id self-host (C) GREEN + deterministic (`kaic2b.c == kaic2c.c`).
- Fixtures `test-native-1229-writeside`, `test-native-1229-arm-int-binder`,
  `test-native-1110-reuse-int-slot` all native == C.
- No runtime change — the fix is pure KIR lowering, so runtime.h / runtime_llvm.c
  are untouched (both backends share the corrected KIR).

## Follow-ups

- The `mscr` restore now guards `lower_variant_groups` and `lower_group_chain`.
  The lit/record match paths (`lower_lit_arms`, `lower_record_arms`) thread `sd`
  directly and have no reuse-fork-sibling shape today, but if one ever forms
  there, the same per-arm restore applies — a one-line change mirroring this one.
- Remove the vestigial `body` parameter from the `bind_pattern_fields` /
  `bind_slot_pattern` chain (dead since #1233's body scan was deleted) — carried
  over from the read-side retro, still open.
