# Lane experience — issue #1201: native SIGSEGV on the layout-rewrite walk

## Scope as planned vs as shipped

**Planned (issue brief):** the native-built self-hosted compiler (`kaic2-native`)
SIGSEGVs while emitting C for any Layout-bearing program. The stated hypothesis
was the *raw-binder-into-boxed-consumer* pattern of the native boxing border,
pointing at the `lr_one`/`lr_pair` constructor-lambdas in `layout_rewrite.kai`
(`_kai_lam_28` on the crash stack). Fix the rawness verdict *in general*, add a
CI gate that runs `kaic2-native --emit=c` over a Layout input vs the oracle.

**Shipped:** the crash was NOT a boxing-border rawness bug and NOT in
`layout_rewrite.kai` at all. The root cause is a **use-after-free in the native
KIR lowering of an owned guard-chain match** — `lower_guard_arms` bound the
whole-value arm binder as an alias of the scrutinee but never emitted the
structural pointer-slot incref that every other arm path (`lower_default_arm`,
the variant groups) already emits. The match-exit `KDrop(sd)` then freed the
cell the binder transfers to the result. The fix is one edit in
`stage2/compiler/kir_lower_walk.kai`; `layout_rewrite.kai` is untouched. Plus the
CI gate the brief asked for.

## The investigation — why the hypothesis was wrong

The offset-16 NULL read (`EXC_BAD_ACCESS at 0x10`) is real: offset 16 is slot 1
of a variant node's inline slot array (`&v->as` at offset 8, slot 1 at +16). The
walk reads slot 1 of an `ExprKind` (`ECall`'s `args_`, or a sub-`Expr`'s `.kind`
via a record field). So the *symptom* pointed straight at the walk.

But the walk was the *victim*, not the culprit. The native lowering freed the
`Expr`/`ExprKind` cell early; every later read of it — slot 1 of a variant
(SIGSEGV at 0x10) or `.kind` of a record (a clean `field access on non-record`
`exit(1)` once the crash class shifted) — saw freed memory.

The tell was the sibling walk `dsg_map_expr_kind` (desugar.kai): structurally
identical, runs in the native self-host, does NOT crash. The difference is not
the constructor-lambdas — an early spike that rewrote `lr_kind` to build each
`ExprKind` inline (dropping `lr_one`/`lr_pair`) turned the SIGSEGV into
`field access on non-record`, i.e. moved the crash class but did NOT fix it. That
was the decisive negative result: the bug survives the lambda rewrite, so the
lambdas were never the cause. That spike was reverted; the real difference is
that `lr_expr`'s outer `match lr_children(e, lt) { ce -> ... }` is an **owned,
single-arm, all-binder match** — exactly the shape that routes to
`lower_guard_chain` → `lower_guard_arms`, the one arm path missing the #858 incref.

The C-direct oracle never hits `kir_lower`, so it always emitted the
`is_alias=true` incref (`emit_c.kai`): "the C backend works" was the KIR/native
path diverging from the oracle, not the feature being backend-specific.

## Design decisions

- **Fix at the lowering, not the walk.** The walk is correct kaikai; making it
  dodge the bug (inline constructors, restructured matches) would leave the
  UAF live for the next owned all-binder match over a call result. The fix
  restores oracle parity in `lower_guard_arms` — general, not a layout patch.
- **Mirror `lower_default_arm` exactly.** `lower_arm_rc(p, body, sd, owned, st)`
  + `match_selftail_scr_drop(...)`, the same two calls the default-arm path
  already makes, threading `owned`/`sd` down from `lower_guard_chain`. No new
  RC machinery — the existing #858 dup path is simply now reached from the
  guard-chain too.
- **Reverted the `layout_rewrite.kai` spike.** One lane, one fix: the RC fix
  alone makes the repro byte-identical to the oracle, so the lambda rewrite is
  unnecessary and out of scope. (The inline form is arguably cleaner, but that
  is a separate cleanup, not this fix.)

## Structural surprises the brief did not anticipate

- The brief's rawness hypothesis was a red herring; two subagent sweeps
  independently ruled out the boxing border and the slot-index paths and
  converged on the missing alias incref. The offset-16 clue was accurate about
  *where the deref faulted* but not about *what produced the NULL*.
- `lower_guard_arms` is the ONLY arm-lowering path in `kir_lower_walk.kai` that
  omitted `lower_arm_rc`. Every sibling (default, variant single, variant group)
  had it. It went unnoticed because guard-chain matches whose binder result
  outlives the scrutinee-drop are rare in the compiler's own hot paths — the
  layout walk is the first to exercise it under the native self-host with data
  that frees the aliased cell before the read.

## Fixtures added

- Extended `tools/test-native-selfhost-gate.sh`: after its trivial-sample
  SELF-COMPILE, it re-runs the SAME native binary over a Layout-bearing program
  (`examples/sugars/kinds_layout_encode_decode.kai`, non-empty `layout_types`),
  forcing the full layout-rewrite walk under the native backend, and asserts
  byte-identical C vs the oracle. This closes the gate's blind spot: its sample
  (`println`) has no Layout type, so `rewrite_layout_calls` early-returns and the
  walk never ran.
- Folded INTO the existing gate rather than a separate step/script — the first
  cut was a standalone `test-native-selfhost-layout.sh` that rebuilt kaic2 +
  main.o + link from scratch, doubling the native build in the same job and
  blowing the 25-min budget (CI timeout on PR #1203). Reusing the binary the
  gate already links costs a `--emit=c` + diff (seconds), not minutes. Also
  bumped `build-native` timeout 25 → 35 as headroom.

Coverage gap: the gate is native-only (SKIPs without libLLVM), so it lives in
`tier1-native.yml`, never in tier1's `TEST_LIGHT_TARGETS` (tier1 runs on Linux
without LLVM).

## Verification

- Repro (`kaic2-native --emit=c` over the Layout fixture): rc=139 → rc=0,
  byte-identical to the oracle (7588 lines).
- Minimal repro reduced to `type H = { m: U32<be> }` + `fn main = println("x")`
  — declaring a Layout type is enough (populates `layout_types`); no
  encode/decode needed. Control without `<be>`: rc=0.
- Native self-host gate held at baseline 0. Byte-id selfhost + tier0 local; the
  RC change carries serial parity to CI (`BACKEND_PARITY_JOBS=1`).

## Follow-ups

- None required for the fix. The RC change is general: any owned single-arm /
  guarded all-binder match over a call result now gets its alias incref. If a
  future audit wants it, the `layout_rewrite.kai` `lr_one`/`lr_pair` →
  inline-constructor cleanup (the reverted spike) is a low-risk readability
  change, independent of correctness.
