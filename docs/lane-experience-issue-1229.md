# Lane experience — #1229: native match-arm Int binder rawness (partial)

## Outcome in one line

Shipped a sound fix for the `bind_slot_kslot`-rescan divergence class (a real
hole), but instrumentation proved that class is **not** what the CI `#1229`
crash hits. The crash is the #1110-**deferred** reuse/drop write-side, which
only reproduces on the x86 CI runner. So: `refs #1229`, workaround kept, issue
stays open for a dedicated lane. Two distinct bugs share the crash signature.

## Scope as planned vs as shipped

**Planned:** close #1229 — a kind-1 Int match-arm binder in a hot `Ty` walker
dropped as a pointer (raw i64 as boxed) → SIGSEGV, runner-only. Two directions
offered: (A) collapse the rawness verdict so unbox-seed and KSlot can't diverge;
(B) no-op the KDrop over an effectively-raw register.

**Shipped:** (A) in the "carry, not recompute" form — but scoped honestly to the
class it actually fixes, NOT as closing #1229. The Shape workaround
(`canon_tvars_shapeapp`) is kept, because reverting it re-exposes the *other*
bug and the native self-compile SIGSEGVs on CI.

## What (A) is, and why it is sound

`bind_slot_kslot` claimed to read the unbox seed's verdict but actually
**re-derived** rawness with `binder_mode_unboxed(nm, body)` — an existence-scan
for an `EVar(nm)` carrying `MUnboxed` over the **perceus-mutated** body. When
perceus rewrites away a binder's `MUnboxed` reads, the scan says boxed while the
seed made the alloca raw → KSlot/representation desync. The fix decides the
verdict once (`native_raw_arm_binders`, unbox.kai, same predicate as the seed),
emits a position-keyed side table (`RawArmBinder = RAB(name, line, col)` — a
`PBind`'s line/col survive perceus and tcrec untouched), threads it via
`LowerSt.raw_binders`, and `bind_slot_kslot` reads it by position. The whole
`evar_mode` scan family (~90 lines) is deleted. This is a genuine divergence
killer; it just is not the crash below.

## Why (A) does NOT close the CI crash — the instrumentation

The CI gdb stack is identical to the issue's original: `infer.canon_tvars_ty
+4639`, inlined `kai_decref` (`mov %eax,(%rdi)`), `rdi` = a libc/malloc address
(garbage), `rax` = 264275271 (absurd rc). A raw i64 is decref'd as a pointer.

Instrumenting SEED (`seed_variant_int_binders`) vs the table
(`native_raw_arm_binders`) vs `bind_slot_kslot` over the real self-compile
showed: **every arm binder of `canon_tvars_ty` — `n`, `b`, `sv` — is boxed, and
SEED, table, and KSlot all agree.** The KIR confirms: `n: box = projk t.0/1`,
`b: box = proj .t2.0`, `sv: box = projk t.0/1`. So (A) does not change
`canon_tvars_ty`'s codegen at all — same as without the fix. The raw i64 being
decref'd is **not an arm binder**.

## Where the crash actually is (traced, not fixed)

Ruled out by reading the runtime:

- **Reuse write-side** — `kaix_variant_reuse_move_i64` / `kai_variant_at`
  overwrite slots WITHOUT decref'ing old ones (move semantics); the old children
  moved to binders. Not here.
- **Reuse token** — `kai_drop_reuse_token` does a non-recursive `rc--`, never
  touches children. Not here.
- **Scrutinee whole-cell drop** (`kaix_internal_drop(.t28$sd)` on the
  `Some[Int]` result of `intpair_lookup`) — `Some`'s payload is kind-0 boxed
  (`b: box = proj`, generic Option), so that drop is correct.

The live candidate that remains: the raw temporary `.t5 = int.unbox(b)` written
into a ctor Int slot (`con TyVarT #256 {i64:.t5}`) in the trmc/reuse path — the
**ctor-slot WRITE-side raw permit** that the #1110 retro (docs/lane-experience-
1110-native-rebox-reuse.md, "Follow-ups") DEFERRED, noting it "still crashes the
native self-compile" and "has no reduced repro — only the full self-compile
crashes." `canon_tvars_ty` with the inline `TyShapeApp`/`TyVarT` arms IS that
missing repro, but it maps to an x86 offset a mac (ARM64) build cannot produce.

## The workaround, and why it is kept

`canon_tvars_ty`'s `TyShapeApp(sv, arg)` arm routes through
`canon_tvars_shapeapp(sv, arg, m)`, keeping the kind-1 `sv`/`b` out of the hot
match's arms. This sidesteps the reuse/drop write-side bug (the arm that
triggers it never forms). It is kept until the write-side lands. Reverting it +
(A) still SIGSEGVs on CI — the proof (A) alone is insufficient.

## Fixtures

- `examples/perceus/native_arm_int_binder_walker_1229.kai` (+ `.out.expected`
  = 5313): a `Ty` walker whose `TShape(sv, arg)` arm binds a kind-1 Int
  sub-binder used raw in arithmetic (`sum_score`) and comparison (`count_pos`).
  Native must equal C. It exercises (A)'s class (a raw arm binder lowering
  correctly), NOT the reuse write-side crash. Wired as
  `test-native-1229-arm-int-binder` in `tier1-native.yml`.

## Gates

- Native self-host gate GREEN **with the workaround** (COMPILE + LINK + RUN +
  SELF-COMPILE, byte-id C). Without the workaround it SIGSEGVs on the CI runner
  (the deferred write-side).
- Byte-id self-host (C) GREEN + deterministic — the change is native-only.
- Fixture native == C (5313).
- rc-detector `c/` UBSan failures are pre-existing macOS `runtime.h:2333`
  `__int128` artifacts (identical on clean HEAD), green on Linux CI.

## Follow-ups (the real #1229)

- **Reuse/drop ctor-slot WRITE-side raw permit** — the #1110-deferred bug.
  First deliverable: an **x86 repro** mapping `canon_tvars_ty+4639` to a concrete
  arm (the trmc/reuse ctor-slot write of `TyVarT`/`TyShapeApp` with a raw Int
  slot). Without that disasm, further digging is blind 30-min CI cycles. Once the
  arm is known, remove the `canon_tvars_shapeapp` workaround as the closing test.
- Remove the now-vestigial `body` parameter from the `bind_pattern_fields` /
  `bind_slot_pattern` chain (dead since the body scan was deleted).
