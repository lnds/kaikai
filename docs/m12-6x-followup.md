# m12.6.x — Refinements + Contracts follow-up

Tracks every item the m12.6 lane (sub-lanes a–e + h, 2026-04-27)
deferred to a future milestone. The common thread is **TyRefineT
on the semantic side**: m12.6 v1 shipped syntax + runtime semantics
without ever surfacing the refinement in the typer's `Ty`.

**Update 2026-04-27/28 (m12.6.x closed end-to-end):** items #1, #2
(param-refinement entailment v1), #3 (match-arm narrowing), #4,
#5 (enforcement + attribute parser), #6, and #8 (positional msg)
all landed.

**Update 2026-04-28 (#2 deepened, #7 unblocked):**
- #2 progressed beyond v1: the `Interval` lattice (`ITop | IBot |
  Ival(Int, Int)`) landed in `0a6e0f2`, and the interval pass +
  `--dump-intervals` flag + `examples/intervals/basic.kai`
  fixture landed in `b6bd5f6`. Alpha-equivalent matching,
  operator rewriting, and call-site substitution still pending —
  see "Still pending under #2" below.
- #7 is **partially unblocked**: the regex stdlib (RE2-style NFA,
  Phases 1–7) shipped in `a1cdda9`, and `3909c71` pinned the
  status. The refinement-side syntax (`String where matches
  /.../`, lexer/parser integration, subsumption check) remains as
  the m12.6.x sub-lane.

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

### 2. Static interval propagation through refined args *(v1 LANDED 2026-04-27; sub-steps 1–4 LANDED 2026-04-28; FULL VERSION STILL PENDING)*

**v1** (2026-04-27, `413e1eb`): preds_to_asserts now drops a
contract assert that is **structurally identical** to one of the
fn's parameter refinements (`fn divide(a: Int, b: Int where b !=
0) requires b != 0` → zero asserts emitted). Match: literals by
value, EVar by name, EBinop/EUnop by op + recursive
sub-equality. ~70 lines including the Expr-walker.

**Sub-step 1** (2026-04-28, `0a6e0f2`): `Interval = ITop | IBot |
Ival(Int, Int)` lattice as foundation for interval reasoning.
~188 lines in `stage2/compiler.kai`.

**Sub-steps 2+3+4** (2026-04-28, `b6bd5f6`): interval pass +
`--dump-intervals` flag + `examples/intervals/basic.kai` fixture.
Real interval reasoning (`x >= 1` entailed by `x > 0` on Int) is
now mechanically supported by the lattice.

**Still pending under #2**:
- Alpha-equivalent matching: `requires b > 0` against
  `Int where x > 0` (different variable names) doesn't fire.
- Operator rewriting: `x > 0` not entailed by `0 < x`.
- Substitution at the call site: omitting an assert when the
  caller passes a literal (`divide(10, 5)` would entail
  `requires b != 0` since `5 != 0` folds to true). The v1
  pass operates inside the callee body; call-site substitution
  needs either monomorphisation per refinement combination or
  per-call inline checks.

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

### 3. Match-arm narrowing — `p : RefinedT` *(LANDED 2026-04-28)*

**v1**: parse_arm detects an optional `: Type` after a binding
pattern; when the inner TypeExpr is a TyRefine, the parser
synthesises a guard `{ let self = name; pred }` that AND-combines
with any explicit `if user_pred`. Only PBind patterns participate;
other shapes followed by `:` raise a clear error.

**Limit**: only inline refinements (`Int where ...`) are accepted
at the `:` site. Aliases (`type Port = Int where ...` followed by
`p : Port`) need alias resolution at parse time — deferred.

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

### 5. `[<refinement-pure>]` stdlib annotations + enforcement *(ENFORCEMENT + ATTRIBUTE PARSER LANDED 2026-04-27/28)*

Both halves landed:
- **Enforcement** (m12.6.x v1): `find_impure_call` walks every
  contract predicate at parse time and rejects calls outside the
  closed pure-prelude set.
- **Attribute parser** (m12.6.x v2): `[<refinement_pure>]` opt-in
  on user fn signatures. `DAttribPure` wrapper + post-parse
  `extract_pure_names_decls` + `validate_contract_predicates_decls`
  re-runs the impure-call check with the combined `closed ++ user`
  list, so user fns marked pure participate in predicates.

Limit: attribute placement requires a newline before `[<` because
`Bool [` adjacent on a single line lexes as `Bool[T]` generic-type
syntax. Inline placement needs lexer disambiguation.

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

### 7. Regex predicates — `String where matches /.../ ` *(PARTIALLY UNBLOCKED 2026-04-28)*

**Stdlib half landed** (`a1cdda9`, regex Phases 1–7): RE2-style
NFA, public API `regex_compile / match / find_all / replace /
split`, ASCII subset (no backrefs / no lookaround). `3909c71`
pins the status in `docs/stdlib-layout.md` and
`docs/stage2-design.md`.

**Refinement-side half still pending** as the m12.6.x sub-lane:
- A regex literal in the lexer (`/.../`, anchored only, no
  backreferences, no lookaheads — Pony / Ada Static_Predicate
  subset).
- A `matches: (String, Regex) -> Bool` in the predicate-pure set.
- A subsumption check in the static-proof pass (regex containment
  for the literals supported).

Cost: ~1 day now that the runtime evaluator exists. The
compile-time path can call into the same `regex_compile` /
`regex_match` runtime helpers directly; no separate evaluator
needed.

### 8. Diagnostics quality *(v1 LANDED 2026-04-27, FULL VERSION STILL PENDING)*

**v1**: contract asserts now panic with `"requires violated at
line N"` / `"ensures violated at line N"` instead of the generic
`"assertion failed"`. preds_to_asserts builds the EStr msg and
emit_assert_stmt embeds the literal span when opt_msg is a
literal EStr.

**Still pending**: pass

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

Items #7, #8 are largest and most isolated. #7's stdlib half
landed 2026-04-28 (`a1cdda9` + `3909c71`); only the
refinement-side syntax + subsumption remain. #8 lands inside m11
if not before.

## Relation to the Tier 3 LLM-affordances bet

Every item in this list improves the JSON the compiler hands an
LLM. #2 (interval propagation) means typed-hole reports include
the *narrowed* type at every position; #5 (refinement-pure) means
LLMs can author predicates without being told the closed prelude
set every time; #8 (diagnostics) means a violated predicate yields
the exact `requires` / `ensures` text the model needs to fix the
call site. m12.6.x is on the critical path for the audit-trail
+ LLM-coding bets jointly.
