# Lane experience — issue #1179 gap 1: C backend erases State for loop-local vars

## Scope as planned vs shipped

Planned (issue #1179 gap 1): make the C backend erase the dispatched
`State` handler for a local `var` whose only "escape" is the closure the
`while`/`until`/`repeat` combinator wraps around its body — the same
result the native backend already gets from the KIR inliner (#1158) +
post-inline var→slot (#1173), but on the C backend's AST path.

Shipped exactly that, via a new pre-resolve pass rather than a port of
the KIR machinery:

- `stage2/compiler/loop_lower.kai` — rewrites a literal
  double-trailing-lambda combinator call into
  `EIntrinsic("__loop_*", [original_callee, lambda_args...])`, gated to
  the C backends (`BkC`/`BkCModular`) in the driver, excluded for
  `--emit=kir` and `--dump-purity`.
- The lambdas stay in the AST as wrappers; the C emitter pastes their
  bodies into an inline C loop in the enclosing frame. With no capturing
  closure left, the existing `desugar_var_decls` slot specialisation
  fires unchanged and the var lowers to a stack slot.
- Gap 2 (slot→register) is untouched and stays open in #1179.

## Design decisions

**Route A shape: intrinsic + wrapper lambdas, not a KIR port and not
lambda-lifting.** Porting the KIR path meant reimplementing a general
inliner + beta reduction + canonical-handler rewriting over the
post-perceus AST (half of `kir_inline`/`kir_varslot`); minting top-level
loop fns required free-variable analysis and scope reconstruction
pre-resolution. The intrinsic keeps the loop bodies *in place* (names
resolve in their own scope, nothing is lifted) and keeping the lambdas
as wrappers means Perceus applies its entire existing closure-capture
discipline unchanged: in-lambda reads dup, the frame keeps ownership and
its exit drop, and the closure's create-incref/drop-decref pair (both
runtime-side, `kai_closure` increfs captures) vanishes together with the
closure. RC balance for the inline loop follows from proven rules
instead of new ones.

**The original callee rides as intrinsic arg 0.** The resolver checks
its visibility exactly as it checked the call (a missing `import loop`
still errors), and the typer delegates to `synth_call` on it — type,
row join, and lambda checking are the original call's by construction.
The emitter ignores arg 0.

**Blocked-name set instead of a program-wide bail.** `list.repeat` and
`string.repeat` exist, so "any non-loop decl named like a combinator
disables the pass" would disable `repeat` forever. A module fn blocks a
name only when its signature could actually receive the trailing-lambda
call (second param a function type); root-file fns, consts, effect ops,
protocol ops, and `import x as loop` block unconditionally. The set
seeds the walk's scope, so blocked and locally-shadowed names take the
same skip path.

## Structural surprises

- **Three independent use-scanners had to learn `EIntrinsic`.** Perceus'
  `pcs_collect_uses_kind` (names used only in the loop counted as unused
  → entry drop → UAF), fnreg's `scan_uses_kind` (feeds the emitter's
  `block_unused_lets`, which attached an inline `kai_decref` right after
  the slot's `SLet` — the first emitted-C inspection caught it), and the
  post-interp cell-read relower `lcr_kind` (a `#{i}` inside a loop body
  left the lifted read un-lowered → type error). Every walker with a
  `_ ->` default is a potential silent wrong-default when a node starts
  carrying live sub-expressions; the emitted C, not the test suite, is
  what surfaced two of the three.
- **The lambda collector had to treat loop lambdas as inline code.**
  `collect_expr` stamps lambda bodies with `cur_enc_fn = _kai_lam_<id>`;
  a genuine nested lambda inside a loop body would then be registered
  under a lam symbol while the inline emission looks it up under the
  enclosing fn. Loop-arg lambdas now walk their bodies in the enclosing
  scope and mint no `LamInfo`.
- **`#{i}` interp reads keep the handler.** The desugar's lexical interp
  scan treats a naked cell read inside `#{...}` as an escape, so such a
  var stays on the canonical State handler while the loop still emits
  inline — a correct hybrid (fewer closures, State dispatch inside the
  loop), verified by fixture `loud_sum`-shape runs.
- **kaic1 parser**: `(match ...) and (match ...)` does not parse in the
  stage-1 bundle; split into two `let`s.
- **Purity-folded baselines**: a tail-recursive triangular sum compiles
  to a closed form under the purity attrs + `-O2` (0.0 s at N=1e8), so
  the benchmark accumulation had to be a `acc*31 + i` hash to measure
  anything.

## Measured

C backend, `var`+`while` hash loop at N=1e8 (mac arm64, best of 3):

| binary | wall |
|---|---|
| before (2 nested State handlers + setjmp + closures) | 4.95 s |
| after (slots + inline loop) | 2.28 s |
| tail-recursive reference | 0.05 s |

~2.2× faster; the residual ~46× over tail-rec is slot memory traffic +
per-iteration RC/boxing — gap 2 (slot→register), explicitly out of this
lane. rb-tree fill @1M on the C backend is unchanged (0.437 s → 0.434 s
median, same tree). `sum_to` emits **zero** State clauses (was two
handlers); KIR dumps are byte-identical to baseline (native pipeline
untouched); selfhost stays deterministic (tier0 green) — the emitted
stage2.c changes legitimately since the compiler's own loops now lower
to slots.

## Fixtures

- `examples/sugars/loop/issue_1179_var_slot_loops.kai` (+golden) —
  while/until/repeat/nested/string-accumulation battery; the harness
  greps the emitted C for zero desugar-minted State clauses.
- `examples/sugars/loop/issue_1179_each_closure_state.kai` (+golden) —
  a var captured by an `each` closure keeps its State clauses.
- Target `test-issue-1179-var-slot-loop`, wired into
  `TEST_LIGHT_TARGETS` and `test-fast` next to
  `test-issue-248-loop-sugars`.

Coverage gap left: `forever` is not lowered (returns `Nothing`, no var
shape in the issue); a `repeat` whose count expression performs effects
is evaluated once before the loop, matching the combinator.

## Quality

`loop_lower.kai` scores km B+ (83.4), cogcom avg 2.8 / max 13 — above
the B floor and in line with the shipped walker-class precedent
(`kir_varslot.kai` B 81.0, `kir_inline_beta.kai` B+ 85.9); the Halstead
density of a full-ExprKind structural walk is the limiting dimension.

## Follow-ups

- Gap 2 (#1179): slot→register. On the C backend the inline loop makes
  the remaining cost legible: `array_get`/`array_set` + dup/decref per
  iteration on a 1-element array the C compiler cannot promote.
- The slot specialisation's drop discipline predates this lane: a slot
  read in tail position dups without a balancing exit drop (same shape
  with or without loops). Worth a dedicated look if slot arrays ever
  hold non-trivial payloads.
- `loud_sum`-shape hybrids (interp read keeps the handler) would slot
  too if the interp scan lowered reads instead of vetoing.
