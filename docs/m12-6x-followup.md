# m12.6.x — Refinements + Contracts follow-up

Tracks every item the m12.6 lane (sub-lanes a–e + h, 2026-04-27)
deferred to a future milestone. The common thread is **TyRefineT
on the semantic side**: m12.6 v1 shipped syntax + runtime semantics
without ever surfacing the refinement in the typer's `Ty`.

**Update 2026-04-27 (m12.6.x partial landing):** items #1, #4, #5
(enforcement only), and #6 are now landed. The remaining items
(#2 interval propagation, #3 match-arm narrowing, #5 attribute
parser, #7 regex literals, #8 diagnostics quality) are still the
pending list — see status notes inline below.

## What landed in m12.6 v1 (for context)

- **m12.6.a** — `TyKind | TyRefine(TypeExpr, Expr)` AST + parser
  for `BaseT where Pred`, leading-comparator desugar
  (`where >= 0` → `where self >= 0`), 11 walker arms across the
  syntactic TyKind passes.
- **m12.6.b** — `TkRequires` / `TkEnsures` keywords +
  `parse_contracts_loop`. Both clauses desugar to asserts wrapping
  the body.
- **m12.6.c** — `let result = body` shape, so `ensures result > x`
  binds `result` as a real local in the post-condition scope.
- **m12.6.d** — Constant folder over predicates: trivially-true
  preds drop at parse time. Covers Bool / Int literals, the
  boolean operators, comparisons, and Int arithmetic between
  literals.
- **m12.6.e** — `ensure(value) where pred` primary expression →
  `Option[T]`. Recognised only when followed by `where`.
- **m12.6.h** — Demos `factorials` and `collatz` flipped to OK
  with contracts; `docs/refinements-and-contracts.md` Implementation
  status section names what's done vs deferred in concrete terms.

## Deferred items

### 1. `TyRefineT` semantic type — the load-bearing block *(LANDED 2026-04-27)*

`resolve_ty` currently drops `TyRefine(base, pred)` to whatever
`base` resolves to. That makes the predicate invisible to unify,
substitution, monomorphisation, and codegen. Every other deferred
item below is downstream of this one.

Add `Ty | TyRefineT(Ty, Expr)` to the semantic type catalog.
Then audit every `Ty` walker (`apply_ty`, `unify`, `occurs`,
`ty_to_string`, `mangle_ty`, `ty_eq_shape`, etc.) and decide for
each whether `TyRefineT(b, p)` should:

- **Recurse on `b`** (transparent — what most current code does
  for `TyDimT`).
- **Carry the predicate** (preserve, e.g. for diagnostics).
- **Reason about the predicate** (subtype rules — only `unify`
  and the new subtype check need this).

The single subtle invariant is **subtype direction**:
`TyRefineT(b, p) ⊑ b` always; `b ⊑ TyRefineT(b, p)` requires
narrowing. The unifier must allow the upcast (e.g.
`Int where >= 0` flowing into a parameter typed `Int`) and reject
the downcast without a narrowing step. Acceptance criterion: a
function returning `Int where >= 0` flowing into `Int` parameters
unifies cleanly; the reverse demands `ensure(...)` or a match-arm
narrow.

Estimated cost: 1–2 days. Affects ~20 functions (every `Ty` arm)
but each arm is small. The biggest risk is the unifier — get the
direction wrong and either every refinement leaks into every base
type, or every assignment to a base needs an explicit cast.

### 2. Static interval propagation through refined args *(STILL PENDING — design-heavy)*

**Why deferred from m12.6.x v1**: omitting an assert at *one* call
site requires either monomorphising the callee per refinement
combination (binary bloat) or emitting per-call inline checks at
the caller (which defeats the audit pitch — the predicate now
lives in two places). The v1 const folder of m12.6.d already
handles closed predicates; the missing piece — substituting
caller-side refinements into callee predicates — needs an
end-to-end design that picks one of the two paths above.

m12.6.d's `try_eval_pred` only evaluates **closed** predicates.
With `TyRefineT` in hand, the typer can substitute the call-site
argument's refinement into the callee's predicate at every call
site:

```kai
fn divide(a: Int, b: Int) : Int requires b != 0 = a / b

fn use_it(b: Int where != 0) : Int = divide(10, b)
                                            # ^^^ statically proves
                                            # b != 0 at the call site
                                            # — assert is omitted.
```

Acceptance criterion: when the caller's refinement `entails` the
callee's predicate, the runtime check is dropped. "Entails" stays
in the decidable subset (interval propagation, regex subsumption);
no SMT.

Cost: 0.5–1 day on top of #1. Most of the machinery is in
`try_eval_pred` already; the new piece is the substitution
(`predicate[arg ↦ caller's refinement]`) and the entailment check.

### 3. Match-arm narrowing — `p : RefinedT` *(STILL PENDING)*

**Why deferred from m12.6.x v1**: requires a new PatKind variant
(`PTyped(String, TypeExpr)`), arms in every Pattern walker, and a
match-runtime that supports per-arm fall-through via a runtime
predicate check. The desugar shape (an `if pred { bind } else {
fall through to next arm }`) is fine in isolation but the
fall-through interaction with existing match exhaustiveness needs
care.

```kai
match n {
  p : Port -> use_port(p)
  _        -> Fail.fail("port out of range: #{n}")
}
```

The `p : Port` arm desugars to a runtime check: if the predicate
of `Port` holds, bind `p : Port` (with the refined type in scope)
and run the body; otherwise fall through. Without `TyRefineT`,
"refined type in scope" is meaningless — `p` would just have
type `Int`.

Cost: 0.5 day on top of #1. Pure pattern-match desugar; the
runtime check is already what `assert` produces.

### 4. UoM composition — `Decimal<USD> where >= 0` *(LANDED 2026-04-27)*

Once `TyRefineT` exists, the canonicalisation rule
`TyDimT(TyRefineT(b, p), u) ↔ TyRefineT(TyDimT(b, u), p)` lets a
single field carry both a unit and a value-range constraint:

```kai
type Account = {
  balance: Decimal<USD> where >= 0,
  ...
}
```

Cost: ~half a day. The canonicalisation is one rewrite rule;
display/diagnostics need to render the combined form readably.

### 5. `[<refinement-pure>]` stdlib annotations + enforcement *(ENFORCEMENT LANDED 2026-04-27, ATTRIBUTE PARSER STILL PENDING)*

The enforcement half (rejecting calls in predicates that aren't in
the closed pure prelude set) is in tree as of m12.6.x v1. The
attribute parser — letting user fns opt into the pure surface —
remains pending: the closed list `[is_some, is_none, is_finite,
is_nan, string_length, list_length, matches]` is hard-coded today.

The spec lists a closed set of prelude predicates that may appear
inside refinement / contract predicates: `is_finite`, `is_nan`,
`length`, `is_some`, `is_none`, `matches`. Today the predicate
parser accepts any Expr (including calls to user-defined fns and
side-effecting expressions). m12.6.x adds:

- Parser support for `[<refinement-pure>]` after a fn signature
  (an attribute-style annotation).
- A validation pass over every `where` / `requires` / `ensures`
  predicate that rejects calls outside the refinement-pure set.

Cost: 1 day. The annotation parser is straightforward; the
validation pass walks predicate Exprs with the existing
`map_expr_kind` visitor.

### 6. Compile-time errors for trivially-false predicates *(LANDED 2026-04-27)*

`try_eval_pred` already detects `assert false` from the const-fold
pass; today it leaves the assert in place and lets the runtime
panic do the diagnostic. Promote: when a contract predicate
folds to `false` at parse time, emit a fixed-position error
("precondition / postcondition trivially violated at line N")
instead of the runtime trap.

Cost: ~half a day. Just a different sink in `preds_to_asserts`.

### 7. Regex predicates — `String where matches /.../ `

Requires:
- A regex literal in the lexer (`/.../`, anchored only, no
  backreferences, no lookaheads — Pony / Ada Static_Predicate
  subset).
- A `matches: (String, Regex) -> Bool` in the predicate-pure set.
- A subsumption check in the static-proof pass (regex containment
  for the literals supported).

Cost: 1–2 days. Most of the cost is the regex evaluator; the
spec is explicit that we **reuse the runtime regex library** at
compile time rather than write a new one. Until that library
exists, this item is also blocked on a separate "regex stdlib"
lane.

### 8. Diagnostics quality pass

m12.6 v1 panics with a generic `assertion failed`. The spec
calls for predicate-aware messages:

```
error: precondition violated in `divide`
  --> src/foo.kai:42:5
   |
42 |   divide(numerator, 0.0)
   |   ^^^^^^^^^^^^^^^^^^^^^^
   = note: required: `b != 0.0`
   = note: argument `b` was: 0.0
   = help: narrow `b` to `Real where != 0.0` at the call site,
           or wrap in `ensure(b) where != 0.0` and handle `None`.
```

Two pieces:
- The `SAssert` shape needs a structured "violation context"
  (predicate text, the original ident the predicate references,
  call-site span) instead of the current `Option[Expr]` message.
- The runtime panic path needs to format the structured context
  rather than the generic "assertion failed".

Cost: 1 day, lockstep with the m11 diagnostics-quality milestone.
Lands either when m12.6.x picks it up or when m11 runs the
diagnostics quality pass over the whole language.

## Suggested ordering inside m12.6.x

Items #1, #2, #3, #4 are a single coherent block — semantic
refinements + reasoning + composition. Land them adjacent; the
shared `TyRefineT` plumbing amortises the design overhead. Estimate
3 days total at observed velocity.

Items #5, #6 are independent of #1–#4 and could land as
prerequisites if the refinement-pure validation shows up in early
demos. Estimate ~half a day each.

Items #7, #8 are largest and most isolated. #7 blocks on a regex
stdlib lane; #8 lands inside m11 if not before.

## Relation to the Tier 3 LLM-affordances bet

Every item in this list improves the JSON the compiler hands an
LLM. #2 (interval propagation) means typed-hole reports include
the *narrowed* type at every position; #5 (refinement-pure) means
LLMs can author predicates without being told the closed prelude
set every time; #8 (diagnostics) means a violated predicate yields
the exact `requires` / `ensures` text the model needs to fix the
call site. m12.6.x is on the critical path for the audit-trail
+ LLM-coding bets jointly.
