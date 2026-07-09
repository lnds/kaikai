# syntax sugars

This is the authoritative specification for the five call-site
syntactic sugars shipping in milestone **m7b**. They are
**general language features**, not effect-specific extensions:
they apply anywhere the grammar allows, and the parser /
grammar treats this document as the single source of truth for
their shape, precedence, and desugaring.

The five sugars:

1. **Trailing lambdas** — the last `() -> T` argument of any
   call can be written as a block outside the parens. The
   block-form `{ x -> body }` is also legal as a standalone
   lambda expression in non-trailing positions (operand of
   `|` / `|>`, RHS of `let`, etc.).
2. **Capability read/write** — a naked read `cap` and
   `cap := v` as short forms for capability ops on `State[T]`
   and `Reader[T]`.
3. **Local mutable cells** — `var x := init` declares an
   in-block `State[T]` handler.
4. **Array indexing** — `a[i]` and `a[i] := v` for
   `Mutable.array_get` / `Mutable.array_set`.
5. **Double trailing lambdas** — when the last two arguments
   are both function type, both may be written as trailing
   blocks (`while { cond } { body }`, `if_then_else(p) { a }
   { b }`).

Each sugar desugars before type checking — the checker sees
only the post-desugar AST, so effect-row inference is
unchanged by anything in this doc.

## Context

The sugars were introduced informally in `docs/effects-stdlib.md`
next to the effects that motivate them (so the reader meets each
sugar where it is useful). This document is where the grammar
actually lives:

- Trailing lambdas — Doc B §*Syntax note: trailing lambdas*
  motivates; §1 below specifies, including the lambda-block
  expression form.
- naked cell read / `cap := v` — Doc B §*Syntax note: capability
  read/write sugar* motivates; §2 below specifies.
- `var` — Doc B §*Syntax note: local mutable cells with `var`*
  motivates; §3 below specifies.
- `a[i]` / `a[i] := v` — Doc B §*Syntax note: array indexing*
  motivates; §4 below specifies.
- Double trailing lambdas — motivated by `while`, `until`,
  `if_then_else`, and similar control-flow helpers built as
  ordinary stdlib functions (see `docs/stdlib-layout.md` §`loop`);
  §5 below specifies.

Doc A (`docs/effects.md`) §*Handling* records that the
`handle { body } with Eff { ... }` form already uses the
trailing-lambda mechanism; that's the first concrete use of §1
and a cross-reference back to Doc A.

## Design principles

The five sugars together add:

- one keyword: `var`.
- three binding shapes: `cap := v`, `a[i] := v`, `var x := v`.
- one naked-read form: a bare `cap` identifier.
- one postfix form: `a[i]`.
- two call-site shapes: trailing lambda, and a second
  trailing-lambda slot for two-block control-flow helpers.
- the block-form `{ x -> body }` accepted as a standalone
  lambda expression wherever a `Primary` is legal.

Two filters justify accepting this surface:

1. **Each sugar has one intent the long form obscures.** A
   naked `cap` is a capability read, not any other kind of
   expression; `cap := v` is a capability write on `State`;
   `var` is a local cell; `a[i]` is array indexing. Doc A's
   "few forms, each with clear intent" standard.
2. **None replaces an existing form.** The long forms —
   `counter.get()`, `counter.set(v)`, `Mutable.array_get(a, i)`,
   `handle { ... } with State[T](init) as name { ... }` —
   remain legal. Sugar is additive.

The sugars that were **rejected** during Doc B review are also
instructive (see §*Deliberately not on this list* at the end).

## 1. Trailing lambdas

### Rule

A call expression whose **last argument has function type
`(...) -> T` or `(...) -> T / e`** may be written with that
argument as a block outside the parens:

```kai
f(arg1, arg2) { body }               ≡   f(arg1, arg2, () => { body })
f(arg1, arg2) { x -> body }          ≡   f(arg1, arg2, (x) => { body })
f(arg1, arg2) { x, y -> body }       ≡   f(arg1, arg2, (x, y) => { body })

# Single-argument calls may omit the parens entirely:
f { body }                           ≡   f(() => { body })
f { x -> body }                      ≡   f((x) => { body })
```

The `x ->` (or `x, y ->`) binder before the body names the
lambda's parameters; omit it when the lambda takes zero
arguments.

The source form uses `->` (not `=>`) on purpose, matching Doc
A §*Handling* clauses (`op(args, resume) -> expr`,
`return(x) -> expr`). kaikai distinguishes two shapes:

- `(x) => body` — a **lambda expression**: a function value
  that stands on its own.
- `{ x -> body }` — a **block with parameters**: the body of
  a clause or trailing lambda, where the arguments are
  declared by the surrounding form.

The desugar target on the right-hand side of each equivalence
above is a lambda expression, so `=>` appears there. The
block-form with `->` on the left-hand side is what the user
writes; the checker translates it to the expression-form with
`=>` internally.

### Grammar delta

```
Call        ::= Primary "(" Args ")" TrailingLambda?
              | Primary TrailingLambda           # paren-free: single-lambda call
TrailingLambda ::= "{" (LambdaParams "->")? Block "}"
LambdaParams ::= Ident ("," Ident)*
```

**Attachment rule.** A `TrailingLambda` attaches to its
preceding `Primary` (with or without `"(" Args ")"`) only when
there is no newline between them. A newline separates them into
two statements: `Primary` as an expression-statement and
`{ ... }` as a standalone block expression. Same discipline as
Kotlin and Swift closures. See §*Ambiguity with block
expressions* for worked examples.

The check "the last declared argument has function type" is a
type-checker concern, not a parser concern — the parser
accepts any call followed by a same-line `{ ... }`; the checker
rejects if the declared arity and types do not align.

### Examples

```kai
# stdlib effect helpers:
try { body }
with_default(0) { compute() }
with_state(0) { body }
nursery { n -> n.spawn(task_a) }
with_reader(env) { body }
map(xs) { x -> x * 2 }
each(xs) { x -> Console.print(x) }
```

### Relation to `handle`

`handle { body } with Eff { clauses }` is a control-flow
construct of the language (see Doc A §*Handling*). `handle` and
`with` are reserved keywords; the form has its own grammar
production and is not a function call with a trailing block.
Both blocks (`{ body }` and `{ clauses }`) are parsed by
handle-specific rules: the body is an ordinary block expression,
and the clauses block contains handler clauses (`op(args,
resume) -> expr`) which are not ordinary expressions. Trailing
lambdas (this document's main subject) do not interact with
`handle` at all.

### Ambiguity with block expressions

Blocks exist as ordinary expressions: `{ stmt1; stmt2 }` is a
value, and it is legal in statement position as a standalone
block. Any sequence "expression followed by `{ ... }`" therefore
has two possible parses:

- one call with a trailing lambda, or
- two statements: the expression, then the block.

The whitespace rule from §*Grammar delta* resolves it: **a
trailing lambda attaches to its preceding `Primary` only when
they are on the same line** (no newline between them). Newline
terminates the statement; whatever follows is a fresh
production.

With parens (the always-legal form):

    f() { body }         # one call: f(() => { body })

    f()
    { body }             # two statements: f() , then the block { body }

Without parens (the `f { body }` paren-free form only applies
to single-lambda calls):

    f { body }           # one call: f(() => { body })

    f
    { body }             # two statements: f reference, then the block

The first of each pair attaches because `f` and `{` are on the
same line. The second does not, because the newline closes `f`
as its own statement.

### Editorial preference

Trailing lambdas are additive: `f(() => { body })` and
`f() { body }` are both legal, and the compiler accepts either.
Style guidance for stdlib and project code:

- Prefer the **trailing form** when the lambda is the only
  argument, or when it carries the intent of the call (`try`,
  `with_state`, `nursery`, `each`).
- Prefer the **paren form** when the lambda is one of several
  arguments of roughly equal weight (`map(xs, (x) => x + 1)`
  reads as a transformation over `xs`, not as "do something to
  `xs`").
- When in doubt, the trailing form is usually the right call —
  `f(arg) { body }` reads as "do `{ body }` with `arg`
  configured", which matches the intent of most effectful
  helpers.

### Lambda-block in non-trailing positions

The block-form `{ x -> body }` (or `{ body }` for zero arity) is
also legal as a **standalone lambda expression** in any position
where an expression is legal — not only in trailing position.
This is the form preferred when the lambda is the right operand
of a binary pipe such as `|` (map pipe) or `|>`:

```kai
[1..10] | { x -> x * x }              # squared list
xs |> { ys -> list.length(ys) }       # apply a custom transform
let inc = { x -> x + 1 }              # bind to a variable
```

The desugar is identical to `(x) => body`: both produce the same
lambda expression. The two forms coexist intentionally and signal
different intents (CLAUDE.md cross-cutting principle: *few forms,
each with clear intent*):

- `(x) => body` — **lambda-as-value**: assigned to a binding,
  passed in paren-form, returned from a function. Reads as a
  function value standing on its own.
- `{ x -> body }` — **lambda-as-block**: the body of a call's
  intent (trailing position) or a transformation that reads
  better as a block (operand of `|` / `|>`, multi-line bodies).

The same-line attachment rule from §*Grammar delta* still
applies: `xs | { ... }` parses as `xs | (lambda)` only when `|`
and `{` are on the same line. A newline between them splits the
two into separate statements (the second of which would be a
standalone block expression).

## 2. Capability read/write: naked read and `cap := v`

### Receiver classes

Three receiver shapes accept this sugar today; the desugar dispatches
on the receiver's type:

| Receiver shape | naked `cap` lowers to | `cap := v` lowers to |
|---|---|---|
| `State[T]` (an `as`-bound handler capability) | `cap.get()` (resolves to `State.get`) | `cap.set(v)` |
| `Reader[T]` (an `as`-bound handler capability) | `cap.get()` (alias-rewritten to `Reader.ask`) | rejected — `Reader` has no `set` op |
| `Ref[T]` (a value of type `Ref[T]`, e.g. a `let` or fn parameter) | `Mutable.ref_get(cap)` | `Mutable.ref_set(cap, v)` |

The first two require an `as`-bound name (rule 4 below). The third —
added in issue #275 — works on any value typed `Ref[T]` regardless of
how it was bound, including a function parameter, mirroring the way
`a[i] := v` auto-injects `Mutable` into the row for arrays (issue
#265). `Ref[T]` itself comes from `Mutable.ref_make[T](init)` and
crosses function boundaries, which `var` cannot.

### Rules

Four restrictions, lifted verbatim from Doc B:

1. A naked read applies **only to `State[T].get()`,
   `Reader[T].ask()`, and `Mutable.ref_get(c)` for `c : Ref[T]`**.
2. `cap := v` applies **only to `State[T].set(v)` and
   `Mutable.ref_set(c, v)` for `c : Ref[T]`**.
3. A naked-read identifier must be **a simple capability
   binding** (`counter`, not `config.section.level`).
4. The naked read and `:=` on a `State[T]` / `Reader[T]`
   capability require **an `as`-bound capability name**. Using
   the default capability name (`State` itself) keeps the
   explicit `State.get()` / `State.set(v)` form. (`Ref[T]` has
   no "default name" — every `Ref[T]` value is bound by an
   ordinary `let` or parameter, so this rule does not apply
   to the `Ref` shape.)

   Reason for rule 4: without `as`, the default capability's
   identifier is also the effect's name (`State`). A bare
   `State` already names the effect, and `State := v` would
   read as an assignment to the effect rather than to a cell.
   Requiring an `as`-bound name sidesteps both: the sugar
   applies only to identifiers the user has introduced as
   bindings, and the default form stays as an explicit method
   call.

### Grammar delta

The read needs no new production: a capability read is an
ordinary `Ident` in `Primary` position; the checker rewrites it
once it knows the identifier resolved to a capability binding.
Only the write adds a statement form:

```
Stmt        ::= ...
              | Ident ":=" Expr                  # capability / array write
```

Both are resolved by the checker, which verifies the
identifier's type. `StateCap[T]` and `ReaderCap[T]` below are
the internal types the compiler assigns to `as`-bound
capabilities of the corresponding effects — they are checker
implementation details, not names users spell in source code.

- a naked read of `x` lowers to the get/ask op iff
  `x : StateCap[T]` (for any `T`), `x : ReaderCap[T]`, or
  `x : Ref[T]`; otherwise `x` stays an ordinary variable read.
- `x := v` legal iff `x : StateCap[T]` and `v : T`, or
  `x : Ref[T]` and `v : T`, **or** the array-indexing form below.

When `x : Ref[T]`, the call site adds `Mutable` to the enclosing
function's effect row (issue #275, mirroring the `a[i] := v` /
`Mutable.array_set` row injection from issue #265).

If `x := v` targets an identifier without a capability type, the
error points at the sugar site and suggests either the method
call (`x.set(v)`) or the explicit long form (`State.set(v)`).

### Why the read is naked

`:=` is the single mark of mutability. It declares the cell
(`var x := init`), and every write spells it (`x := v`), so the
cell is already visible at its declaration and at each mutation.
Marking the read too would be redundant: a bare `counter` reads
the current value, and the surrounding `:=` discipline keeps the
capability visible per Doc A §*Context*'s capability-explicit
stance.

`@` carries no expression-level role: its only use is the
as-pattern (`name @ subpattern` in `match`), documented in
`docs/proposed-extensions.md` §14. That usage is **infix** in
patterns, so there is no clash with the naked read in
expressions.

## 3. Local mutable cells: `var`

### Rule

```kai
var name: T := init      # with type annotation
var name := init         # with inference
```

desugars, in the same block, to:

```kai
handle { /* rest of block */ } with State[T](init) as name {
  get(resume)    -> resume(state)
  set(v, resume) -> resume((), v)
  return(x)      -> x
}
```

where "rest of block" is every statement from the `var`
declaration down to the closing `}` of the enclosing block.

The `name` of the `var` declaration becomes the `as` binding of
the synthesised `handle` — there is no separate step. Scope and
shadowing rules for that binding are in §*Scope* below.

Because the synthesised `handle` opens and closes `State[T]`
inside the same block, the enclosing function's effect row
does **not** gain `State[T]` — the cell is purely local. See
§*Effect-row impact* below for the full statement.

### Grammar delta

```
Stmt        ::= ...
              | "var" Ident (":" Type)? ":=" Expr
```

`var` is a **new keyword**. It is the only keyword the sugars
in this document add; every other sugar is a symbol or
grammar shape.

When the `: Type` annotation is absent, the desugar emits a
fresh type variable in place of `T`; the type checker then
infers `T` from `init` through ordinary Hindley-Milner
generalisation, same as an unannotated `let`.

### Scope

The cell lives from its `var` declaration to the next `}`.
Rules that follow from the desugar:

- Sibling `var` declarations each become their own nested
  `handle`, innermost first.
- A `var` inside an inner block (e.g., an `each` callback)
  dies at that inner block's `}` and is not visible to
  sibling statements of the outer block.
- The cell's name shadows outer bindings per the usual
  scoping rules, and pollutes nothing beyond its block.
- A `var` declaration may also appear at the top of a
  handler-clause list (before all operation clauses, sibling
  to them inside `handle { ... } with Eff { ... }`). It
  desugars to a sibling `with State[T](init) as name` wrapping
  the handler. Reads inside clause bodies use the same
  naked `name` / `name.get()` forms as anywhere else; `var` and
  `let` must precede every operation clause inside one
  handler block (interleaving is a parse error).

### Effect-row impact

Because the implicit `handle` closes `State[T]` in the same
block, the enclosing function's effect row **does not** gain
`State[T]`. A `var counter := 0` inside a function of signature
`: Int / Console` leaves the signature untouched.

`var` encapsulates `State[T]` and nothing else. Any other
effect the body uses — `Console.print(counter)`,
`File.write_file(...)`, `Spawn.yield()` — still appears in the
enclosing function's row, same as without the `var`. The sugar
is narrow on purpose: it makes one local cell invisible; it
does not hide anything else about what the body does.

### Performance

The compiler recognises the canonical clauses (the ones
`var` emits) plus the lack of `resume_multishot` and
specialises the `handle { ... } with State[T](init) { ... }`
into a stack-allocated mutable slot. Koka calls this
"variable specialization". After LLVM's `mem2reg`, the
generated code is identical to what a C-style mutable `int`
compiles to. Full details live in Doc C; for this document
the invariant is *`var` has zero runtime overhead in the
common case*.

Specialisation applies when the cell does not escape its
scope. If a closure capturing the cell escapes (passed to
`Spawn.spawn`, returned out of the block, stored in a heap
structure), the specialisation cannot fire and the handler
falls back to the generic CPS-threaded desugar — still
correct, no longer zero-overhead. Doc C pins the exact escape
analysis.

## 4. Array indexing: `a[i]` and `a[i] := v`

### Rules

```kai
a[i]         ≡   Mutable.array_get(a, i)      # read
a[i] := v    ≡   Mutable.array_set(a, i, v)   # write
```

Both require `a : Array[T]` (for some `T` inferred at the call
site) and `Mutable` in the effect row. The checker enforces
both; a missing `Mutable` produces the standard
"`Mutable` not handled" error with a hint about wrapping in
`with Mutable { ... }` or adding `Mutable` to the signature.

Out-of-bounds access (`i < 0` or `i >= Mutable.array_length(a)`)
panics via the runtime's audited escape (`kai_prelude_panic`);
`Mutable` ops do not return `Option[T]` for out-of-bounds. Same
semantics in the sugar as in the underlying ops — see Doc B
§`Mutable` for the full statement.

### Grammar delta

```
Postfix     ::= Primary ("[" Expr "]" | "." Ident | "(" Args ")")*
Stmt        ::= ...
              | IndexLhs "[" Expr "]" ":=" Expr   # indexed write
              | Ident ":=" Expr                   # capability write (shared rule from §2)
IndexLhs    ::= Ident ("." Ident)*
```

`a[i]` as an expression is a postfix application — same slot
as `a.field` and `a(x)`, with no restriction on what precedes
the `[`. `a[i] := v` as a statement is restricted: the
`IndexLhs` before the `[` must be an identifier possibly
followed by field accesses (`counter[0] := v`,
`config.scores[i] := v`). Expressions ending in a call
(`get_array()[i] := v`) are rejected because they mutate a
temporary the caller cannot observe elsewhere — almost always
a bug. A programmer who genuinely wants to write through a
call-returned array binds it first (`let xs = get_array(); xs[i]
:= v`).

The parser picks the correct desugar from the LHS shape: a
plain identifier is a capability write (§2); an `IndexLhs`
followed by `[...]` is an array write.

### Map dispatch (issue #128)

When `e1 : Map[K, V]`, the same `e1[e2]` postfix lowers instead
to `map_get(e1, e2) : Option[V]`. The typer picks the lowering
by inspecting the inferred type of `e1` at the index site;
`Array[T]` keeps the `array_get` semantics above unchanged, and
unresolved type variables default to `Array[T]` so the existing
"expected `Array[T]`" diagnostic still fires when an unannotated
local is indexed without a constructor hint.

```kai
let registry : Map[String, Pid[Cmd]] = registry_empty()
let pid = registry["worker-3"]   #  ≡  map_get(registry, "worker-3") : Option[Pid[Cmd]]
```

The write-side sugar `m[k] := v` is **not** supported on
`Map[K, V]` in v1. The typer emits a diagnostic that points
at `map_put(m, k, v)`; supporting an indexed write over a
persistent container needs a separate design pass and ships
alongside the HAMT carrier in v2 (issue #128's *Out of scope*
list, mirrored in `docs/proposed-extensions.md` §6's
retrospective).

### What it does **not** extend to

Two deliberate non-targets:

- **`Ref[T]`** — reading and writing refs stays as the
  explicit `Mutable.ref_*` op form. `Ref` is a low-level tool
  used mostly by stdlib / FFI adapters, where explicit ops
  double as documentation (Doc B §`Mutable` *Idiomatic usage*).
- **Slice syntax** — `a[i..j]` is not in v1. Tracked in
  `docs/proposed-extensions.md` §7; lands alongside
  `Vector[T]` post-MVP, with a separate grammar rule.

## 5. Double trailing lambdas

### Rule

A call whose **last two arguments are both function type** may
be written with both trailing as blocks:

```kai
f(arg)        { x -> body1 } { y -> body2 }
            ≡ f(arg, (x) => { body1 }, (y) => { body2 })

f(arg1, arg2) { body1 }      { body2 }
            ≡ f(arg1, arg2, () => { body1 }, () => { body2 })

# Zero-argument call with two trailing lambdas:
f { cond } { body }
            ≡ f((() => cond), (() => { body }))
```

The canonical use is control-flow helpers built as ordinary
stdlib functions:

```kai
while      { i > 0 } { i := i - 1; io.println("#{i}") }
until      { done }  { process_one() }
if_then_else(p)      { run_a() } { run_b() }
```

These compose with row polymorphism the same way single-trailing
calls do — the row variable on the two lambda parameters
unifies, and the call's result row is the union.

### Grammar delta

```
Call           ::= Primary "(" Args ")" TrailingLambda? TrailingLambda?
                 | Primary                TrailingLambda  TrailingLambda?
```

The optional second `TrailingLambda` is greedy: when two `{...}`
blocks follow a callable on the same line, the parser attaches
both. The same-line discipline from §1 *Grammar delta* extends
to the second block: a newline before either `{` terminates the
call and starts a new statement.

### Type-check rule

When two trailing lambdas are present, the call's declared
arity must be at least two and the **last two declared
parameters must both have function type**. The checker reports
the same diagnostic shape as single-trailing for arity or type
mismatches; the second lambda's expected type is the
penultimate declared parameter, the first lambda's expected
type is the last.

### Editorial preference

Use double trailing **only when the two lambdas have visibly
distinct intents** — predicate + body, then-branch + else-branch,
condition + action — so the reader can tell which is which from
position alone. When both lambdas play similar roles
(`map_two(xs, f, g)`), keep paren form; the position of two
indistinguishable blocks is more confusing than verbose.

Three constructs that benefit clearly:

- **`while { cond } { body }`** — predicate then action.
- **`if_then_else(p) { a } { b }`** — then-branch then else.
- **`with_resource(r) { setup } { teardown }`** — RAII-shaped
  bracketed-init pattern.

### Ambiguity with block expressions

Same resolution as §1: blocks-as-expressions and trailing-
lambda blocks differ by **same-line attachment**. With two
trailings the rule generalises:

```kai
f { a } { b }            # one call: f((() => a), (() => b))

f { a }
{ b }                    # one call f((() => a)), then a standalone block { b }

f
{ a } { b }              # f reference, then two standalone blocks
```

The first `{` after the callable starts a trailing lambda only
when same-line; the second `{` is consumed greedily under the
same rule. Newlines terminate.

## 6. n-tuples — `(a, b, ..., n)` parser sugar

**Issue #154**, shipped 2026-05-03. Parser-only desugar to existing
stdlib record types — no new TyCtor, zero typer/RC cost.

```kai
# Construction
(a, b)                ≡  Pair  { fst: a, snd: b }
(a, b, c)             ≡  Triple { fst: a, snd: b, trd: c }
(a, b, c, d)          ≡  Quad  { fst: a, snd: b, trd: c, frt: d }

# Type
(A, B)                ≡  Pair[A, B]
(A, B, C)             ≡  Triple[A, B, C]
(A, B, C, D)          ≡  Quad[A, B, C, D]

# Pattern (let-destructure or match arm)
let (a, b) = pair     ≡  let Pair { fst: a, snd: b } = pair
let (a, b, c) = trip
let (a, b, c, d) = q

# Field access stays nominal — same as Pair today.
pair.fst  pair.snd
trip.fst  trip.snd  trip.trd
quad.fst  quad.snd  quad.trd  quad.frt
```

**Cap**: arity 2..4. Wider products should be a named record,
not a tuple. Arity 5+ is rejected at parse time with a
diagnostic naming the cap.

**Single-paren form `(e)` stays grouping**, never a 1-tuple
(same as Rust, OCaml). `()` keeps its existing meaning: unit
expression / zero-arg lambda head / `() -> T` function type.

**Disambiguation**:

- *Type position* — after parsing comma-separated types and
  consuming `)`, peek for `->`. If `->` follows, it's a
  function type `(A, B) -> C`. Otherwise it's a tuple type.
  Single-element `(A)` with no `->` is grouping (returns `A`).
- *Expression position* — `parse_paren_or_lambda` first tries
  the lambda-params scan, then falls back to expression. If
  after the first expression a `,` appears (and we did not
  open a lambda), accumulate further comma-separated exprs and
  desugar on `)`.
- *Pattern position* — new entry from `parse_pattern_rest`:
  `(` opens either a grouping `(p)` (single inner pattern, no
  comma) or an n-tuple pattern `(p1, p2, ...)` desugaring to
  `PVariantRecord("Pair" | "Triple" | "Quad", [...])`.

**Backward compat**: the existing record-literal form
`Pair { fst: x, snd: y }` continues to parse and typecheck
unchanged. The sugar is purely additive.

**Stdlib types**: `stdlib/core/tuple.kai` declares
`Pair[a, b]`, `Triple[a, b, c]`, `Quad[a, b, c, d]`. They
load automatically as part of the core prelude.

**Why named accessors, not `.0 / .1`**: the lexer accepts
only identifiers (and a few keywords) after `.`; numeric
field access would need a new lexer rule. Named accessors
stay consistent with the existing `Pair.fst / Pair.snd`
surface and don't add a third way to read a tuple.

## 7. Hex and binary integer literals

**Issue #156**, shipped 2026-05-03 (PR #160). Lexer-only
extension to integer-literal recognition — no new AST node,
no new type, no new operator.

### Rule

Integer literals may be written in hexadecimal (`0x` prefix)
or binary (`0b` prefix) in addition to decimal:

```kai
let mask  : Int = 0xFF
let cafe  : Int = 0xCAFE
let max64 : Int = 0x7FFFFFFFFFFFFFFF   # signed 64-bit max
let bits  : Int = 0b11111111
let port  : Int = 0b1111101000          # = 1000
```

The literal type is `Int` (signed 64-bit, the runtime's only
integer type). The AST node is `EInt` — the same node that
backs decimal literals. Constant-folding, refinement
propagation, monomorphisation, and Perceus see the same shape
they see for `42` or `1_000_000`.

### Grammar delta

The integer-literal token recognises three branches in the
lexer (`stage2/compiler.kai` — see `lex_hex_or_bin_int` and
`lex_scan_digits`):

```
IntLit   ::= "0x" HexDigit+
           | "0b" BinDigit+
           | DecDigit ("_"? DecDigit)*
HexDigit ::= [0-9a-fA-F]
BinDigit ::= [01]
DecDigit ::= [0-9]
```

A leading `0` followed by `x`/`X` or `b`/`B` enters the hex
or binary branch; any other prefix is decimal.

### What is **not** supported

- **Underscore digit separators in hex/bin**. `0xFF_FF` does
  not parse. Decimal literals already accept `_` as a
  separator (`1_000_000`); the hex/bin branches deliberately
  do not extend it. Open as a follow-up if demand arises.
- **Octal `0o`**. Not in the lexer. Hex covers the same
  byte-pattern use cases more readably.
- **Hex floating-point literals** (`0x1.8p3`). `Real` literals
  are decimal-only.
- **Negative literal prefix in the literal itself**. `-0xFF`
  parses as the unary-minus operator applied to `0xFF`, the
  same as `-42`.

### Diagnostics

A malformed literal — `0x` with no hex digits, `0b` with no
binary digits, or a stray non-hex/non-bin character mid-body
— emits a contextual error pointing at the bad position.
Fixtures: `examples/literals/hex_malformed.kai`,
`examples/literals/binary_malformed.kai`.

### Why no new AST node

The whole point of the feature is **lexer-only**. The parser,
typer, monomorphiser, Perceus pass, and codegen all see
`EInt(n)` exactly as they did before. This keeps the change
narrow: `0xFF` and `255` are interchangeable at every stage
after lexing.

## 8. Positional record construction — `T { v1, v2 }`

**Issue #266**, shipped 2026-05-05. Parser-only desugar to the
existing named-field record literal — no new AST node, no typer
change.

### Rule

A record-literal `T { ... }` may supply its field values
positionally, in declaration order, instead of by name:

```kai
type Complex = { re: Real, im: Real }
type Point   = { x: Int, y: Int }

let z = Complex { 1.0, 2.0 }     #  ≡  Complex { re: 1.0, im: 2.0 }
let p = Point   { 0, 0 }         #  ≡  Point   { x: 0, y: 0 }

# Named form keeps working unchanged:
let zn = Complex { re: 1.0, im: 2.0 }
```

### Disambiguation

The parser inspects the **first item** after `T {`:

- `IDENT :`  → named-field literal (existing path).
- `IDENT (, | })` → bare-ident punning (existing §10 form: `T { x, y }`
  desugars to `T { x: x, y: y }`).
- anything else (literal, call, expression, parenthesised expr, an
  ident followed by `+`, `(`, `.`, …) → positional list. Each
  comma-separated item is a full expression.

Intermediate items in the positional list cannot reintroduce
named-field syntax: `T { 1.0, im: 2.0 }` is rejected at parse time
with `expected positional or all-named record fields, found mixed
forms`.

### Desugar shape

The parser tags each positional value with a `__pos_<i>__` field-name
sentinel. A pre-typer pass (`desugar_pos_records_decls`) consults the
record-decl table and rewrites the sentinels into the real field
names in declaration order:

```
T { v1, v2, v3 }   →   T { f1: v1, f2: v2, f3: v3 }
```

After this rewrite the typer + emitter see the same AST shape as the
named form. The sentinel is the only piece of new state that exists
between parse and the desugar pass; no later walker needs to know
about positional construction.

### Validation

Two diagnostics:

- **Unknown record name** — `unknown record type \`T\` for positional
  construction; declare the type or use named-field form`.
- **Arity mismatch** — `\`T\` expects N positional fields, got M`.

Both fire at the desugar boundary and short-circuit the rest of the
compile so downstream invariant walkers never see a tagged AST.

### Editorial preference

- **Positional**: short, fixed-shape records where field order is
  obvious from the type name and the call site. `Complex { 1.0, 2.0 }`,
  `Point { x, y }`, `Pair { fst, snd }` (though §6's tuple sugar
  `(fst, snd)` reads better for Pair).
- **Named**: records with three or more fields, fields whose meaning
  is not order-obvious, or a refactor-prone shape (where reordering
  the type's declaration would silently rewire the call). `Color { r:
  255, g: 128, b: 0, a: 255 }` keeps named.

### What is **not** in v1

- **Function-style `T(v1, v2)`** — separate proposal, conflicts with
  the type-ctor model and with generic type application.
- **Partial positional with `..rest`** — n-tuple-style spread is a
  separate discussion.
- **With-update positional** (`{ z | 5.0, _ }`) — confusing
  semantics, not in v1.
- **Auto-derived positional `Show`/`Eq`** — independent proposal.

## 9. Complex literals — `<digits>i` suffix

**Issue #267 (Phase 1)**, shipped 2026-05-05. Lexer + parser
extension that lets numeric literals carry a trailing `i` to
denote a pure-imaginary `Complex` value. No new AST node — the
parser desugars the literal to a call against the stdlib
`complex.mk` constructor.

### Rule

A numeric literal followed *immediately* (no whitespace, no
intervening character) by `i` lexes as `TkComplex`:

```kai
let a = 3i        # → complex.mk(0.0, 3.0)
let b = 2.5i      # → complex.mk(0.0, 2.5)
let c = 1e10i     # → complex.mk(0.0, 1.0e10)
```

The literal's type is `Complex` (the record `{ re: Real, im:
Real }` from `stdlib/math/complex.kai`). The integer form `3i`
yields `re = 0.0, im = 3.0` — `Complex` has no integer
component.

### Identifier `i` keeps working

The contiguity rule preserves `i` as a free identifier — only
`<digits>i` (with the `i` directly after the digit/exponent
scan) attaches:

```kai
let i = 5
let x = i + 1      # `i` is the int variable, `i + 1` = 6
let y = 3 + i      # whitespace breaks the suffix → 8
let z = 3+i        # `i` after `+` is still an ident → 8
```

The lexer numeric scan stops at the first non-digit / non-`.` /
non-exponent character. Whitespace, operators, and parens all
terminate the scan before the suffix peek runs.

### Grammar delta

```
ComplexLit ::= IntLit "i"
             | RealLit "i"
```

The `i` is consumed only when adjacent to a numeric token at
lex time; the parser sees a single `TkComplex` whose span
covers `digits + i`.

### Desugar shape

`TkComplex` parses as a primary expression that produces:

```
ECall(EField(EVar("complex"), "mk"), [EReal(0.0), EReal(im)])
```

— exactly the same AST a user gets from writing
`complex.mk(0.0, im)` by hand. The resolver later rewrites the
qualified call to an `EModCall` once the `complex` module is
bound. `bin/kai` auto-imports `stdlib/math/complex.kai` as part
of the prelude (issue #245), so the desugared call resolves
without an explicit `import math.complex`.

### What is **not** supported (Phase 2 of #267, blocked by #180)

- **Heterogeneous arithmetic.** `2.0 + 3.0i` does *not* parse-
  through into a working `Complex` because `2.0 : Real` and
  `3.0i : Complex` need a `Real + Complex` operator dispatch.
  That requires `protocol P[A]` (#180). Until #180 lands, mix
  with an explicit lift:

  ```kai
  let z = complex.from_real(2.0) + 3.0i   # works (Complex + Complex)
  let w = 2.0 + 3.0i                       # FAILS — needs #180
  ```

- **`i` as a method or accessor on Real**. `2.0.i` would be
  field access on a Real, which has no `i` field — error.

### Why no new AST node

The whole point is to stay lightweight: lexer + 8-line parser
arm; the rest of the compiler (typer, Perceus, emit) sees a
plain `ECall` of `complex.mk` and threads everything through
the same paths used by hand-written `complex.mk(0.0, 3.0)`.

### Fixtures

- `examples/sugars/complex_literal_basic.kai` — `3i`, `2.5i`,
  `1e10i` lex and produce `Complex` values with the expected
  fields.
- `examples/sugars/complex_literal_with_complex.kai` —
  `complex.from_real(2.0) + 3.0i` works through the existing
  `impl Add for Complex`.
- `examples/sugars/complex_literal_ident_i.kai` — pins the
  identifier-`i` boundary so the lexer never over-attaches.

## 10. Record-spread sugar — `T { ...p, x: 10 }`

**Issue #326**, shipped 2026-05-07. Parser + pre-typer desugar that
fills missing record fields from a source value. The emitter sees
the existing `ERecordLit` shape — no new AST node, no new emit path.

### Rule

A record literal whose first item is `...expr` builds a new value
of `T` whose fields default to those of the source, with named
overrides replacing selected fields:

```kai
type Color = { r: Int, g: Int, b: Int, a: Int }

let red       = Color { r: 255, g: 0, b: 0, a: 255 }
let translucent = Color { ...red, a: 128 }
#                ≡ Color { r: 255, g: 0, b: 0, a: 128 }

let inverted  = Color { ...red, r: 0, g: 255, b: 255 }
#                ≡ Color { r: 0,   g: 255, b: 255, a: 255 }
```

This is the language's **functional update** form: every other path
to "same record but with one field different" was a hand-copy of
every untouched field.

### Restrictions in v1

- The spread MUST be the first item. `T { x: 10, ...p }` is rejected
  with `\`...\` must be the first item in a record literal — found a
  field before the spread`. Keeps the rule "spread first, overrides
  after" and avoids debate about override-direction precedence.
- Only **named** overrides may follow the spread. Pun (`{ ...p, x }`)
  and positional (`{ ...p, 10, 20 }`) are rejected. Pun would silently
  bind `x: x` from the local scope; v1 requires the explicit `x: x`.
- A second spread is rejected: `record literal admits a single \`...\`
  spread`.
- The source must match the outer record type. `P { ...q, ... }` where
  `q : Q` is rejected by the typer's nominal `let`-annotation check
  (the desugarer wraps the source in `let __spread_src__ : T = src`).
- No spread in record patterns. Pattern spread is a separate
  proposal — out of scope for this issue.
- No spread inside the positional record sugar (#266) — mixing the
  two is rejected.

### Desugar shape

The parser tags the spread's source with the `__spread__` field-name
sentinel, leaving the rest of the named overrides untouched. A
pre-typer pass (`dpr_record_spread` inside
`desugar_pos_records_decls`) detects the sentinel, looks up `T`'s
declared field list, and rewrites the literal into a let-block:

```
T { ...src, x: 10, y: 20 }

   →

{
  let __spread_src_<line>_<col>__ : T = src
  T {
    f1: __spread_src_<line>_<col>__.f1,
    f2: __spread_src_<line>_<col>__.f2,
    ...
    x: 10,
    y: 20,
    ...
  }
}
```

The let captures `src` once so a side-effecting source isn't replayed
across N field accesses (`{ ...make(), x: 1 }` calls `make()` exactly
once). Fields are emitted in `T`'s declared order; an override wins
over the corresponding spread copy. If `T` is generic, the let
annotation is omitted (the typer infers via the field-by-field check
instead — `T[a, b]` cannot be spelled without committing to concrete
type args).

After this rewrite the typer + emitter see the same AST shape as a
hand-written named record literal. The sentinel never reaches a
later pass.

### Validation

The desugarer reports four diagnostics; everything else falls through
to the existing record-literal pipeline:

- **Unknown record name** — `unknown record type \`T\` in \`...\`
  spread; declare the type before constructing it`.
- **Override of an undeclared field** — `record-spread override \`z\`
  is not a field of \`T\``.
- **Duplicate override** — `record-spread override \`x\` listed more
  than once`.
- **Source type mismatch** — `type mismatch in let annotation /
  expected: T / found: Q`. (Reuses the typer's existing nominal
  check; no new code path.)

The first three short-circuit before the typer runs; the fourth fires
during type-check on the synthetic `let` annotation.

### Editorial preference

- Use the spread when more than half of the new value's fields come
  from a single existing record. `Color { ...red, a: 128 }` and
  `Settings { ...defaults, theme: "dark" }` are the canonical shapes.
- For a brand-new value where every field is fresh, the named or
  positional form is clearer than `T { ...zero, ... }`.
- Don't use spread for "merging two records of the same type" — there
  is no second-spread form, and emulating it (`T { ...a, x: b.x, y:
  b.y, ... }`) defeats the purpose. If real demand surfaces we can
  add multi-spread later.

### What is **not** in v1

- **Override-then-spread** (`{ x: 10, ...p }`) — single direction
  simplifies the mental model. May revisit if real demand surfaces.
- **Multiple spreads** (`{ ...p, ...q, x: 10 }`) — composition
  semantics need their own design pass.
- **Pattern spread** (`P { ...rest }` to capture remaining fields) —
  separate proposal, out of scope.
- **Type inference for `T`** (`{ ...p, x: 10 }` with no leading type
  name) — separate enhancement; v1 follows the existing record-lit
  rule that always names the type.

### Fixtures

- `examples/records/spread_basic.kai` — single override, two fields
  copied from the source.
- `examples/records/spread_no_override.kai` — `P { ...p }` with zero
  overrides; structural copy.
- `examples/records/spread_all_overridden.kai` — every field
  overridden; observable result is identical to the bare named form.
- `examples/records/spread_type_mismatch.kai` — spreading a `Q` into a
  `P` literal; rejected with `type mismatch in let annotation`.
- `examples/records/spread_must_be_first.kai` — `{ x: 10, ...p }`;
  rejected at parse time.
- `examples/records/spread_pun_rejected.kai` — `{ ...p, x }`;
  rejected at parse time.

## 11. Pipe family — `|`, `||`, `|?`

In addition to the apply pipe `|>` (which threads its left operand
into the *first argument position* of a call on the right), full
kaikai (stage 1+) provides three single-token pipes that desugar
to a known operation on the head type's declaring module.

| Operator | Operation        | Desugar (head `[T]`)              | Issue |
|----------|------------------|-----------------------------------|-------|
| `\|`     | map pipe         | `list.map(xs, f)`                 | #201  |
| `\|\|`   | flat-map pipe    | `list.flat_map(xs, f)`            | #201  |
| `\|?`    | filter pipe      | `list.filter(xs, p)`              | #412  |

All three are left-associative and share a single-token form (no
whitespace between the bars or between `|` and `?`) so the parser
sees one symbol at the operator slot. They sit at the same
precedence as `|>` (level 8 in §*Precedence and associativity*),
so `xs |? even | (n) => n * 2` parses as
`(xs |? even) | ((n) => n * 2)`.

The right-hand side accepts every lambda surface that `|` accepts:
a bare name, a paren-form arrow (`(x) => body`), a block lambda
(`{ x -> body }`), or a `.field` / `.method` placeholder (#385 /
m7f §19) — `xs |? .active` desugars to
`xs |? ((__pl) => __pl.active)`.

Effects on the predicate are captured at the call site exactly as
for `|` and `||`: a `Console`-effecting predicate makes the whole
pipe expression carry `Console` in its inferred row.

Constraint #1 from #201 still holds: `Option` and `Result` heads
are rejected with a typed diagnostic pointing the user at `!` or
`option.and_then` / `result.and_then` for explicit chaining. The
pipes are list-shaped sugar, not a generic monadic chain.

### Fusion (#1134)

Adjacent `|` stages fuse into one traversal when every stage closure is
pure: `xs | f | g` lowers to `map(xs, x => g(f(x)))` — one result list,
no intermediates. A `|`-chain into a terminal `foldl` fuses further, to a
single accumulating loop that builds no list at all; and a range literal
at the head (`[a..b] | f |> foldl(...)`) becomes a counting loop that
never materialises the range. Fusion is invisible — it changes only time
and allocation, never a value.

Purity is the license: fusion reorders stage execution per element, which
is observable only if a stage has effects. The typer reads each stage's
inferred effect row; a stage whose row is non-empty (beyond a masked
`Mutable`) breaks the fused run at that point, so observable ordering is
preserved byte-for-byte. No annotation controls this — the row decides.

## 12. Multi-clause function bodies — `case`-led arms

Issue #415. A third form of `fn` body, alongside `= expr`
(single-expression) and `{ stmts }` (statement-block). The body block
is composed entirely of `case`-led arms; each arm pattern-matches
against the implicit tuple of arguments and may carry an optional
`when` guard.

```kai
fn classify(n: Int) : String {
  case 0          -> "zero"
  case n when n < 0 -> "neg"
  case _          -> "pos"
}

fn classify2(a: Int, b: Int) : String {
  case 0, 0 -> "both-zero"
  case 0, _ -> "first-zero"
  case _, 0 -> "second-zero"
  case _, _ -> "neither"
}
```

### Decision rule

When `parse_fn_decl` reaches a `{`, it peeks past the brace and any
newlines/semicolons. If the first significant token is `case`, the
block is parsed as multi-clause; every subsequent arm must also be
`case`-led until the closing `}`. Otherwise the body falls into the
existing statement-block parser. LL(1) with one token of lookahead.

### Pure-block rule

A body block is **either** a sequence of statements **or** a sequence
of `case` arms. Mixing is a hard parse error, regardless of order.
Once stmt-mode is committed (the first token was `let`/`var`/expr/etc.)
a stray `case` produces "case-block must contain only `case` arms".
Once case-mode is committed (the first token was `case`) a non-`case`
non-`}` token produces the same diagnostic. There is no warning-only
mode; the rule is enforced unconditionally.

If you need setup before discrimination, extract a helper or use the
single-expression body wrapping a `match`:

```kai
fn lookup(xs: [Entry], k: String) : Lookup =
  let cache = compute() {
    match (xs, k) { ... }
  }
```

### Desugar

For a 1-arg fn the scrutinee is the parameter directly:

```
fn f(a: T) : R { case p1 -> b1 ; case p2 -> b2 }
# desugars to
fn f(a: T) : R = match a { p1 -> b1 ; p2 -> b2 }
```

For an N-arg fn (2 ≤ N ≤ 4) the scrutinee is the implicit tuple of
parameters; surface kaikai has no tuple expression so the desugar
reuses the existing multi-arg `match a, b { p, q -> ... }` machinery
(`desugar_multi_match`) — each arm carries N comma-separated patterns,
mirroring issue #129. The N=4 cap matches `parse_match`; raising it
requires re-evaluating the nested-match blowup.

### Why `case` keyword

- `|` is already overloaded as a variant separator and as the map-pipe
  operator; a third overload would compound the disambiguation cost.
- A bare-pattern leader would force backtracking to disambiguate
  stmt-block from arm-block, breaking the LL(1) discipline.
- A `match { ... }` block-of-arms (no scrutinee) was viable but adds
  ceremony that does not pay off; the per-arm `case` reads more
  naturally.

### Why `{}` is mandatory

Every block in kaikai uses `{}`. Allowing case-arms without braces
would be a unique exception. The closing `}` also gives the parser a
precise anchor for the "expected `case` or `}`" diagnostic.

### Fixtures

- `examples/sugars/case_block_single_arg.kai` — 1-arg form with a
  `when` guard.
- `examples/sugars/case_block_multi_arg.kai` — 2-arg form, all literal
  patterns, four arms.
- `examples/sugars/case_block_with_guard.kai` — multi-arg `when` guard.
- `examples/sugars/case_block_recursive_list.kai` — recursive list
  traversal with list-spread patterns and a guard.
- `examples/sugars/case_block_non_exhaustive.kai` — exhaustiveness
  check fires through the desugared `match`.
- `examples/sugars/case_block_mixed_stmt.kai` — pure-block rule
  rejects `let` interleaved with `case`.

## Cross-sugar interaction

All five sugars compose without ambiguity because their grammar
slots are disjoint:

- a naked cap read is an ordinary `Ident` in `Primary` — the
  checker rewrites it from the resolved type, no new grammar.
- `a[i]` is a postfix of `Primary`.
- `cap := v` and `a[i] := v` are statement forms.
- `var x := init` is a statement form with a distinct keyword.
- Trailing lambda is a suffix of a call (also a `Primary`
  postfix slot, distinct from `[i]`); the second trailing
  lambda (§5) extends the same suffix slot with greedy
  same-line attachment.
- Lambda-block in expression position (§1 *Lambda-block in
  non-trailing positions*) is parsed as `Primary` when the
  position demands an expression and the same-line rule binds
  the `{` to its preceding operator or `(`.

Combining them:

```kai
var counter := 0
each(events) { e ->
  if counter < max {
    buffer[e.id] := e.payload
    counter := counter + 1
  }
}
Console.print("processed #{counter}")
```

Parsing this unambiguously walks:

- `var counter := 0` — statement, declares a `State[Int]` cell.
- `each(events) { e -> ... }` — call with trailing lambda.
  Inside the lambda, `counter` and `buffer` are in scope.
- `counter < max` — `counter` is an ordinary `Ident` the checker
  rewrites to a cap read, binary `<`.
- `buffer[e.id] := e.payload` — indexed write. `buffer` must
  be `Array[T]`; the checker picks `T` from `e.payload`.
- `counter := counter + 1` — capability write + capability
  read.
- `Console.print("processed #{counter}")` — interpolation
  containing `counter`, which is an ordinary expression
  inside `#{...}`.

Every branching decision (identifier → `:=`, `[`, `.`, or end
of statement) resolves with the standard LL(1) single-token
lookahead. No extra peek beyond that is required.

## Deliberately not on this list

Candidates discussed during Doc B review and rejected, with
reasons:

- **`r := v` for `Ref[T]`.** Would unify Ref and State writes
  under one sugar. Rejected because Ref is a value, not a
  capability binding, and the checker would then need to
  inspect the LHS's declared type to pick a desugar target,
  complicating diagnostics.

- **A naked read for `Ref[T]`.** A bare `r` does not read a
  `Ref[T]`; that stays the explicit `Mutable.ref_get(r)` form.
  Same reason as above — Ref is a value, not a capability
  binding.

- **`counter <- v` (Haskell-style) instead of `counter := v`.**
  `<-` reads as "pull value from computation" in Haskell and
  Elm contexts; kaikai's `:=` reads as "assign". The latter
  matches the semantics closer. Rejected.

- **`yield name = expr` for generators.** Generators are a
  separate effect (not in v1) and would need their own sugar
  design. Rejected as out of scope.

## Migration and diagnostics

- **Error messages show the source form.** A type error
  inside `counter := counter + 1` prints the expression as
  the user wrote it, with the caret at the sugar site. Where
  understanding the error depends on the desugar (e.g., the
  checker talks about `set(v: T)`'s parameter type and the
  user sees `:=`), the message attaches a `note: equivalent to
  counter.set(counter.get() + 1)` line. The compiler preserves
  the sugar span through desugar so this annotation is local
  and cheap. Modelled after Rust's handling of `?` and similar
  sugars.
- **The codemod does not introduce sugars.** `kai fmt
  --upgrade-effects` (m7a) rewrites bare builtins to explicit
  effect form (`print` → `Console.print`, etc.) but leaves
  `.get()` / `.set(v)` / `Mutable.array_get(...)` alone. A
  second codemod pass, m7b or later, can migrate to the
  sugars — kept separate so that (a) m7a can ship without
  waiting for sugars, and (b) users keep the choice of using
  sugars or not.
- **JSON diagnostics carry both forms.** The `--json` output
  (same contract as typed-holes, see `docs/typed-holes.md` and
  Tier 2 principle #4) serialises each diagnostic with the
  source expression *and* its desugared form as separate fields.
  The human text prefers the source; the LLM channel consumes
  the structured pair without needing to map between them. No
  friction on either side.
- **`kai fmt` is canonical, not optional.** Like `gofmt`, there
  is one formatting for each piece of code and no configuration
  knobs. Indentation rules for the sugars in this document
  (trailing-lambda body from the call's column, chained `|>` on
  separate lines, `var` at the top of its block, etc.) are
  encoded in `kai fmt`. CI is expected to run `kai fmt --check`
  and reject diffs. This keeps code consistent across projects
  and removes an entire class of style debates — important for
  both human reviewers and LLM authors who produce output that a
  canonical formatter can normalise before diffing.

## Out of scope for v1

- **User-definable sugars / macros.** Every sugar in this doc
  is built in. No facility for a library to declare its own
  prefix/infix operator or custom call shape.
- **`:=` as reassignment of `let` bindings.** `let` is
  immutable; `let x = 3; x := 5` is a type error. No plan to
  relax this.
- **Path-based LHSs on `:=`** (`counter.value := v` for
  nested updates on compound capability values). Adding paths
  to the LHS of `:=` would force the parser to track which
  field accesses are capability-rooted; the complexity does
  not fit v1. A later revision may reconsider once concrete
  use cases appear.

## Open questions

1. **Multi-line trailing lambda indentation.**
   *Decided:* the `{ ... }` body indents from the call's own
   column (standard indent, one step in), not from the closing
   `)`. Matches Kotlin, Swift, Prettier for JavaScript — the
   family the reader is most likely to have seen. `kai fmt`
   enforces this and carries the same gofmt-style discipline
   as the rest of kaikai's tooling (see §*Migration and
   diagnostics* and §*Next steps*).

2. **Naked-read precedence relative to postfix `.` and `[i]`.**
   *Dissolved:* with the read naked, `a.b` is an ordinary
   postfix projection on `a`. When `a` is a capability binding,
   the checker reads it first and then projects, so `a.b` reads
   "read the capability, then project" with no extra precedence
   rule. There is no read marker to bind, so the question that
   the `@` prefix once raised no longer applies.

3. **`var` declarations in the top-level of a file.**
   *Decided:* error at top level. The broader rule: kaikai has
   **no mutable state at module scope** — modules are for
   definitions (types, effects, functions, constants), and any
   mutable cell (`State[T]`, `Mutable` ref/array) lives inside
   a function. `var` at file top level emits a `handle` with
   nowhere to close; the checker rejects it with a message
   pointing at the `var` keyword and suggesting to put the cell
   inside `main` or whichever entry point needs it. The same
   rule forbids file-level `handle ... with State[T](init)
   { ... }` sites — there is no enclosing function body to
   be the handler's `body`.

## Next steps

- **Doc C (`docs/effects-impl.md`)** pins:
  - The desugar order in the pipeline (sugars run after
    parsing, before name resolution).
  - The variable-specialisation pass for `var` + canonical
    `State[T]` handlers.
  - The codemod spec for `kai fmt --upgrade-effects` and the
    (separate) sugars migration tool.
  - The formatter spec for `kai fmt`: canonical line breaks
    and spacing for each sugar, on top of the structural rules
    already pinned in this document.
- **Grammar update in `docs/kaikai-minimal.md`** — all five
  sugars modify the expression / statement grammar.
  `kaikai-minimal.md` carries the canonical EBNF; its
  §*Grammar* should be extended with the post-sugars productions
  introduced here so the LL(1) property is checkable from a
  single source.

Trailing lambdas (single and double), the lambda-block
expression form, `@` / `:=`, `var`, and `a[i]` all land together
in milestone **m7b**. None has a hard dependency on another, so
the order inside m7b is by convenience.
