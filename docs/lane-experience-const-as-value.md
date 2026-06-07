# Lane experience: `const` as a first-class value + effect-row contract purity

## Scope as planned vs as shipped

**Planned (start of session):** the daily was failing; diagnose and fix.

**Shipped:** the diagnosis unrolled three nested layers of the same
anti-pattern — *enumerate by hand what the system already knows* — and the
lane grew (with explicit user direction at each step) into removing all
three:

1. **Daily blocker (fixed first, committed as `2d891f5`).** `stress-fixtures`
   compiled kaic2-emitted C with `-I stage0` only; the stage-2 emit uses
   stage2-only runtime symbols (`kai_intf`, `kai_register_payload_ctors`,
   tagged-int / slab / reuse helpers). Hard error on Linux/gcc, only a
   warning on macOS/clang — so it passed locally and failed in CI. Fix:
   `-I stage2 -I stage0`. Audited every `cc`/`clang` call site; the rest of
   `stage2/Makefile` is already protected by `CPPFLAGS_CORE := -I .`
   resolving `runtime.h` to stage2/ first. Two latent twins in
   `demos/vs` + `demos/9d9l` (which also still passed the retired
   `--prelude` flag and lacked `-lm`) fixed too.

2. **`[<refinement_pure>]` is redundant with the effect system.** A contract
   predicate (`requires`/`ensures`) could only call a hand-maintained
   whitelist of 9 names in `refinements.kai`, or fns opt-in-marked with the
   `[<refinement_pure>]` attribute — which did NOT propagate cross-module
   (so no stdlib fn was ever usable in a contract) and broke `pub` export as
   a side effect. Replaced the whole mechanism with: *a predicate may call
   any effect-pure fn*, read directly off the callee's declared effect row
   (`REmpty`). The typer already rejects a `REmpty` signature whose body
   performs an effect, so reading the row is sound; `merged` decls carry
   prelude + every imported module, so cross-module resolves for free.
   Deleted the attribute, its parsing, `refinement_pure_names`,
   `user_pure`, `extract_pure_names`, and the `DAttribPure` AST variant
   entirely (70 references, cache encode/decode, 45 propagation arms).

3. **`const` is disguised as a zero-arg function.** Issue #269 lowered every
   `const NAME : T = lit` to a `DFn`-thunk + rewrote every `NAME` reference
   to `NAME()`. This dragged: an EVar→ECall rewrite pass (~265 lines), a
   const-name collection pass, the `DAttribPure` wrapper (reused as a
   "this is pure" marker), and an early lowering for `pub`-enforcement
   (#510) because `DConst` had no `module_origin` slot. Replaced with: a
   const is a value. `DConst` gains `module_origin` and survives intact
   through the whole pipeline; the typer registers `name : T`
   monomorphically (not `() -> T`); both backends materialise a const read
   as its **literal body inline** — no global, no boxing, no call. The unbox
   pass then treats it exactly like the literal it stands for.

Net: **~−450 lines** (more ad-hoc machinery deleted than code added).

## Design decisions and alternatives considered

- **Contract purity: name-list vs effect-row.** The user's framing was
  decisive: kaikai already models purity in the type system (empty row =
  pure), so a parallel name list is pure redundancy. The only nuance is
  totality (a `REmpty` fn can still panic/diverge); kaikai does not model
  `Div` as an effect today (unlike Koka), so "empty row" captures
  *observable-effect purity*, which is what a contract check actually needs.
  If `Div` ever enters the row, the same gate excludes it automatically —
  forward-compatible. (Open discussion the user flagged for later: should
  kaikai model divergence as an effect, Koka-style.)

- **Const emit: global+init vs inline literal.** First attempt emitted a
  `static KaiValue *` global + a `_kai_init_consts()` initialiser. The user
  asked the load-bearing question — "why would you box a constant?" — and
  it collapsed: a const body is *always a literal*, so there is no heap
  identity to preserve. Inline is semantically exact and lets the unbox
  pass optimise const arithmetic to raw C (the global-boxed version
  regressed `math_real_basic` with `type mismatch in *` because the boxed
  const broke unbox). Inline modelling: `EmitCtx.consts` / `LlvmEmit.consts`
  carry the const decls (same role `fns`/`variants` already play), and the
  EVar value arm materialises the body.

- **`find_lam` keyed on `(enc_fn, line, col)`, not `(line, col)`.** A latent
  pre-existing bug: lambdas were looked up by source position alone, which
  collides under the multi-module selfhost emit when two modules have a
  lambda at the same line/col (`emit_c.kai:1340:25` vs
  `monomorph.kai:1340:25`). Deleting 389 lines in `desugar.kai` aligned
  those two by accident and surfaced it. Fixed at the root: `LamInfo` gains
  the enclosing-fn symbol (already module-mangled), `find_lam` keys on it,
  and `EmitCtx`/`LlvmEmit` thread `cur_enc_fn` (set in fn / lambda / test /
  bench / check body emit, mirroring the collector's `enter_enc_fn`).

## Structural surprises the brief did not anticipate

- The daily diagnosis bottomed out in a *language-design* problem, not a
  build problem. Each fix exposed the next layer.
- `DConst` was already *half-migrated* to a first-class value: the resolver,
  typer (partial), and emit had `DConst` arms, but the function disguise sat
  on top. Someone had started the right design and stopped.
- The const-in-pattern desugar (`match n { ZERO -> ... }` → equality guard)
  was tangled into the deleted `desugar_const_refs_*` family and got removed
  with it. Restored as a focused `desugar_const_patterns_decls` pass that
  generates `__cv == NAME` (EVar, inlined) instead of the old `NAME()` call.
- `const_pattern` regressed to a segfault (an uppercase const name parses as
  `PVariant`, and without the desugar it ran `kai_variant_name_of` on a
  tagged Int). The smoke suite caught it; the pattern pass fixed it.
- A mid-lane branch switch by another agent dragged the uncommitted WIP onto
  an unrelated docs branch. Recovered with a full patch backup + selective
  stash; zero loss, verified byte-for-byte against the backup.

## Fixtures added and coverage gaps

- `m12_6_pure_attr_ok` / `_inline` / `_no_collision`: rewritten from
  attribute tests to effect-row purity tests (user pure fn, transitive
  purity, generic-return coexistence). `_inline` now covers transitive
  purity in one file (the cross-module case lives in stdlib, not the
  self-contained sugars harness).
- `m12_6_contract_effectful_rejected` (new, negative): a predicate calling a
  `/ Console` fn is rejected with the new diagnostic.
- `m12_6_pred_pure_user_fn` (new, positive): replaces `m12_6_pred_impure_neg`
  (deleted — it asserted the *wrong* behaviour, rejecting a pure user fn
  because it was absent from the hand-list).
- Gap: no dedicated cross-module const-as-value fixture in a harness that
  passes `--path stdlib` (manually verified `math_real_basic` exercises it).
  The backend-parity harness covers `examples/sugars` const fixtures both
  backends.

## Validation

Light local: selfhost byte-identity OK; test-blocks (`capture` — the
lambda-resolution regression — and quicksort) OK; all lane const +
refinement fixtures green. Full suite delegated to CI.

## Real cost vs estimate

No time estimate was made (per project discipline). The lane was far larger
than "fix the daily" — three deletions of load-bearing machinery, an AST
variant removal, a typer-registration change, a dual-backend emit rewrite,
and a root-cause fix to lambda resolution. The recurring lesson: *when the
fix is to delete a hand-maintained list and ask the system instead, the
deletion ripples — follow every consumer.*

## Follow-ups left for next lanes

- Const arithmetic emits boxed when inlined into a boxed-mixed expression
  (e.g. `2.0 * PI` → `kai_op_mul(kai_real(2.0), kai_real(3.14159))`).
  Correct, but the old function-disguise let the unbox pass produce raw C
  (`2.0 * 3.14159`). Re-seeding the inlined literal's `mode` from the use
  site would recover the raw path. Efficiency only, not correctness.
- Divergence-as-effect (Koka `div`) is the open design question: would make
  "empty row" mean *total*, tightening contract purity to exclude
  panicking/diverging predicates. Deliberately deferred.
- A cross-module const fixture in the stdlib/modules harness.
