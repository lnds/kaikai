# Refinement Types + Contracts for kaikai

Research notes, snapshot 2026-04-25. Discussion document.
Question: do refinement types (Pony, Ada subtypes) combined with
Design-by-Contract (Eiffel, D, Ada 2012) fit kaikai, in a "lite"
form that stays decidable and SMT-free?

**Short answer: yes. Together they form a single coherent
mechanism — types describe what values are valid, contracts
describe what operations guarantee — and the two share most of
the implementation machinery. Schedule as one milestone, not
two.**

## Two complementary mechanisms

### Refinement types — restrict the valid values of a type

```kai
type CurrencyCode = String where matches ~r/^[A-Z]{3}$/
type NonNegative  = Int    where >= 0
type Percent      = Real   where 0.0 <= self <= 1.0
type Port         = Int    where 1 <= self <= 65535
```

Regex predicates use the `~r/.../` sigil (Elixir-style — `~r`
followed by a delimiter pair). The sigil is an expression in its
own right (typed `Regex`); `where matches ~r/.../` desugars to
`matches(self, ~r/.../)`, mirroring the `where >= 0` →
`self >= 0` receiver-elision sugar already familiar from numeric
predicates.

A refinement type is `BaseType where Predicate(self)`. The
predicate references `self` — the value being constrained.
Wherever a value of the refined type travels (assignment,
parameter, return), the predicate is part of its identity.

Implementations in production: **Ada subtypes** (`subtype Positive
is Integer range 1 .. Integer'Last;`) since 1983; **Pony**
capabilities; **Scala 3 + Iron** (community); **TypeScript
template literal types** (limited to string shapes).

### Contracts — restrict what operations expect and guarantee

```kai
fn divide(a: Real, b: Real) : Real
  requires b != 0.0
  ensures  is_finite(result)
= a / b

fn deposit(acc: Account, amount: Decimal<USD>) : Account
  requires amount > 0.0
  ensures  result.balance == acc.balance + amount
  ensures  result.transactions == acc.transactions + 1
=
  { acc with
    balance = acc.balance + amount,
    transactions = acc.transactions + 1
  }
```

A contract is `requires <predicate>` (preconditions, the caller
must satisfy) and `ensures <predicate>` (postconditions, the
function guarantees). Inside `ensures`, `result` names the
return value, and the original arguments stay in scope by name —
since records are immutable in kaikai, no `old` keyword is
needed.

Implementations in production: **Eiffel** (the canonical
1986 source), **Ada 2012** (`Pre =>`, `Post =>`), **D**
(`in { ... }` / `out (result) { ... }`), **Cobra**, **Kotlin**
idiom (`require` / `check` as stdlib). The static-verification
strain (Spec#, Dafny, SPARK) uses SMT and is **not** what
kaikai adopts.

### Why together

Refinement types cover the **invariants on data**: any value of
type `Account` has `balance >= 0` because the type says so.

Contracts cover the **invariants on operations**: `deposit`
relates the input account, the amount, and the output account,
which is a constraint that no single type can express because
it spans multiple values.

The implementation machinery — predicate parser, predicate
type-checker, runtime check insertion, diagnostics — is **the
same** for both. Splitting them into two milestones doubles the
documentation work and the design overhead, without any savings.

## Why it fits kaikai

### Tier 1 — Safe at compile time

Same principle as effects in types and units of measure: what a
value can be, and what an operation guarantees, lives in the
signature and is checked at compile time **where possible**.
Where the compiler cannot prove it (because the predicate
mentions a runtime value with no refinement narrowing it), the
check becomes a `panic`-class assert at the boundary, audited
exactly the same way as `panic` and unfilled `?`.

### Tier 1 — Runtime efficient

- Predicates over literal values: zero runtime cost (typer
  evaluates them at compile time).
- Predicates statically derivable from refinements at the call
  site: the typer proves them and **omits** the runtime check.
- Predicates that need runtime evaluation: a single `if !cond {
  panic }`. The optimiser eliminates redundant ones in straight
  code; the rest are predictable, branch-predictor-friendly.
- `--release` flag omits all checks that were proved statically
  during compilation; runtime-only checks stay on by default
  (same model as Ada SPARK).

### Tier 1 — Fast compilation

Predicates are **decidable**. The constraints that keep them so:

1. **Pure**: no effects, no user-defined function calls except
   prelude predicates explicitly marked `[<refinement_pure>]`
   (e.g. `is_finite`, `is_nan`, regex constants).
2. **Linear in the AST**: interval propagation over comparisons,
   constant-folding over `&&` / `||` / `!`.
3. **No SMT**: when the typer cannot prove a predicate, it does
   not try harder; it inserts a runtime check. This is the
   single biggest decision separating the lite design from
   Liquid Haskell / Dafny.

Compile-time impact: proportional to the subset of functions
that carry contracts. Pure code without refinements/contracts
pays nothing.

### Tier 2 — Approachable core

Nothing conceptually new. "Int positive", "string matching a
regex", "function that requires `b != 0`" — these are concepts
every senior developer asks for, regardless of language
background. The forms are familiar from Ada (40 years), Eiffel
(40 years), D, Kotlin idiom.

Layered: code without refinements/contracts compiles exactly
as today. The feature is opt-in at the type or function level.

### Tier 3 — LLM authorability

A typed hole `?: NonNegative` tells an LLM not just the type
(`Int`) but the **valid range** (`>= 0`). A function signature
with `requires amount > 0` tells the LLM the precondition
**explicitly**, not implicitly via comments. Both are exactly
the kind of structured signal the Tier 3 bet exploits — they
shift weight from "the model knowing kaikai" to "the compiler
telling the model what is valid".

Combined with `kai effects --json` and `kai type --json`, this
gives the LLM a much richer view of what code is valid at a
position than any other mainstream language offers today.

## Why it fits the adoption strategy

### For fintech (`adoption-enterprise-fintech.md`)

The adoption document calls bet B **"effects as compile-time
audit trail"** — selling effects in signatures as the structural
evidence a CISO/auditor needs. Refinements + contracts are the
**third leg** of that pitch:

```kai
fn transfer(from: Account, to: Account, amount: Decimal<USD>) : (Account, Account)
  / Audit + DB
  requires from.balance >= amount
  requires amount > 0
  ensures  result.0.balance + result.1.balance == from.balance + to.balance
  ensures  result.0.balance >= 0
```

That signature is a **mini-spec**:
- The effects (`Audit`, `DB`) say what the function touches.
- The `requires` clauses say what the caller must guarantee.
- The `ensures` clauses say what the function guarantees back.
- The refinement on `Account.balance: Decimal<USD> where >= 0`
  says no account ever has a negative balance, by construction.

The auditor reads 50 signatures with their contracts instead of
50 function bodies. That is the literal pitch the adoption doc
proposes.

### For web/startup (`adoption-web-startup.md`)

The branded-types case (`String<UserId>` ≠ `String<OrderId>`)
that UoM enables extends naturally: refinement types add value
constraints to those brands.

```kai
type Email     = String where matches ~r/^[^@]+@[^@]+\.[^@]+$/
type UserId    = String where matches ~r/^u_[a-zA-Z0-9]{16}$/
type Sanitized = String where ... # post-sanitiser invariant

fn send_welcome(email: Email, name: String<Sanitized>) : Unit / Net = ...
```

The compiler rejects `send_welcome("not an email", raw_input)`
at the call site, statically when literals, at parse boundary
when from network input.

## Proposed design

kaikai-specific decisions. Each one pinned for review.

### 1. Refinement type syntax

**Decision**: `where` clause after the base type, with `self`
referencing the value.

```kai
type Port         = Int    where 1 <= self <= 65535
type CurrencyCode = String where matches ~r/^[A-Z]{3}$/
type NonNegative  = Int    where self >= 0    # self explicit
type NonNegative2 = Int    where >= 0         # self elided (unary comparison)
```

When the predicate is a unary comparison against a literal, `self`
on the LHS may be elided. Otherwise `self` is mandatory.

Reasons:
- `where` reads as the natural English: "Int where (it is) >= 0".
- F# uses `where` for type-parameter constraints; that meaning
  is unambiguous in kaikai because kaikai has no Haskell-style
  typeclass constraints to compete for the keyword.
- Ada uses `range` (numeric only) and `Dynamic_Predicate`
  (general). One unified `where` is cleaner for kaikai.

### 2. Contract syntax

**Decision**: `requires` and `ensures` clauses between the
signature and the `=` (or `{`) of the body.

```kai
fn divide(a: Real, b: Real) : Real
  requires b != 0.0
  ensures  is_finite(result)
= a / b

fn factorial(n: Int) : Int
  requires n >= 0
  ensures  result >= 1
= if n == 0 { 1 } else { n * factorial(n - 1) }
```

Multiple `requires` and `ensures` clauses are allowed — they
conjoin. `result` is a reserved name inside `ensures` only.

Reasons:
- Ada 2012's `Pre =>` / `Post =>` aspect notation reads less
  naturally than Eiffel's keyword form.
- D's `in { ... } out (result) { ... }` is verbose for the
  common one-liner case.
- Eiffel's keyword form generalises to multi-clause cleanly.

### 3. Predicate language

**Decision**: a small, decidable subset.

Allowed:
- Comparisons: `<`, `<=`, `==`, `!=`, `>`, `>=`.
- Boolean operators: `&&`, `||`, `!`.
- Constants and `self` / arguments / `result`.
- Field access on records (`acc.balance`).
- Prelude predicates marked `[<refinement_pure>]`: `is_finite`,
  `is_nan`, `length` (on lists/strings), `is_some`, `is_none`,
  string `matches ~r/regex/` (regex literals lex as the
  Elixir-style sigil `~r/.../`; full-match semantics).
- Arithmetic operations between constants and refined values
  (so the typer can interval-propagate).

Disallowed:
- Calls to user-defined functions (would need effect-purity
  proof; punt to a future extension).
- Quantifiers (`forall`, `exists`).
- References to mutable state outside `args` and `result`.
- `old` keyword (kaikai has no cross-function mutable state
  worth tracking; arguments stay in scope by name).

Reasons:
- This subset matches Ada's `Static_Predicate` shape and the
  Pony refinement subset. Both have decades of production
  evidence that the subset is enough for ~80% of real cases.
- SMT-completion (Liquid Haskell, Dafny) is the obvious
  extension if a class of useful predicates is found that the
  subset cannot express. **Out of scope for v1**.

### 4. Verification strategy

**Decision**: hybrid — static where trivial, runtime otherwise.

For each predicate at a check point (assignment to refined type,
function call, return):

1. **If predicate is a constant expression**: typer evaluates;
   accepts or rejects at compile time.
2. **If predicate reduces to refinement-types-lite over the
   call-site arguments** (interval propagation, regex
   subsumption, literal substitution): typer proves; **omits**
   the runtime check.
3. **Otherwise**: typer inserts a runtime check (`if !cond {
   panic("predicate violated: ...") }`). The location is
   tracked for `--holes-json`-class diagnostics extension.

Reasons:
- This is the model Ada SPARK uses. 40 years of evidence that
  it is the right balance for non-research languages.
- It composes with the "audit trail" pitch: the auditor can
  query `kai contracts foo.kai:42 --json` and see which
  predicates are statically proved versus runtime-checked at
  every call site.

### 5. Conversions to refined types

**Decision**: explicit narrowing via `ensure` (one-shot, returns
`Option`) or pattern match (multi-arm).

```kai
# one-shot conversion
fn from_int(n: Int) : Option<Port> = ensure(n) where 1 <= self <= 65535
# desugars to: if 1 <= n && n <= 65535 { Some(n) } else { None }

# pattern-style narrowing
match n {
  p : Port -> use_port(p)
  _        -> Fail.fail("port out of range: #{n}")
}
```

`ensure(value) where Predicate` returns `Option<RefinedT>`
(`Some` if predicate holds, `None` otherwise). The match-arm
form `p : Port` accepts the value as `Port` if the predicate
holds at runtime.

There is **no** implicit narrowing from base type to refined
type; that would silently insert runtime checks at every
boundary and defeat the auditability.

### 6. Composition with Units of Measure (m12.5)

**Decision**: orthogonal, naturally composable.

```kai
type PositiveMoney[u: Measure] = { amount: Decimal<u> where >= 0 }

fn deposit(acc: Account, amount: PositiveMoney<USD>) : Account
  requires amount.amount > 0.0   # not just >= 0; deposit needs strict positive
  ensures  result.balance == acc.balance + amount.amount
= ...
```

UoM and refinements live in different "spaces" of the type
system. UoM tracks dimensional algebra; refinements track value
ranges and shapes. A single field can carry both.

### 7. Composition with effects

**Decision**: orthogonal. Predicates in `requires` / `ensures`
are pure (no effects); the function body still has whatever
effect row it declares.

```kai
fn fetch_amount(id: OrderId) : Decimal<USD> / DB
  requires String.length(id) > 0
  ensures  result >= 0.0
= DB.query("...")
```

The `requires` checks `id` length statically or at the call
site; the `ensures` checks `result` after the DB call returns.
Neither participates in effect inference.

### 8. Diagnostics

**Decision**: refinement and contract violations get
specialised messages with the predicate text, the offending
value, and the call site.

> **v3 status (2026-06-15, refs #86 / #750):** the runtime panic now
> emits clause kind, function name, rendered predicate, decl-site
> line/col, **the runtime value of the offending binding**, **and a
> `help: narrow` suggestion** for `requires` predicates of the simple
> `<ident> <cmp> <literal>` shape (piece-1 context + piece-2 value +
> piece-3 help). Both backends (C and LLVM) produce identical output.
> Concretely, today's output is:
>
> ```
> panic: requires violated in `divide`
> required: b != 0
> declared at line 15, col 14
>   = help: narrow `b` to `Real where != 0`
> argument b was: 0
> ```
>
> The help line's base type (`Real`) comes from the offending
> parameter's *declared* annotation, not the comparison literal: an
> `n != 0` over `n: Int` reads `Int where != 0`. A declared type is the
> right source for an edit suggestion — it is the spelling the user
> rewrites — and it is already present at the assert-insertion site
> (`wrap_with_contracts` runs in `parse_decl`), so no typer thread is
> needed. The help renders only for the single-ident shape piece-2
> covers; other shapes (multi-conjunct, non-scalar, `ensures`) fall
> through to piece-1 with no help.
>
> The help precedes the runtime `argument` line rather than closing the
> message as the aspirational form below shows: the help is static
> (it lives in the parse-time C string literal), the value is appended
> at runtime, so the static help comes first. Reordering to put help
> last would mean threading a separate `help` argument through both
> emitters and both `runtime.h` copies purely to permute two lines —
> not worth the blast radius (the help content is what matters, not its
> position). Still pending: the call-site `--> file:line:col` caret
> (needs the call-site span threaded into the assert) and the
> `or wrap in ensure(b) where != 0.0` half of the help (a return-type
> refinement, deferred). Issue #86 stays open to track those.

Aspirational target (call-site caret + the `ensure` half of the help,
tracked under #86):

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

The structured form (JSON, queryable like typed holes) is part
of the m11 diagnostics quality pass and the `kai effects --json`
proposed extension.

## Key use cases

### Money + Ledger (C2 Fintech toolkit)

```kai
type Account = {
  id:           AccountId,
  balance:      Decimal<USD> where >= 0,
  transactions: NonNegative,
}

fn transfer(from: Account, to: Account, amount: Decimal<USD>)
   : (Account, Account)
   / Audit + DB
  requires from.id != to.id
  requires amount > 0.0
  requires from.balance >= amount
  ensures  result.0.balance + result.1.balance == from.balance + to.balance
  ensures  result.0.balance >= 0      # redundant — type guarantees it
  ensures  result.0.transactions == from.transactions + 1
  ensures  result.1.transactions == to.transactions + 1
=
  let from2 = { from with balance = from.balance - amount,
                          transactions = from.transactions + 1 }
  let to2   = { to   with balance = to.balance + amount,
                          transactions = to.transactions + 1 }
  Audit.log_transfer(from.id, to.id, amount)
  (from2, to2)
```

What the compiler can prove statically:
- `result.0.balance >= 0` from `from.balance >= amount` (require)
  and `result.0.balance = from.balance - amount` (body shape).
- The two refinements on `transactions` (NonNegative) are
  preserved because we add 1 to a NonNegative.

What stays as runtime check:
- Conservation: `result.0.balance + result.1.balance == ...`
  involves arithmetic the typer does not symbolically evaluate.
- `from.id != to.id` — runtime check at entry.

The function body **cannot lie**: if the implementation gets
the conservation wrong, the runtime check on the `ensures`
catches it before any caller observes the broken state.

### Domain types throughout an app

```kai
type Email      = String where matches ~r/^[^@\s]+@[^@\s]+\.[^@\s]+$/
type Username   = String where matches ~r/^[a-z][a-z0-9_]{2,31}$/
type IPv4       = String where matches ~r/^\d{1,3}(\.\d{1,3}){3}$/
type HttpStatus = Int    where 100 <= self < 600
type SafeAge    = Int    where 0 <= self <= 150

fn register(email: Email, username: Username, age: SafeAge) : User / DB
  requires age >= 13   # COPPA at the function level
  ensures  result.id.matches(/^u_[a-zA-Z0-9]{16}$/)
= ...
```

Each call site gets either a literal that the typer validates,
or a parsed/sanitised value that crossed a `ensure(...)` /
match boundary. The function body can rely on every argument
matching its predicate without re-checking.

## Implementation in stage 2

### Typer changes

1. **Refinement type representation**:
   ```
   Ty = TyInt | TyReal | ... | TyDim(Ty, UnitExpr)
      | TyRefine(Ty, Predicate)
   Predicate = PCmp(CmpOp, PExpr, PExpr)
             | PAnd(Predicate, Predicate) | POr | PNot
             | PMatches(PExpr, Regex)
             | PCall(PreludePred, [PExpr])
   PExpr = PVar(Name) | PSelf | PResult | PLit | PField(PExpr, Name)
         | PArith(ArithOp, PExpr, PExpr)
   ```
   Refinements compose with UoM: `TyDim(TyRefine(TyReal, P), Unit)`
   and `TyRefine(TyDim(TyReal, Unit), P)` are canonicalised to
   the same form (refinement on the dimensioned base).

2. **Contract attachment**: `FnDecl` gains
   `requires: [Predicate]` and `ensures: [Predicate]`. Type-check
   each predicate in a scope where args (and `result` for
   `ensures`) are bound; reject if not boolean or if not in the
   decidable subset.

3. **Verification pass** (post-inference, pre-codegen): for every
   call site and every assignment to a refined type, attempt
   static proof:
   - Substitute the call-site argument types/refinements into
     the callee's predicate.
   - Run interval propagation + regex subsumption.
   - Mark proved predicates for omission; mark unproved for
     runtime check.

4. **Subtyping rules**: `Int where >= 0` is a subtype of `Int`,
   not the converse. Refinements on records propagate
   field-wise. Allow refinements only in explicit positions
   (function args, `let : T`, match arms) — no bidirectional
   inference into refinements, to keep the unifier simple.

### Parser changes

1. **`where` clause** on `type Foo = BaseT where Pred` — already
   single-token addition; predicate parses with the existing
   expression grammar restricted to the decidable subset.
2. **`requires` / `ensures` clauses** between fn signature and
   body. Both are repeatable; both end at the next clause or
   the `=` / `{`.
3. **`result` keyword** in `ensures` predicates only; reject
   elsewhere.
4. **`ensure(expr) where Pred`** as a primary expression that
   returns `Option<RefinedT>`.

### Codegen changes

1. **Function entry**: emit prologue with one `if !pred { panic }`
   per `requires` clause that the typer marked as runtime.
2. **Function exit**: emit epilogue with one `if !pred { panic }`
   per `ensures` clause not statically proved. `result` binds
   to the return value before predicates evaluate.
3. **Refined-type construction**: `ensure(...)` desugars to
   `if pred { Some(value) } else { None }`. Match-arm
   `p : RefinedT` desugars to `if pred(scrutinee) { bind p =
   scrutinee; arm body } else { fall through }`.
4. **`--release` flag**: drop runtime checks already proved
   statically. Runtime-only checks stay on (programmer can
   `assume` to opt out per-callsite, with audit trail).

### Stdlib changes

A small set of `[<refinement_pure>]` prelude predicates:

```kai
fn is_finite(x: Real) : Bool [<refinement_pure>]
fn is_nan(x: Real) : Bool    [<refinement_pure>]
fn length[T](xs: [T]) : Int  [<refinement_pure>]
fn is_some[T](o: Option<T>) : Bool [<refinement_pure>]
fn is_none[T](o: Option<T>) : Bool [<refinement_pure>]
fn matches(s: String, r: Regex) : Bool [<refinement_pure>]
```

The set is closed in v1: extending it later is additive.

### Roadmap proposed

New milestone **m12.6 — Refinements + Contracts**, after
m12.5 (Units of Measure), before m5 (Perceus). Reasons:

- Independent of effects/fibers (m7/m8). Does not block
  anything.
- Composes with m12.5 (UoM) — same architectural neighbourhood,
  same level of typer change.
- Substantially benefits the C2 Fintech toolkit and the
  adoption-doc bet B (audit trail compile-time).
- Isolated change to typer + parser + codegen prologues. No
  effect-row or monomorphisation impact.

Estimate: **5-7 days at observed velocity**. Refinements +
contracts share most of the implementation (predicate
evaluator, interval propagation, runtime check insertion); the
only contract-specific extension is `requires` / `ensures`
parsing and prologue/epilogue codegen, which is ~1-2 days on
top of the refinement-types base.

Alternative: split into m12.6 (refinements) + m12.7 (contracts)
if mid-flight pressure dictates. Splitting is cheap because
the components are orthogonal in parser/codegen; the main cost
is doc duplication.

## Risks

1. **Slope to "we need a bit more"**. The first user request
   will be `where x < other_field` (relational across multiple
   args). That is the SMT cliff. **Mitigation**: explicit
   doc that v1 is restricted; provide `requires` / `ensures`
   as the relational form, even if runtime-only.

2. **Subtyping subtleties**. `Int where >= 0` is a subtype of
   `Int`; the converse needs explicit narrowing. Bidirectional
   inference into refinements is the trap. **Mitigation**:
   refinements only in explicit declared positions; never
   inferred from a base type.

3. **Regex evaluator in the typer**. Adding a regex engine to
   the compiler is non-trivial. **Mitigation**: use a subset
   (anchored only, no backreferences, no lookaheads); reuse
   the runtime regex library at compile time, do not write a
   new one.

4. **Runtime cost**. Each call site with unproved predicates
   gets one `if` per predicate. **Mitigation**: `--release`
   drops proved predicates; runtime-only ones are by design
   (Ada SPARK model).

5. **Diagnostics quality**. "Predicate violated at line 42"
   without context is useless. **Mitigation**: budget ~0.5-1
   day specifically for diagnostics — show the predicate text,
   the offending value, suggest a narrowing helper. *v2 (refs
   #86) shipped predicate text + clause kind + function name +
   decl-site line/col + the runtime value of the offending
   binding (for simple `requires` predicates, both backends).
   The call-site span caret and the narrowing-help suggestion
   remain the follow-up tracked under the same issue.*

6. **Composition with UoM**. `Decimal<USD> where >= 0`
   composes correctly when canonicalised; but error messages
   need to render the combined form readably. **Mitigation**:
   pretty-printer pass for combined types, tested as part of
   the diagnostics work.

## Three open questions to decide

1. **`where` keyword**: `where` (Pony, F#-style) vs
   `with constraint` vs `:` (Pascal-subtype-style).
   **Recommendation**: `where`. Reads in English, no conflict
   with kaikai's existing keywords, matches Pony.

2. **Where contracts attach**: between signature and body (as
   above) vs after `fn` (Ada `Pre => ...` aspect-style).
   **Recommendation**: between signature and body. Eiffel
   form is more readable in multi-line.

3. **Are contracts statically erased in `--release`?**: yes for
   proved predicates, **no** for runtime-only (security risk
   to drop unproven invariants). **Recommendation**: as
   stated; align with Ada SPARK precedent. Provide explicit
   `assume(pred)` opt-out per call site, audited.

## Executive summary

- **Yes, it fits**. Same conceptual neighbourhood as effects in
  types and Units of Measure. Tier 1 + Tier 3 alignment is
  natural; Tier 2 is unaffected.
- **Yes, it is worth it**. Closes the third leg of the
  adoption-doc bet B (audit trail compile-time): effects say
  what touches what, refinements say what values are valid,
  contracts say what operations guarantee.
- **Implementable**. ~5-7 days at observed velocity. Most of
  the cost is shared between refinements and contracts —
  splitting them doubles the doc work without saving
  implementation work.
- **When**: new milestone **m12.6** between m12.5 and m5. Can
  be brought forward to before C2 Fintech if the pitch needs
  the third leg in place by demo time.
- **Risks manageable**. Ada/Eiffel/Pony/D have proved every
  one of these decisions. The only research-shaped risk is
  the regex evaluator subset; everything else is engineering.

Recommendation: schedule formally as m12.6 in
`docs/stage2-design.md` immediately after m12.5; consider a
joint m12.5+m12.6 pre-flight if the C2-pre fintech demo wants
both UoM and refinements together.

## Implementation status — 2026-04-27

> **Enforcement status (2026-07-10, refs #1169):** refinement-type
> predicates are now **enforced at the three downcast sites** —
> annotated bindings (`let x: Refined = v`, `var`), refined
> parameters, and refined returns. A closed value that refutes the
> predicate is a **compile error** (mirror of the call-site literal
> check); a dynamic value gets a runtime check that panics with the
> structured diagnostic (`refinement violated in ... / predicate: ...
> / declared at ...`, same shape as `requires`/`ensures`). Checks are
> **omitted** where the predicate is proven — constant folding, or
> entailment from the value's own refined binding — so proven happy
> paths pay nothing. `var` cell *reassignment* is not re-checked
> (only the initial value); that follow-up belongs to the typer-side
> narrowing work.

**Parser-side landed (sub-lanes a–e).** `BaseT where Pred`,
`requires` / `ensures` clauses, the `result` binding, parse-time
constant folding, and the `ensure(value) where pred` primary
expression all ship in `stage2/compiler.kai`. The factorials and
collatz demos use them; both flip from PASS-no-golden to OK on
`make demos-no-regression`.

What this means in practice for code today:

- `type Port = Int where 1 <= self and self <= 65535` parses
  and the predicate is preserved on the syntactic TypeExpr. The
  semantic type drops to `Int` (so `Port` is a nominal alias of
  `Int`, no implicit narrowing), but everything that walks
  TyKind treats `TyRefine(base, _)` as transparent — display,
  protocol dispatch, alias expansion, etc.
- `requires P` and `ensures Q` on a fn signature lower to
  asserts that wrap the body. The current shape (m12.6.b/c):
    fn foo(x: T) : R requires P ensures Q = body
  desugars to
    fn foo(x: T) : R = {
      assert P
      let result = body
      assert Q
      result
    }
  so `result` is a real local binding inside `ensures`. Ordering
  — all `requires` first, then the body bind, then all `ensures`
  — matches the spec.
- The const folder of m12.6.d evaluates closed boolean / Int
  expressions and drops trivially-true predicates at parse time
  (zero runtime cost). Predicates that mention runtime values
  (function args, `result` after the bind) stay as runtime
  asserts.
- `ensure(v) where Pred` returns `Option[T]`: `Some(v)` when the
  predicate holds, `None` otherwise. Recognised only when the
  call is followed by `where`, so user code that defines a
  function called `ensure` keeps working.

What is **not** done yet (deferred from the original plan):

- **Refinement-aware unify / `TyRefineT` semantic type.** The
  semantic side still drops the predicate, so `Int where >= 0`
  unifies as `Int` and the typer cannot reason about the
  refinement. This blocks (a) static interval propagation
  through refined arguments, (b) implicit upcast `Int where >= 0`
  ⊑ `Int`, (c) match-arm narrowing `p : RefinedT`, and (d)
  composition with UoM at the semantic level.
- **`[<refinement_pure>]` stdlib annotations.** The attribute is
  spec'd but not parsed yet because there is no enforcement pass
  to rule against impure calls in predicates.
- **Compile-time errors for trivially-false predicates.** The
  const folder sees `assert false` but emits the runtime assert
  anyway; promoting to a compile-time diagnostic is a future pass.
- **Regex predicate subsumption.** Regex literals lex + parse +
  evaluate at runtime via the `~r/.../` sigil + `matches` predicate
  (m12.6.x #7 lane, 2026-05-03). Static *containment* — proving
  `String where matches ~r/^[a-z]+$/` ⊑ `String where matches
  ~r/^[a-z0-9]+$/` at the typer level — is the remaining piece and
  follows the same trajectory as numeric interval subsumption.

The honest one-line summary: kaikai now has the **syntax** and the
**runtime semantics** of refinements + contracts. The **type-level
reasoning** (interval propagation, subtyping, narrowing) is the
piece that remains, and it requires reintroducing the refinement on
the semantic Ty side.

**Follow-up lanes** for items deferred from m12.6 v1 are tracked
as GitHub issues with the `refinements` label: issue #83
(static interval propagation — alpha / operator / call-site
substitution), issue #84 (`[<refinement_pure>]` inline placement),
issue #85 (regex literals), issue #86 (predicate-aware
diagnostics). The original load-bearing item (`TyRefineT` on the
semantic side) landed 2026-04-27.

## Sources

- [Design by Contract — Bertrand Meyer 1986](https://se.inf.ethz.ch/~meyer/publications/computer/contract.pdf)
- [Eiffel Programming Language — preconditions, postconditions, invariants](https://www.eiffel.org/doc/eiffel/Design_by_Contract%20%28DbC%29)
- [Ada 2012 Pre/Post — RM 6.1.1](http://www.ada-auth.org/standards/12rm/html/RM-6-1-1.html)
- [Ada Subtypes — RM 3.2.2](http://www.ada-auth.org/standards/12rm/html/RM-3-2-2.html)
- [SPARK Ada — Static Verification](https://www.adacore.com/about-spark)
- [D Language Contracts](https://dlang.org/spec/contracts.html)
- [Pony — Type Refinements](https://tutorial.ponylang.io/types/type-aliases.html)
- [Liquid Haskell — Refinement Types](https://ucsd-progsys.github.io/liquidhaskell/) *(reference for what kaikai does NOT adopt)*
- [Dafny — Verification-Aware Programming](https://dafny.org/) *(reference for what kaikai does NOT adopt)*
- [Iron — Refinement Types for Scala 3](https://github.com/Iltotore/iron)
- [Code Contracts for .NET (deprecated)](https://learn.microsoft.com/en-us/dotnet/framework/debug-trace-profile/code-contracts) — historical Spec# successor
