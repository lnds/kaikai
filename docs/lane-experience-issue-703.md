# Lane experience — issue #703: double `match v` on a let-bound variant in a UFn body

**Scope:** fix the C-backend `non-exhaustive match` panic (a Perceus
use-after-drop) triggered by two consecutive `match v` reads of the
same let-bound variant value. LLVM was correct; C was wrong — this
inverted the "C is the oracle" premise and was carved out as Cluster F
of the LLVM↔C parity triage (`docs/llvm-parity-plan-2026-05-26.md`).

**Layer the bug was in:** the Perceus pass (`stage2/compiler/perceus.kai`),
NOT the C emitter. The C emitter was honoring a contract the Perceus
pass failed to satisfy.

## Scope as planned vs as shipped

Planned: diagnose layer (a) Perceus drop placement vs (b) C emitter
mis-lowering, fix the correct layer, keep `v` live across both reads,
no RC-budget regression, add a regression fixture, remove the parity
skip. Shipped exactly that, with the root cause one level deeper than
the issue's hypothesis: the hypothesis said "drop emitted too early /
missing dup"; the actual cause was that **Perceus skipped the entire
function body** because the function had an unboxed (UFn) signature.

## How the layer was proven

The fix discipline (CLAUDE.md) is "don't patch the wrong layer." I
proved the layer with the strict RC report (`-DKAI_TRACE_RC=1`) on the
existing fixture, before and after:

- **Before fix:** `alloc_total=5 free_total=2 leaked=3`, `tag variant
  allocs=1`. `free_total=2` is the smoking gun: the single variant was
  decref'd **twice** — once per `match v`. The match emitter
  (`emit_match_default`) ends every match with `kai_decref(_scr)`,
  consuming its scrutinee. With two matches reading `v` raw, the first
  decref freed `v` (rc 1→0), the second dispatched on freed memory →
  the tag matched no arm → `non-exhaustive match`.
- **After fix:** `free_total=1`, the variant is freed exactly once;
  `leaked` rises 3→4 only because one more cons-list cell now survives
  (the known #297 tail-position residual), strictly better than a UAF.

Reading the generated C confirmed the mechanism directly: both
`match v` scrutinees were the bare `kai_v` (no `__perceus_dup`), so
each match's exit `kai_decref(_scr)` hit the shared reference.

## Root cause

`perceus_decl` had an early-return for UFn decls (line ~391):

```kai
else if prc_is_ufn_decl(fns, name, mo) { d }   // skip the WHOLE body
```

The justification was sound for *parameters*: a UFn's params are raw C
scalars (`int64_t`), so dup/drop against `kai_<pname>` would name a
non-existent boxed alias. But the early-return skipped Perceus for the
**entire body**, and a UFn body can still bind **boxed locals** — here
`let v = A([..])`. The unbox pass only marks params/return unboxed; the
local variant stays `MBoxed` and needs the normal
dup-at-non-last-read + exit-drop discipline. The skip threw the baby
out with the bathwater.

`fn f(seed: Int) : Int` qualifies as a UFn (scalar params, scalar
return), so its body — including the multi-read boxed `v` — never saw
Perceus. Any UFn whose body binds a boxed local read ≥2 times had the
same latent bug; the cons-list payload (non-immortal, per #304) is just
what made it observable here.

## The fix

Replace the UFn early-return with a body rewrite that uses an **empty
param scope** (mirroring the existing `DTest` path):

```kai
else if prc_is_ufn_decl(fns, name, mo) {
  let uses = pcs_collect_uses_expr(body, [], false, [])
  let new_body = pcs_rewrite_expr(body, [], uses, false, [])
  let arm_dropped = pcs_arm_drop_pass(new_body, name, [], uses)
  let reused_body = pcs_recognise_reuse_expr(arm_dropped)
  DFn(..., reused_body, ...)
}
```

With `[]` as the param scope: raw params stay untouched (they never
enter RC scope, so no spurious dup against `kai_<pname>`), and
`pcs_prepend_unused_drops` is skipped (no boxed params to reclaim). But
in-body `SLet` bindings enter the scope **during** the block walk
(`pcs_rewrite_block` → `prc_pat_bindings_skip_raw`), so `v` gets its
`__perceus_dup` on each non-last read and its block-exit `__perceus_drop`.
Post-fix C: both matches read `kai_internal_dup(kai_v)`; the original
ref is reclaimed by the exit drop.

This is also the correct behavior for LLVM — LLVM was right by accident
(its RC discipline on this path is absent / different), not by design.

## Why LLVM was accidentally right

LLVM's `if`-lowered match path on this shape did not emit the
scrutinee decref that the C emitter does, so it never freed `v`
prematurely — it leaked instead of crashing, and the leak is invisible
to an exit-code/stdout check. The fix makes both backends correct for
the same reason (dup-balanced reads), not just C.

## RC-budget verification

The new fixture (`double_match_same_variant.kai`, both matches in
non-tail `let _` position so the exit drop is not TCO-skipped) reports
under `-DKAI_TRACE_RC=1`: `tag variant allocs=1`, variant freed once,
i.e. **variant live = 0**, the #297 invariant. No RC regression: the
variant goes from double-freed (UAF) to freed-once. The residual
cons-list leak is the pre-existing #297 tail-scope limitation,
identical to the existing `passthrough_let_dup_drop` fixture.

## Fixtures added

- `examples/perceus/double_match_same_variant.kai` + `.out.expected`
  (`0`) — the minimal `let _ = match v; let _ = match v; 0` shape that
  frees the variant. Wired into `test-perceus-issue703` (golden) and
  `test-perceus-issue703-asan` (the real gate — a too-early drop is a
  UAF, a too-late one a leak; ASan catches both). Both folded into the
  stage2 `test` aggregate (→ root `tier1`) and root `tier1-asan`.
- Removed `passthrough_let_dup_drop` from `tools/backend-parity-skips.txt`;
  it now passes the C-vs-LLVM parity check.

The exact issue repro (second `match v` in tail position) also exits 0
on both backends now; it leaks the variant by one (the #297 tail-skip)
but no longer crashes, so it is not the golden fixture — the non-tail
form is, because it pins the stronger "freed once" invariant.

## Structural surprises

1. The issue hypothesis pointed at drop *placement*; the real cause was
   a whole-body *skip* gated on the function's signature shape. The
   instrumentation that found it: a temporary eprint in `perceus_decl`
   printing `is_ufn` per decl — `f` printed `UFN-SKIP`, and no `[DBG
   slet] name=v` line ever appeared, proving the body was never walked.
2. The UFn skip predates the #297/#342/#350 exit-drop machinery; those
   lanes added drop discipline to the non-UFn path and never noticed
   the UFn path bypassed all of it. Any boxed local in a scalar-signature
   fn was unprotected.

## Cost vs estimate

~2.5 h. Diagnosis (RC report before/after + C reading + the two
instrumentation rebuilds) was the bulk; the fix is 6 lines mirroring an
existing path. The kaic2 rebuild-per-iteration (~90 s each) dominated
wall time.

## Follow-ups left

- The #297 tail-position exit-drop skip (lines ~949-956 of perceus.kai)
  still leaks a binding read in tail position to preserve TCO. Not in
  scope here; the broader audit stays under the `perceus` label.
- Worth a sweep: are there other passes (tcrec, arm-drop, reuse) that
  also early-return on UFn decls and would now need the same
  empty-scope treatment for boxed locals? This lane only touched
  `perceus_decl`; selfhost byte-identical + tier1 green suggest no
  collateral, but a deliberate audit would close the question.
