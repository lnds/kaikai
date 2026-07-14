# Lane experience — issue #1214: promote a non-literal pure-scalar var init

## Scope as planned vs shipped

**Planned.** The imperative `while` + `var` spelling of a hot loop ran
~16× slower than the equivalent tail recursion (native; ~14–20× on the C
backend). The issue proposed two fronts: (1) inline the `while` intrinsic
when pred/body are literal closures, and (2) lower non-escaping `var` cells
to raw registers.

**Shipped.** A one-predicate relaxation. Front (1) was already done by
#1179 gap 2 (`lower_loop_intrinsics` un-gated for all backends): the KIR of
`collatz_while` already shows a block loop with a backedge, no closure call
per iteration. Front (2) was ALSO done by #1179 gap 2 — but ONLY for a
**literal** scalar init (`var i := 0`). The `collatz` repro starts from a
parameter (`var m := n`), a non-literal init, which #1179's own follow-up
named as the deferred hot case:

> *"Non-literal scalar inits (`var i := n`) keep the array slot; the border
> unbox at the decl is straightforward if a hot case appears."*

This IS that hot case. The fix relaxes `cell_promote`'s init gate from
`cp_init_is_scalar_lit` (literals only) to `cp_init_is_pure_scalar`: a
scalar-typed init that is a literal, a plain local read, or builtin
arithmetic over those. `var m := n` and `var m := n*2+1` now promote to a
raw `i64` register; a call init or a String init keeps the array slot.

Confirmed by KIR diff: `var m := n` went from
`m__slot: box = array_make(int.box(1), n)` + `array_get_borrow`/`array_set`
+ boxed `prim %` per iteration to `m__slot: i64 = int.unbox(n)` + raw
`prim i%(m__slot, 2)` — byte-for-byte the shape the literal-init version
already got, which is the shape tail recursion gets.

Measured (longest Collatz chain under N=5,000,000, native `--release`,
`/usr/bin/time -l`, min of 3 warm runs, quiet machine; all print
`n=3732423 steps=596`):

| variant | wall | peak RSS |
|---|---:|---:|
| `while`+`var` before | 10.17 s | 323 MB |
| `while`+`var` after | **0.68 s** | **1.8 MB** |
| tail recursion (reference) | 0.58 s | 1.8 MB |

15× faster and 180× less memory; the residual over tail recursion is
1.17× (0.68 vs 0.58 s), within noise. The per-iteration array traffic is
gone — RSS is identical to the recursive form.

## Design decisions

**Gate on purity + scalar type, not on the syntactic form `EVar`.** The
architect's verdict: what makes `array_make(1, f())` vs `__cell_init(f())`
safe is that the init evaluates exactly once, before the loop, at the same
sequence point — which holds for BOTH lowerings, so an effectful init is
not duplicated. The real discriminant is whether the init has a trivially
provable box→raw border. A literal, a local read, and builtin arithmetic
over those do; a call does not (its scalar-looking result may lower boxed
for a reason the local gate cannot see — a proto-dispatch, say). So the
predicate accepts `EInt`/`EReal`/`EBool`/`EVar` and `EBinop`/`EUnop` with a
builtin arithmetic op (`+ - * / %`) over operands that recursively pass,
each level gated by `cp_ty_is_cell_scalar` on the result type — a user
protocol op returning a boxed type fails the type check and is declined.
This buys `n`, `n+1`, `n*2`, `-n` without opening the gate to calls.

**RC of the non-literal init is automatic.** `var m := n` where `n` is a
boxed parameter: the promoted `__cell_init(n)` replaces `array_make(1, n)`
IN PLACE, so Perceus sees the same `EVar(n)` in the same flow position it
already balanced. Last-use → the border unbox consumes; live-after → Perceus
dups and the border borrows. No RC-special logic — the fix relies on
cell_promote running pre-Perceus (post-monomorph / pre-unbox), which it
already does, so the init's `n` is a visible use for the RC pass.

**The residual-use revert is orthogonal.** `cp_block_slot`'s
rewrite-then-verify scans the statements AFTER the decl for a residual
mention of the CELL name (`m`); it never scans the init. The init's
`EVar(n)` (n ≠ m) cannot trip the revert. A pathological `var m := m`
(init reads an outer `m`) would revert conservatively — correct, just
unpromoted.

## Structural surprises

- **The while-inline was already done; only the init gate remained.** The
  issue reads as two features, but a KIR dump showed the loop already
  inlined (block loop, no closure-call) and `steps := 0` already promoted.
  The whole 16× residual hung on the single boxed cell `m` whose only sin
  was a non-literal init. A dump-diff of `m := 27` (promoted) vs `m := n`
  (not) in the pred/body confirmed 100% of the boxed traffic was the cell
  read, no independent second cause — so the one-predicate relaxation
  closes it completely, no second front needed.

## Fixtures

- `examples/sugars/loop/issue_1214_nonliteral_init.kai` (+ golden):
  `var m := n` (local read), `var m := n*2+1` (arithmetic) — both must
  promote; `var m := twice(n)` (call) and `var acc := s` (String) — both
  must keep the array slot. The harness diffs native==C output, then greps
  the KIR for the promoted cells' `m__slot: i64` and the emitted C for the
  non-promoted cells' retained `array_make`. Target
  `test-issue-1214-nonliteral-init`, wired into `.PHONY`,
  `TEST_LIGHT_TARGETS`, and `test-fast` next to `test-issue-1179-var-slot-loop`.

## Quality

`cell_promote.kai` scores km A (90.9), cogcom avg 1.8 / max 4 — the
relaxation added two small recursive predicates and did not move the grade.

## Follow-ups

- Non-scalar var inits (String cells) still keep the array slot; a boxed
  cell has no raw register form, so this is a floor, not a gap.
- A call init (`var m := f(n)`) stays on the slot deliberately. If a hot
  case appears where the call's result is provably a raw scalar, the gate
  could admit a whitelisted set of core scalar-returning calls — but that
  is a per-callee proof the local gate cannot do today.
