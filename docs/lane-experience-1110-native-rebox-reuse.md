# Lane experience — #1110: native reuse-in-place / TRMC raw-i64 into a reused Int slot

## Scope as planned vs as shipped

**Planned (brief):** the native reuse-in-place / TRMC path writes a RAW untagged
`i64` into a reused variant Int slot, and a downstream tagged dispatch reads it
as a `KaiValue *` → SIGSEGV (`ldr x26,[x26,#8]` with `x26=0x8`) or value
corruption (419 vs 922.25). Brief's proposed fix: a "native mode-slave re-box
pass" so a raw register flowing into a boxed use-site is re-boxed at that
border, mirroring the C oracle's `kai_int(raw)` wrap on the reused-slot write.
The suggested extension point was `nemit_load_reg_boxed` (the existing slot-1
border) or a new pass; the reuse-write was named as where the raw escapes.

**Shipped:** a *different, deeper* fix. The brief's diagnosis (write-side re-box)
was **wrong**: the write already normalises (`nemit_atom_raw` unboxes a boxed
slot value and loads a raw register directly — both produce the right `i64`).
The real bug was a **classifier divergence**: TWO independent verdicts of whether
a match-arm Int binder is "raw" that could disagree. The fix collapses them to
one backend-aware classifier.

## The real root cause (five wrong diagnoses were ruled out first)

The native backend decides a match-arm Int binder's representation in two
places that must agree:

- **`seed_variant_int_binders` (unbox.kai)** seeds the binder's **mode**
  (`MUnboxed`/`MBoxed`). The mode drives how the emitter LOWERS the binder's
  uses — a raw-op binop becomes an `icmp`/`iadd` because the KIR lowered it raw
  FROM the mode.
- **`bind_slot_kslot` (kir_lower_bind.kai)** decided the binder's **KSlot**
  (`SInt64`/`SBoxed`) with a SEPARATE predicate (`binder_read_raw`). The KSlot
  drives how the emitter LOADS the register.

When the two verdicts disagreed for the same binder — e.g. `l == r` lowered raw
from the mode but `l` loaded boxed from the KSlot, or the reverse — the emitter
handed a raw `i64` to a tagged consumer (or a boxed pointer where an `i64` was
expected). That is the `ldr x26,[x26,#8]` SIGSEGV.

The subtlety that made it a MULTI-DAY bisection: `seed_variant_int_binders`
already existed (since Lane B) with the C oracle's **lax** predicate (kind-1 +
not-interp + not-lambda-capture), which trusts the oracle's TOTAL mode-slave
re-box border. The native's load-border is **not total**. So the lax mode
marked a binder raw that the native could not cover, and any second predicate
(`bind_slot_kslot`) that re-derived rawness with different criteria diverged.

## What actually fixed it: ONE backend-aware classifier

1. **`native_arm_binder_raw_safe`** (new module `unbox_native_raw.kai`): the
   strict native predicate. A kind-1 Int arm binder is raw-safe ONLY when every
   use is an operand of a raw-arith/comparison `EBinop` whose SIBLING operand is
   itself a raw-Int form (a var or an Int literal), so the WHOLE binop lowers
   raw. Any other use — a ctor slot, a call arg, a field read, a mixed-operand
   compare, a nested-match pattern — is boxed. Conservative by exclusion via an
   exhaustive `nab_mentions` walk that (unlike `scan_uses_expr`) descends into
   arm PATTERNS.
2. **Backend flag `nat`** threaded from `unbox_pass` (driver passes
   `use_native`) to `seed_variant_int_binders`. Under `nat` the seed applies the
   strict predicate; under C it keeps the lax one. **The rawness of a binder is a
   property of the backend** (what its load-border can cover) — so the predicate
   MUST be a function of the backend, not a backend-blind hack.
3. **`bind_slot_kslot` reads the mode** instead of re-deriving rawness
   (`binder_mode_unboxed` finds the first `EVar(nm)` and reads its `.mode`).
   One verdict governs both mode and KSlot → they can never diverge.

`emit_native_term.kai` also got a defensive `nemit_rc` guard: a `dup`/`drop`/
`incref` over a raw-scalar register (slot 1/2/3, no refcount) is a no-op —
generalising the rule `nemit_drop_assigns_masked` already applied to raw
back-edge params (#709). This closes the phantom-box leak a stale RC plan would
cause over a now-raw binder.

## The one deliberate non-goal: the ctor-slot WRITE-side permit

The rb-tree −5.7% "win" the brief chased splits into two independent uses of the
key binder:

- **READ (descent, the win):** `k < kx` is a raw-op operand → the strict
  predicate makes `k` raw → `nemit_proj_slot` reads the key `.i64` inline. **The
  descent win is recovered by the read-side permit alone.**
- **WRITE (balance rotation):** the ctor-slot `RBNode(..,kx,..)` rebuild is a
  SEPARATE write-side use. Permitting it raw still crashes the native
  self-compile — and, unlike the read path, it has **no reduced repro** (every
  small fixture with a reuse ctor-slot passes; only the full self-compile
  crashes). A lane whose only correctness oracle is a 3-minute self-compile with
  no minimal repro is not incrementally verifiable. So the ctor-slot permit is
  DEFERRED: `native_arm_binder_raw_safe` keeps a ctor-slot use boxed. Follow-up
  #1110 (or a new issue): the first deliverable there is a MINIMAL repro, not the
  fix.

## Structural surprises the brief did not anticipate

1. **The write already normalises.** Verified by disassembly: `int.box(k*2)`
   emits `(k<<2)|1 = tag(2k)` and the reused slot receives `untag = 2k`. The
   brief's "re-box the reuse write" would have been a no-op.
2. **`scan_uses_in_arms` (fnreg.kai) skips arm PATTERNS.** A binder read in a
   nested match pattern is invisible to `scan_uses_expr`, so any predicate
   leaning on it for its conservative catch-all leaked. The new predicate's
   `nab_mentions` walks patterns explicitly.
3. **`scan_uses_kind` has a `_ -> []` catch-all** that drops `EBang`/`ELitUnit`/
   `EIntrinsic` sub-exprs — another source of missed mentions the exhaustive
   walk fixes.
4. **`--emit=kir` defaults to the C backend** (driver init `backend: BkC`), so
   the KIR dump does NOT reflect native modes. Confirming a binder goes raw
   under native requires the real `--emit=native` path, not `--emit=kir
   --backend=native` (which did not thread `nat` through the dump path). Cost a
   false "the fix didn't apply" scare.

## Fixtures added

- `examples/perceus/native_reuse_int_slot_rebuild_1110.kai` (+ `.out.expected`):
  a tree whose Int + Real slots are rebuilt via reuse-in-place, then summed.
  Native must equal C (444) — the value-corruption shape from the issue.
  Wired as `test-native-1110-reuse-int-slot` in `tier1-native.yml`.
- The binary self-compile gate (`test-native-selfhost-gate`) is the SIGSEGV
  oracle; it stays green.

## Real cost vs estimate

Far over the brief's framing. The brief presented a bounded "re-box at the reuse
write" patch. The actual work was a multi-day bisection that ruled out FIVE
plausible mechanisms by static analysis + disassembly (write-side re-box, `projb`
over kind-1, decl-vs-mask mismatch, pattern-walk gap, `operand_raw` sibling
check) before the sixth — the mode↔KSlot classifier divergence — held. The fix
is architectural (one backend-aware rawness verdict), not a patch. `asu` drove
the diagnosis across six rounds; each hypothesis cost a ~3-minute self-compile.

## Follow-ups left for next lanes

- **ctor-slot WRITE-side raw permit** (reuse-in-place rebuild): blocked on a
  reduced repro. The write is verified sound in isolation; the escape only
  manifests in the full self-compile, so the first deliverable is a minimal
  fixture that reproduces it.
- **`km score` of `unbox_native_raw.kai` is B+** (cognitive complexity of the
  exhaustive `nab_mentions` walk). It is a flat, trivial-per-arm match; the score
  reflects arm count, not tangled logic. Above the B floor.
