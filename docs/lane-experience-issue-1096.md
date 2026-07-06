# Lane experience — issue #1096: complete the numeric operator surface

## Scope as planned vs as shipped

**Planned** (from the issue): three connected gaps in the non-primitive
numeric operator surface.

1. `Decimal` was missing ALL operator protocols — it carried only
   `Numeric` (inline in `decimal.kai`) and none of `Add`/`Sub`/`Mul`/`Div`,
   so `Decimal + Decimal` failed with "no impl of Add for type Decimal".
2. `Div` (`/`) was missing on `Rational` / `BigInt` / `DecimalBig`: they
   had `Add`/`Sub`/`Mul` but their `div` returned `Option`, which does not
   fit the `Div` protocol's `div(self, rhs) : Self`.
3. Div-by-zero was non-uniform: Int now traps (#1093/#1094), but the big
   types returned `Option`. The issue wanted one rule — div-by-zero always
   traps, `Option` is opt-in.

The issue also asked to make the Decimal operator impls **UoM-polymorphic**
(`[u: Measure]`) so `Decimal<USD> + Decimal<USD>` becomes a working money
value, unblocking a later Money redesign.

**Shipped**: gaps 1–3 in full. The UoM part was **cut from scope** after
measurement (see below). Concretely:

- New `stdlib/decimal_proto.kai` (`impl Add/Sub/Mul/Div for Decimal`),
  delegating to the free `decimal.add`/`sub`/`mul` and a new `decimal.op_div`.
- New `impl Div for Rational/BigInt/DecimalBig` in their `*_proto.kai`,
  each delegating to a new 2-arg `op_div` that traps on a zero divisor.
- New `op_div` (2-arg, total, trap-on-zero) alongside the existing
  `Option`-returning `div` in `rational.kai`, `bigint_convert.kai`,
  `decimal.kai`, `decimal_big.kai`. The `Option` `div` stays as the opt-in
  checked variant (documented, not renamed — it has real callers).
- Shared `protocols.trap_divide_by_zero[a]()` helper backing all four.

## The structural surprise the brief did not anticipate

The issue assumed that making the Decimal operator impls UoM-polymorphic
(`[u: Measure]` signatures) would give `Decimal<USD> + Decimal<USD>` its
money-safe behaviour (`USD + EUR` = compile error, like `Real`). **It does
not** — the unit check lives in the typer, not in the impl:

- `Real` is a **primitive** head. `Real<USD> + Real<EUR>` routes through
  the `st_unify` path (`infer.kai`), whose dim-aware unifier compares the
  units and rejects the mismatch. That is why `USD + EUR` is already a
  compile error for `Real`.
- `Decimal` is a **non-primitive** head (`{raw, scale}` record). Its `+`
  routes through `synth_binop_proto` → `Add.add`, which **erases the
  unit**. So `Decimal<USD> + Decimal<EUR>` compiled with no error,
  regardless of whether the impl carries `[u: Measure]`. The impl receives
  a `Decimal` whose unit is already gone.

Measured, not assumed: a temporary `impl Add for Decimal` (no `[u]`) made
`Decimal<USD> + Decimal<USD>` compile AND `Decimal<USD> + Decimal<EUR>`
compile silently — the exact hole. Making the impl `[u: Measure]` changed
nothing.

Two further measurements narrowed the real shape of "UoM on non-primitives":

- `BigInt<USD>` / `Rational<USD>` / `DecimalBig<USD>` do **not even parse**
  ("expected `=` in let binding" at the `<`). Units attach only to the
  three numeric heads `Real` / `Int` / `Decimal` (`kai info units`). So the
  big types have no unit hole — you cannot construct a unit-carrying value
  to add.
- `Decimal` is the sole type where the syntax admits `<u>` but the typer's
  operator path drops it.

Closing the Decimal hole would mean teaching the typer to unify operand
units before proto-dispatch for `+`/`-`/`/` on dimensioned non-primitives
— a typer change, not a stdlib one. **Decision (user):** leave UoM out of
this lane entirely; ship the core operator + trap surface only. Money-via-
UoM stays blocked on that separate typer work.

## Design decisions and alternatives considered

- **`op_div` vs renaming `div` → `checked_div`.** The issue offered
  "rename to `checked_div` OR document". The `Option`-returning `div` has
  live callers (internal stdlib: `rational.exact_div`, `decimal_big.trunc_div`;
  fixtures; `examples/`). Renaming breaks them. Chosen: keep `div`
  (documented as the checked/opt-in variant) and add a new total
  `op_div` that the operator dispatches to. No `checked_div` alias — that
  would be a third name with no new intent (the repo rule against
  overlapping forms).
- **How to trap.** The issue is explicit: same class as an out-of-range
  index, NOT a manual `panic`. Measured: `panic("…")` emits `panic: …`;
  raw `Int` `/0` emits `trap: divide by zero` via `kai_idiv_chk`
  (#1094's mechanism). No trap builtin is exposed to `.kai`. So the shared
  helper routes through raw `Int` division by a runtime zero the folder
  cannot fold — the only honest way to raise that exact trap from stdlib.
- **Div result scale for Decimal / DecimalBig.** Their `div` takes an
  explicit `target_scale`; the 2-arg operator needs a default. Chosen
  `max(sa, sb)`, matching what `add`/`sub` normalise to. Callers wanting
  other precision keep using the 3-arg `div`.

## Fixtures added and coverage

Positive (`test-numeric-decimal-rational`, C + native):
- `examples/numeric/decimal_operators.kai` — new; `+ - * /` on `Decimal`.
- `examples/numeric/big_numeric_operators.kai` — extended with `/` for
  BigInt (truncating), Rational (exact), DecimalBig.

Trap (`test-numeric-div-by-zero-traps`, C + native — new targets modelled
on `test-issue-1093-divzero-trap`):
- `{bigint,rational,decimal_big,decimal}_div_by_zero_traps.kai` — each
  prints one line, divides by a runtime zero, and must exit non-zero with
  `trap: divide by zero` on stderr and only the pre-trap line on stdout.

Coverage gap left: no negative UoM fixture (`Decimal<USD> + Decimal<EUR>`),
because UoM is out of scope. When the typer fix lands, that fixture is the
acceptance test for it.

## Verification

- `make tier0` green — selfhost byte-identical (`kaic2b.c == kaic2c.c`)
  after the stdlib changes.
- `test-numeric-decimal-rational` (C): 5/5 positive fixtures OK.
- `test-numeric-div-by-zero-traps` (C): 4/4 traps OK.
- `km score stdlib/decimal_proto.kai`: A++ (100.0).

## Follow-ups for next lanes

- **Typer unit-check on non-primitive `+`/`-`/`/`** — the real prerequisite
  for Money-via-`Decimal<USD>`. Teach `synth_binop_proto` (or its gate) to
  unify operand units before dispatch and propagate the unit to the result,
  so `Decimal<USD> + Decimal<EUR>` becomes a compile error like `Real`.
- **Money redesign** (route A `Decimal<USD>` vs route B phantom `Money[c]`)
  — still deferred, now unblocked on the typer work above rather than on
  Decimal's operators.
