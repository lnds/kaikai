# effects in kaikai

Algebraic effects are kaikai's first-class mechanism for IO,
cancellation, non-determinism, actor messages, and every other
"something other than a pure function from values to values". The
goal is that **every effect a function can cause appears in its
type**, and the call site cannot invoke it without a handler being
lexically in scope.

This document pins two things: the **representation of effect
rows** (the math) and the **surface syntax** (effect
declarations, function types with effects, `handle` / `resume`,
and the call-site dispatch convention). Migration of the current
kaikai codebase to effect types, and the implementation strategy
(CPS transform, runtime, interaction with monomorphisation) live
in `docs/effects-stdlib.md` and `docs/effects-impl.md` — not yet
written.

Scope of v1: everything needed to parse, type-check, and desugar
a program that declares effects, calls their operations, and
installs handlers. Runtime CPS and stdlib migration come after.

## Context

kaikai commits to Effekt-style algebraic effects with inference
(`docs/design.md` "Decisions"). Koka and Effekt are the two
production-quality prior art:

- **Koka** — row-polymorphic effects in HM-extended types.
  Function types carry a row of effect labels (Koka writes
  `int -> <io|exn> int`). Unification is Rémy-style. One-shot
  continuations are the default.
- **Effekt** — also row-polymorphic, but treats effect
  operations as **capability passing**: a handler binds a
  capability value that methods dispatch through. Effects are
  lexically scoped by construction.

kaikai picks Effekt's capability discipline (makes handlers feel
like ordinary values and composes with structured concurrency —
see `docs/structured-concurrency.md`) over Koka's syntax (which
hides the capability). Underneath, both are row-polymorphic HM
with the same unification.

## Effect rows

### Representation

An **effect row** is a finite set of effect labels, optionally
with a tail variable for polymorphism.

```
ρ ::= ∅                # empty row (pure)
    | { L | ρ }        # extend: label L on top of row ρ
    | α                # row variable (only in polymorphic types)
```

At runtime the row has no representation: it is a compile-time
type-level object. Two rows are equivalent up to **reordering and
removal of duplicates** — `{Io, Fail}` ≡ `{Fail, Io}` ≡
`{Io, Fail, Io}`. Labels are a set, not an ordered list, and
insertion is idempotent.

*Why a set, not a multiset?* A second `Io` in the row never
changes what the program can do: the handler for `Io` already
catches every `Io.print`. Multiset rows add machinery for effect
shadowing that kaikai does not need — lexical nesting of `handle`
blocks gives the same expressive power.

*Why a tail variable?* For functions that are *transparent* to
effects — `map`, `filter`, `await` — the row must be polymorphic
in whatever the argument contributes.

### Unification

Row unification is standard Rémy-style. To solve
`{L₁, …, Lₙ | ρ} ≡ {M₁, …, Mₘ | σ}`:

1. Partition the labels into *both*, *only-left*, *only-right*.
2. If both tail variables are concrete (rigid): require the
   partition's difference sets to be empty; otherwise fail with a
   "row mismatch" diagnostic.
3. Otherwise, introduce a fresh row variable `τ` and bind:
   - `ρ := { only-right | τ }`
   - `σ := { only-left  | τ }`

   Each bind runs an occurs check — a row variable cannot appear
   as the tail of the row it is being bound to — same discipline
   as ordinary type-variable binding in HM.
4. Recursively unify any label payloads (if labels carry type
   arguments — rare, see `State[T]` below).

The algorithm is decidable, linear in the row size, and runs
inside the existing HM inference loop. No fancy lattice, no
subtyping.

#### Worked example

Unify `{Io, Fail | ρ}` with `{Io, State | σ}`, both tails free:

1. Partition: `both = {Io}`, `only_left = {Fail}`,
   `only_right = {State}`.
2. Both tails are free row variables, so skip the rigid case.
3. Fresh `τ`; bind `ρ := {State | τ}` and `σ := {Fail | τ}`.
4. No payloads.

After substitution both sides read `{Io, Fail, State | τ}` (up to
reordering) — unification succeeds.

### Pure, open, closed

Three important row shapes, all spellable by the user:

- **Pure** (`∅`): no effects. A function with a pure row can be
  called from any context; it adds nothing to the caller's row.
- **Closed** (no tail variable): the row is exactly these
  effects, no more. Typical for public signatures that promise
  nothing beyond their declared effects: `Int / Io`.
- **Open** (has a tail variable): the row contains these
  effects *at least*. Typical for higher-order functions that
  are agnostic to their callback's effects: `[B] / Io + e`.

The compiler infers the most *specific* row possible for private
bodies: closed whenever it can, open only when a row variable
remains free (i.e., a generic argument's row escapes). Public
signatures must be written by the user — the compiler checks the
inferred row is a subset of the declared closed row, or unifies
with its tail variable if open.

### Subsumption: none

A function of type `Int / Io` cannot be passed where a pure
`Int` is expected. Adding an effect is *not* an implicit
operation: calling an `Io` function from a pure context is a type
error. The only way to "remove" an effect is to `handle` it,
which is a syntactic operation.

Rationale: subsumption interacts badly with row inference (you
need either row-constraint propagation or explicit coercions),
and the resulting errors are hard to explain. Keep the model
concrete-per-call: if you want polymorphism, use a row variable.

## Syntax

### Function types with effects

```
T                        # pure (no effect suffix)
T / Io                   # single effect (singleton row)
T / Io + Fail            # two-element closed row
T / e                    # row variable
T / Io + e               # row extension: Io plus whatever e is
T / Io + Fail + e        # more of the same
```

`+` is the row-extension operator and associates left. It is
syntactic; the compiler normalises to a canonical form (labels
sorted alphabetically, tail variable last). `e + Io` and
`Io + e` both print as `Io + e` in diagnostics.

**Capitalisation disambiguates labels from variables.** Effect
names are declared with a TitleCase identifier (like types).
Lowercase identifiers in the row position are row variables
bound by the enclosing generic scope.

```kai
# Concrete row
fn parse(s: String) : Int / Io + Fail

# Polymorphic row
fn map[A, B, e](xs: [A], f: (A) -> B / e) : [B] / e
```

Absence of `/` means pure:

```kai
fn add(a: Int, b: Int) : Int
```

Not `Int / {}`. Kaikai has one form; redundant braces do not
compile.

### Declaring an effect

```kai
effect Io {
  print(s: String) : Unit
  read_line() : String
}

effect Fail {
  fail(msg: String) : Nothing
}

effect State[T] {
  get() : T
  set(v: T) : Unit
}
```

Operations are declared without the `fn` keyword and without a
body — just the signature. "Paying the effect" is implicit at
the call site.

`Nothing` is kaikai's empty type (uninhabited; no value can be
constructed). An operation returning `Nothing` cannot resume —
this is how `Fail` encodes abort at the type level. `Nothing` is
not yet formalised in `docs/kaikai-minimal.md`; adding it is a
prerequisite for this doc's examples to type-check (TODO).

Effects can be parameterised (`State[T]`). The parameter is
monomorphic per handler instance: one `handle ... with State[Int]`
installs an `Int`-valued state cell.

### Calling an operation

Inside a scope where the effect capability is available, call the
operation as a **method on the effect type**:

```kai
fn greet(name: String) : Unit / Io {
  Io.print("hello, #{name}")
}
```

`Io.print(...)` desugars to a call through the currently active
`Io` capability. If no handler for `Io` is in lexical scope, the
checker rejects the program with `effect not handled: Io`.

The effect name **is the capability** by default. This is
different from Koka (operations are first-class symbols,
dispatch is implicit) and lines up with Effekt (capabilities are
values). In kaikai the capability is named after the effect,
which gives you a readable call site without an extra binder.

If you want to name the capability differently — useful when two
nested handlers of the same effect are in scope, or for concision
— use `as`:

```kai
handle {
  log.print("start")
  compute()
  log.print("end")
} with Io as log {
  print(s, resume) -> { ...perform the write...; resume(()) }
}
```

`log` is an ordinary identifier now; inside the body, `Io` is
**not** in scope. Calling `Io.print(s)` there would be a type
error: the rebind replaces the default capability name, it does
not shadow-and-hide it.

### Handling

```kai
handle { body } with Effect {
  op_1(args..., resume) -> expr
  op_2(args..., resume) -> expr
  return(x)             -> expr          # optional
}
```

- `body` is the scope where `Effect` is handled.
- Each op clause receives the operation's arguments and a
  `resume` value, which is a callable that continues `body` with
  a value of the operation's return type (see
  `resume: one-shot, explicit` below).
- `return(x)` is an optional clause that runs on `body`'s normal
  return path. If absent, the return value of `handle` is the
  return value of `body`.
- Outside the `with` block, `Effect` is no longer in the row.

Example — supplying a value via an effect:

```kai
effect Ask {
  name() : String
}

fn main() : Unit / Io {
  let greeting = handle {
    "hello, #{Ask.name()}"
  } with Ask {
    name(resume) -> resume("world")
    return(s)    -> s
  }
  Io.print(greeting)
}
```

`Ask.name()` suspends; the handler's `name` clause supplies
`"world"` via `resume`; the body continues and builds the
greeting; `return(s) -> s` is the identity transform, shown here
for completeness (could be omitted with the same semantics).
`Ask` is handled *inside* the `handle` block, so the outer `main`
does not carry it in its row — only `Io` from the final `print`.

### `resume`: one-shot, explicit

`resume` is a value of type `(T) -> S / ρ` where:

- `T` is the operation's declared return type.
- `S` is the type of the `handle` expression (the type `body`
  returns, possibly transformed by `return`).
- `ρ` is the effect row of `body` **without** this handler's
  effect (plus whatever the handler's own body causes).

In a parameterised handler (see *State: a handler with its own
state* below), `resume` has the extended signature
`(T, H) -> S / ρ`, where `H` is the handler's state type: the
second argument replaces the current state. The plain `resume(v)`
form is shorthand for `resume(v, state)` — state unchanged.

`resume` is **one-shot by default**: calling it twice, or letting
it escape the op clause and be called after the clause returns,
is an error. The compiler detects it statically when both calls
lie on the same control-flow path; otherwise the runtime catches
it on the second call. This is the zero-cost continuation regime;
kaikai's principle #2 mandates it.

For genuinely multi-shot or escaping continuations (backtracking
search, generators), the programmer writes `resume_multishot(v)`
— a second builtin that pays the copy cost. `resume_multishot`
exists so the common case is fast and the uncommon case is
explicit. v1 of effects ships only one-shot; multi-shot is noted
for `docs/effects-impl.md`.

### Discarding the continuation

An op clause may simply not call `resume`. When that happens, the
value the clause evaluates to becomes the value of the entire
`handle`; the rest of `body` is abandoned, and the `return`
clause (if any) is bypassed — `return` only runs on `body`'s
normal completion path.

This is how `Fail` works:

```kai
effect Fail {
  fail(msg: String) : Nothing
}

fn safe_div(a: Int, b: Int) : Int / Fail {
  if b == 0 { Fail.fail("division by zero") }
  else      { a / b }
}

fn with_default(f: () -> Int / Fail) : Int {
  handle { f() } with Fail {
    fail(msg, resume) -> -1
    return(x)         -> x
  }
}
```

The `fail` clause returns `-1`; `with_default` returns `-1`.
Because the operation's declared return type is `Nothing`, there
is no valid value to pass to `resume` — the type system prevents
accidental resumption. `resume` is in scope in the clause for
uniformity (every clause binds it) but remains uncallable for
`Nothing`-returning ops.

### Desugaring sketch

Not part of the spec (belongs to `docs/effects-impl.md`), but
worth noting so the syntax is not mystery:

- `handle { body } with Eff { ... }` compiles `body` so each
  `Eff.op` call becomes a suspension point, and installs the
  handler record on a runtime handler stack.
- `Eff.op(args)` compiles to a call into the runtime that walks
  the stack to find the innermost handler for `Eff`, invokes the
  op clause with a reified continuation, and either resumes
  (normal path) or returns directly (discard path).
- `Eff as X { ... }` binds `X` as a capability value inside the
  body; `X.op(args)` and `Eff.op(args)` desugar identically — the
  only difference is which identifier the checker accepts.
- One-shot `resume` is a direct tail call. Multi-shot requires
  copying the fiber stack frames, hence the opt-in.

Interaction with monomorphisation, fibers, and the module system
is covered in the implementation doc.

## Worked examples

### A pure `map` polymorphic in effects

```kai
fn map[A, B, e](xs: [A], f: (A) -> B / e) : [B] / e {
  match xs {
    []           -> []
    [h, ...rest] -> [f(h), ...map(rest, f)]
  }
}

# Use sites:
map([1, 2, 3], (x) => x + 1)                     # [B]         (pure, e = {})
map(["a", "b"], (s) => { Io.print(s); s })        # [B] / Io
map(lines, parse_int)                             # [B] / Fail  (parse_int : (String) -> Int / Fail)
```

The body of `map` never calls an effect operation itself, but the
call to `f(h)` pays whatever `f` pays. The effect row `e` flows
out transparently.

### State: a handler with its own state

Not every handler is stateless. `State[T]` threads a value through
the body without exposing mutation to the user. Declaration
repeated here for convenience:

```kai
effect State[T] {
  get() : T
  set(v: T) : Unit
}
```

A handler for `State[T]` takes an initial value and binds a
`state` identifier inside its clauses. `resume` gains an optional
second argument: `resume(value, new_state)` resumes the body with
`value` *and* updates the handler's state. Plain `resume(value)`
leaves state unchanged.

```kai
fn sum(xs: [Int]) : Int {
  handle {
    each(xs, (x) => State.set(State.get() + x))
    State.get()
  } with State[Int](0) {
    get(resume)    -> resume(state)           # return state, unchanged
    set(v, resume) -> resume((), v)           # return (), new state = v
    return(x)      -> x                        # keep body result, drop state
  }
}

# sum([1, 2, 3]) == 6
```

- `with State[Int](0)` installs the handler with initial state 0.
- `state` in each clause is the handler's current value; it
  refreshes whenever a clause calls `resume(_, new_state)`.
- `return(x) -> x` here discards the final state; swap for
  `return(x) -> (x, state)` to pair the body result with the
  final state.

This is the standard "parameterised handler" shape from the
algebraic-effects literature. It generalises to `Reader[T]`,
`Writer[W]`, exception handlers that collect context, etc. —
covered effect-by-effect in `docs/effects-stdlib.md`.

### Composed handlers

```kai
fn example() : Unit / Io {
  let result = handle {                                     # outer
    handle {                                                # inner
      State.set(10)
      let x = State.get()
      if x > 5 { Fail.fail("too big") } else { x }
    } with State[Int](0) {                                  # inner handler
      get(resume)      -> resume(state)
      set(v, resume)   -> resume((), v)
      return(x)        -> (x, state)
    }
  } with Fail {                                             # outer handler
    fail(msg, resume) -> (-1, 0)
    return(pair)      -> pair
  }
  Io.print("final = #{result}")
}
```

Handlers are installed in order: inner handler runs first,
catches its effect, and the outer sees the body post-handling.
`State` is no longer in the row past the inner `with`; `Fail` is
no longer in the row past the outer. `Io` is only in the
remaining row (paid by the final `print`).

## Inference

- Operator types are known from `effect` declarations: looking
  up `Eff.op` yields its declared arrow type plus the
  single-element row `{Eff}` added to the caller's row.
- Function bodies: each operation call adds its effect to the
  body's inferred row. `handle ... with E { ... }` **removes**
  `E` from the row of `body`. The final row of the handler
  expression is `(row(body) \ {E}) ∪ row(handler clauses)` — in
  prose, *remove `E` from `body`'s row, then add any effects the
  handler clauses themselves use*.
- At a top-level declaration with a declared signature, the
  inferred row must equal the declared row, up to row-variable
  unification. If the declared row is open (`/ e`), `e` unifies
  with whatever the body produced.
- Row variables in scope are generalised over at declaration
  boundaries, not inside expressions — same rule as ordinary
  type generalisation. Example: `fn map[A, B, e](...)` is
  generalised as `forall A, B, e. ...`, with `e` a row variable.

### Interaction with existing HM

The inferencer already runs HM over types with `TyVarT` and a
`Subst`. Row variables are added as a parallel notion:

- New type constructor `TyRow(labels: [String], tail:
  Option[RowVarId])`. Parallel to `TyVarT`.
- New substitution slot for row variables.
- `apply_ty` / `unify` extended with a row case.

Effect rows do **not** appear inside ordinary types — they only
appear in the effect position of function types (`TyFnT` gains a
third slot for the effect row).

## Out of scope for v1

- **Subtyping or row coercion.** Adding an effect is an explicit
  annotation, not an implicit conversion.
- **Higher-rank row polymorphism** (a function value whose type
  quantifies over a row variable in its body). Complicates
  inference without buying much for the use cases at hand.
- **Effect operations with generic type arguments at the use
  site.** `State[T]` parameterises per-handler, not per-call.
- **Multi-shot `resume` by default.** Always opt-in via
  `resume_multishot`.
- **Ambient effects.** Every effect must be explicitly handled.
  No "default Io handler" baked into the language itself — the
  stdlib provides one, installed by the runtime when `main` has
  `Io` in its row.
- **Exceptions as a separate feature.** `Fail` (or a sibling
  `Error[E]`) subsumes try/catch idioms. There is no `try`
  keyword.
- **`raise` / `throw` / `catch` keywords.** The only surface
  verb is `handle`; operation calls happen through plain
  method-call syntax (e.g., `Io.print(s)`).
- **Named handlers as first-class values.** A handler always
  appears at a `handle ... with ...` site; you cannot bind a
  handler to a variable, pass it as an argument, or return it.
  Doable as a later extension, not needed for v1.
- **Multiple instances of the same effect** differentiated by
  label (e.g., two independent `State[Int]` handlers distinguished
  in scope). The innermost handler for an effect always wins; if
  you need two cells, declare two effects.

## Open questions

Each question's body explains the dilemma; a leading label marks
whether there is a tentative leaning or the question is genuinely
undecided. Tentatives become decisions in Doc B or Doc C when
they first need to be acted on.

1. **Syntax for calling a rebound capability.** Does `log` (from
   `as log`) need special marking, or is an ordinary method call
   good enough?
   *Decided:* ordinary method call; the checker knows `log` is
   bound to the `Io` capability via `as`, so `log.op(...)` is
   unambiguous.

2. **`return` clause sugar.** If absent, is the return type of
   `handle` the same as `body`?
   *Tentative:* yes. Is a short form needed for trivial
   transformers? Probably not — `return(x) -> x` is short.

3. **Ordering of `with`-clauses.** Must all ops appear?
   *Tentative:* yes — exhaustive, analogue of pattern-match
   exhaustiveness. Any omitted op is a compile error ("handler
   does not cover Eff.op2").

4. **What does `main` look like?** `fn main() : Unit / Io` with
   the runtime installing the Io handler implicitly, or
   `fn main() : Unit` (pure) with the user writing
   `handle { ... } with Io { ... }` at the top? The first is
   ergonomic; the second is principled.
   *Decided:* the first. Ergonomy wins; the stdlib provides the
   Io handler and the runtime installs it for `main` functions
   whose row includes `Io`.

5. **Do we allow effect aliases?** `type Net = Http + Dns`.
   Useful for stdlib readability; adds a resolution pass.
   *Open.* Defer to Doc B.

6. **Interaction with modules.** Can a user-declared effect be
   re-exported across modules? Is an operation addable after the
   fact (open extension), or does `effect Foo` close its
   signature?
   *Tentative:* closed signature, matching sum types. Keeps
   inference predictable.

## Next steps

- **Doc B** — `docs/effects-stdlib.md`: concrete stdlib effects
  (`Io`, `Fail`, `State[T]`, `Mutable`, `Cancel`, `Spawn`),
  migration of the existing `print`/`read_file`/`array_*`
  builtins, Io default-handler implementation, interaction with
  the `bin/kai run` entry.
- **Doc C** — `docs/effects-impl.md`: CPS transform in the
  stage-2 pipeline, changes to `TyFnT`, handler-stack runtime
  representation, interaction with monomorphisation and fibers
  (m8), diagnostic quality for row-mismatch errors.

v1 of the implementation lands in milestones m7.1–m7.N, each a
separate sub-design that references back here.
