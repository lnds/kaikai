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
2. **Capability read/write** — `@cap` and `cap := v` as short
   forms for capability ops on `State[T]` and `Reader[T]`.
3. **Local mutable cells** — `var x = init` declares an
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
- `@cap` / `cap := v` — Doc B §*Syntax note: capability
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
- three binding shapes: `cap := v`, `a[i] := v`, `var x = v`.
- one prefix form: `@cap`.
- one postfix form: `a[i]`.
- two call-site shapes: trailing lambda, and a second
  trailing-lambda slot for two-block control-flow helpers.
- the block-form `{ x -> body }` accepted as a standalone
  lambda expression wherever a `Primary` is legal.

Two filters justify accepting this surface:

1. **Each sugar has one intent the long form obscures.** `@cap`
   is a capability read, not any other kind of expression;
   `cap := v` is a capability write on `State`; `var` is a
   local cell; `a[i]` is array indexing. Doc A's "few forms,
   each with clear intent" standard.
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

## 2. Capability read/write: `@cap` and `cap := v`

### Rules

Four restrictions, lifted verbatim from Doc B:

1. `@cap` applies **only to `State[T].get()` and
   `Reader[T].ask()`**.
2. `cap := v` applies **only to `State[T].set(v)`**.
3. The identifier after `@` must be **a simple capability
   binding** (`@counter`, not `@config.section.level`).
4. `@` and `:=` require **an `as`-bound capability name**.
   Using the default capability name (`State` itself) keeps
   the explicit `State.get()` / `State.set(v)` form.

   Reason for rule 4: without `as`, the default capability's
   identifier is also the effect's name (`State`). Allowing
   `@State` would give two forms for the same op with no intent
   difference, and `State := v` would read as an assignment to
   the effect rather than to a cell. Requiring an `as`-bound
   name sidesteps both: the sugar applies only to identifiers
   the user has introduced as bindings, and the default form
   stays as an explicit method call.

### Grammar delta

```
Primary     ::= ...
              | "@" Ident                        # capability read
Stmt        ::= ...
              | Ident ":=" Expr                  # capability / array write
```

Both are resolved by the checker, which verifies the
identifier's type. `StateCap[T]` and `ReaderCap[T]` below are
the internal types the compiler assigns to `as`-bound
capabilities of the corresponding effects — they are checker
implementation details, not names users spell in source code.

- `@x` legal iff `x : StateCap[T]` (for any `T`) or
  `x : ReaderCap[T]`.
- `x := v` legal iff `x : StateCap[T]` and `v : T`, **or** the
  array-indexing form below.

If `x` does not have a capability type, the error points at
the sugar site and suggests either the method call
(`x.get()` / `x.set(v)`) or the explicit long form
(`State.get()` / `State.set(v)`).

### Why `@` specifically

`@` is unused elsewhere in kaikai's expression grammar. It is
short enough for `@counter + 1` to read cleanly, visually
distinct from a plain identifier (so the capability read is
still *visible* per Doc A §*Context*'s capability-explicit
stance), and does not collide with type-parameter brackets or
the method call `.`.

The only other proposed use of `@` is as-patterns
(`name@pattern` in `match`), documented in
`docs/proposed-extensions.md` §14. That usage is **infix** in
patterns; `@cap` here is **prefix** in expressions. Contexts
are disjoint; the same symbol plays two roles decided by
grammar position.

## 3. Local mutable cells: `var`

### Rule

```kai
var name: T = init       # with type annotation
var name = init          # with inference
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
              | "var" Ident (":" Type)? "=" Expr
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

### Effect-row impact

Because the implicit `handle` closes `State[T]` in the same
block, the enclosing function's effect row **does not** gain
`State[T]`. A `var counter = 0` inside a function of signature
`: Int / Console` leaves the signature untouched.

`var` encapsulates `State[T]` and nothing else. Any other
effect the body uses — `Console.print(@counter)`,
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
while      { @i > 0 } { i := @i - 1; io.println("#{@i}") }
until      { @done }  { process_one() }
if_then_else(p)       { run_a() } { run_b() }
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

## Cross-sugar interaction

All five sugars compose without ambiguity because their grammar
slots are disjoint:

- `@cap` is a prefix of `Primary`.
- `a[i]` is a postfix of `Primary`.
- `cap := v` and `a[i] := v` are statement forms.
- `var x = init` is a statement form with a distinct keyword.
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
var counter = 0
each(events) { e ->
  if @counter < max {
    buffer[e.id] := e.payload
    counter := @counter + 1
  }
}
Console.print("processed #{@counter}")
```

Parsing this unambiguously walks:

- `var counter = 0` — statement, declares a `State[Int]` cell.
- `each(events) { e -> ... }` — call with trailing lambda.
  Inside the lambda, `counter` and `buffer` are in scope.
- `@counter < max` — prefix `@` on `counter`, binary `<`.
- `buffer[e.id] := e.payload` — indexed write. `buffer` must
  be `Array[T]`; the checker picks `T` from `e.payload`.
- `counter := @counter + 1` — capability write + capability
  read.
- `Console.print("processed #{@counter}")` — interpolation
  containing `@counter`, which is an ordinary expression
  inside `#{...}`.

Every branching decision (identifier → `:=`, `[`, `.`, or end
of statement) resolves with the standard LL(1) single-token
lookahead. No extra peek beyond that is required.

## Deliberately not on this list

Candidates discussed during Doc B review and rejected, with
reasons:

- **`cap.x` implicit read (Koka-style), dropping the `@`.**
  Writing `counter` where `counter.get()` is meant would
  make capability reads invisible at the call site. Doc A
  §*Context* picked Effekt's capability-explicit design over
  Koka's sugar precisely to avoid this. Rejected.

- **`r := v` for `Ref[T]`.** Would unify Ref and State writes
  under one sugar. Rejected because Ref is a value, not a
  capability binding, and the checker would then need to
  inspect the LHS's declared type to pick a desugar target,
  complicating diagnostics.

- **`@r` for `Ref[T]` reads.** Same reason as above — Ref is
  not a capability.

- **`counter <- v` (Haskell-style) instead of `counter := v`.**
  `<-` reads as "pull value from computation" in Haskell and
  Elm contexts; kaikai's `:=` reads as "assign". The latter
  matches the semantics closer. Rejected.

- **`yield name = expr` for generators.** Generators are a
  separate effect (not in v1) and would need their own sugar
  design. Rejected as out of scope.

## Migration and diagnostics

- **Error messages show the source form.** A type error
  inside `counter := @counter + 1` prints the expression as
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

2. **`@` precedence relative to postfix `.` and `[i]`.**
   *Decided:* `@a.b` = `(@a).b`. `@` binds tight — same slot
   as unary `-`, which is why `-a.b` is `(-a).b` in every
   language the reader knows. The intuition is "read the
   capability, then project." The pretty-printer adds explicit
   parens when the nesting is non-obvious.

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

## Landed sugars (post-m12.8)

The following sugars landed after the m7b set and after
m12.8.x — they are documented here because they ride on the same
desugar-before-typecheck principle as the original five.

### Regex sigil (`~r/.../`)

**Landed**: 2026-05-03, PR #159 (closes #85).

A regex literal is written `~r/PATTERN/`. The `~r/` prefix is a
two-character sigil recognised by the lexer; it produces a
`TkRegex(String)` token carrying the raw pattern body. The
closing `/` ends the literal. The body is single-line, ASCII
subset, and does not support backreferences or lookaround (the
constraints of `stdlib/regexp.kai`, the RE2-style engine).

```kai
let email_re : Regex = ~r/^[^@]+@[^@]+\.[^@]+$/
let stripped = re_replace(s, ~r/\s+/, " ")
```

**Why a sigil rather than bare `/.../`**: kaikai's lexer is
LL(1)-friendly and the `/` character already serves as the
division operator. A bare `/.../` literal would force the lexer
to disambiguate by context (Ruby/JavaScript-style lookback),
which the design refuses (see Tier 1 §3 — fast compilation,
single-pass parse). The `~r/` sigil is unambiguous regardless of
context.

**Desugar in expression position**: `~r/PAT/` parses to a
`Regex` value. It is interchangeable with any other expression
producing a `Regex`.

**Desugar in refinement-predicate position**: inside a `where`
clause the expression `matches ~r/PAT/` is a refinement-pure
call to the prelude `matches : (String, Regex) -> Bool` (see
`docs/refinements-and-contracts.md` §"Predicate language" and
§"Regex predicates"). The clause type-checks identically to any
other refinement-pure predicate.

Cross-link: `docs/stdlib-regex-design.md` for the engine; PR #159
for the sigil + token + `matches` integration.

### Hex and binary integer literals

**Landed**: 2026-05-03, PR #160 (closes #156).

Integer literals may be written in hexadecimal (`0x` prefix) or
binary (`0b` prefix) in addition to decimal. The literal type is
`Int` and the AST node is `EInt` — no new type, no new AST node,
no new operator. The parser branches on the second character
after a leading `0`.

```kai
let mask : Int = 0xFF
let cafe : Int = 0xCAFE
let max_signed_64 : Int = 0x7FFFFFFFFFFFFFFF
let bits : Int = 0b11111111
```

Range: signed 64-bit (`Int` is `i64` throughout the runtime).
There is no support for:

- Underscore digit separators (`0x_FF_FF` does not parse).
- Octal `0o` literals.
- Hex floating-point literals (`0x1.8p3` does not parse).

Cross-link: PR #160. Lexer change is in the integer-literal
branch of the tokeniser.

### n-tuple sugar (`(a, b)`, `(a, b, c)`, `(a, b, c, d)`)

**Landed**: 2026-05-03, PR #155 (closes #154).

A parenthesised comma-separated expression of length 2, 3, or 4
is sugar for the stdlib records `Pair[a, b]`, `Triple[a, b, c]`,
and `Quad[a, b, c, d]` respectively. The cap is **N = 4**;
`(a, b, c, d, e)` is a parse error. Field accessors stay named
(`.fst`, `.snd`, `.trd`, `.frt`); there is no positional
`.0`/`.1`. The `(a)` form is a parenthesised expression as
before — the n-tuple sugar starts at N = 2.

The sugar applies in all four positions:

- **Expression**: `(1, "x")` → `Pair[Int, String] { fst: 1, snd: "x" }`.
- **Type**: `(Int, String)` → `Pair[Int, String]`.
- **Pattern**: `match v { (a, b) -> ... }` → `Pair { fst: a, snd: b }`.
- **Let-destructure**: `let (a, b) = call_returning_pair()`.

The existing record-literal form `Pair { fst: x, snd: y }`
continues to parse identically to the sugar.

**Why the cap at N = 4**: matches the receipt of the
*Tuples — REJECTED* gate in `docs/proposed-extensions.md`. The
sugar adds a short construction/type/pattern form without
introducing a new `TyCtor` or a new field-access surface — both
of which the gate ruled out and continues to rule out.

Cross-link: `stdlib/core/tuple.kai` for the `Pair`, `Triple`,
`Quad` definitions; PR #155 for the parser desugar;
`docs/proposed-extensions.md` §"Tuples (sugar) — LANDED" for the
relationship with the older "Tuples — REJECTED" decision.
