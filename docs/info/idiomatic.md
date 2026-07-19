# idiomatic

How to write kaikai the way kaikai wants to be written — the idioms,
and the false-friend reaches that compile but read wrong.

## Description

You can read `kai info syntax` and still write non-idiomatic kaikai:
code that type-checks but reaches for a shape from Haskell, Python,
JS, or Elixir when kaikai has a cleaner native form. This page pairs
each idiom with the common wrong reach. Every positive block here
compiles and runs against the live compiler; blocks marked
`kaikai-neg` are deliberately wrong and shown only to be recognised.

The rule under all of these: when in doubt, write both versions and
let the compiler decide. It is the cheapest correctness check you
have.

## if as statement vs expression

An `if` whose body is `Unit` is a *statement*: no `else` is needed.
Use it for side effects.

```kaikai
fn main() : Unit / Stdout = {
  let n = 7
  if n > 5 {
    Stdout.print("big")
  }
}
```

The redundant `else { () }` still compiles, but it is noise — drop it:

```kaikai
fn main() : Unit / Stdout = {
  let n = 7
  if n > 5 {
    Stdout.print("big")
  } else {
    ()
  }
}
```

An `if` used as a *value* is an expression: both branches are
mandatory and must share a type.

```kaikai
fn label(n: Int) : String = if n > 0 { "pos" } else { "non-pos" }

fn main() : Unit / Stdout = Stdout.print(label(3))
```

Dropping the `else` from a value-position `if` is a type error — the
missing branch is `Unit`, which does not match the other branch's
type. This does NOT compile:

```kaikai-neg
fn label(n: Int) : String = if n > 0 { "pos" }   # missing else
```

## Pipes over nested calls and intermediate lets

The four pipes (`|>` apply, `|` map, `||` flat-map, `|?` filter —
see `kai info pipes`) read top-to-bottom in data-flow order. Reach
for a pipeline before a stack of `let`s or nested calls.

```kaikai
fn even(x: Int) : Bool = x % 2 == 0
fn dbl(x: Int) : Int = x * 2

fn main() : Unit / Stdout = {
  let total = [1..10] |? even | dbl |> sum
  Stdout.print("total=#{int_to_string(total)}")
}
```

The same computation spelled with intermediate bindings and nested
calls compiles, but inverts the reading order and names three
throwaway values:

```kaikai
fn even(x: Int) : Bool = x % 2 == 0
fn dbl(x: Int) : Int = x * 2

fn main() : Unit / Stdout = {
  let evens = filter([1..10], even)
  let doubled = map(evens, dbl)
  let total = sum(doubled)
  Stdout.print("total=#{int_to_string(total)}")
}
```

Use the intermediate-`let` form only when a step needs a name a
reader benefits from, or the same value is consumed twice.

## Point-free sections over throwaway lambdas

When a pipe or combinator step is just "reach into the element", drop
the lambda and write the point-free section — a leading `.` followed
by the field, path, or method (see `kai info syntax`). It reads as the
projection itself, with no bound name to invent.

```kaikai
type Person = { name: String, addr: Addr }
type Addr = { city: String }

fn main() : Unit / Stdout = {
  let people = [Person { name: "ana", addr: Addr { city: "Hanga Roa" } }]
  let names  = people | .name                    # not (c) => c.name
  let cities = people | .addr.city               # not (c) => c.addr.city
  let lens   = names | .length()                 # not (s) => s.length()
  let first  = Some("hi").map(.length())         # UFCS arg position
  Stdout.print("ok")
}
```

The explicit lambda is only worth its name when the body does more
than project — a computation, multiple uses of the parameter, or a
positional `_` placeholder (which a point-free section does not take).

## Units of measure over bare Real

When a quantity has a dimension, annotate it. The unit rides as a
phantom type at zero runtime cost, and the typer catches
dimensional mistakes (adding metres to seconds, returning the wrong
unit) before they run. See `kai info units`.

```kaikai
unit m
unit s

fn speed(d: Real<m>, t: Real<s>) : Real<m/s> = d / t

fn main() : Unit / Stdout = {
  let v = speed(100.0<m>, 9.58<s>)
  Stdout.print("ok")
}
```

Writing the same function over bare `Real` compiles and is correct
arithmetic, but throws away the checking — nothing stops a caller
passing a duration where a distance belongs.

## Effects: infer locally, annotate publicly

A function's effect row is inferred inside local bodies and MUST be
annotated on public signatures. Handle effects lexically with
`handle ... with`; do not reach for escapes. A `pub fn` that performs
a user effect must expose that effect as `pub` too.

```kaikai
pub effect Audit {
  record(s: String) : Unit
}

pub fn process(name: String) : Unit / Audit = Audit.record("did #{name}")

fn main() : Int / Stdout = {
  handle {
    process("alpha")
    0
  } with Audit {
    record(s, resume) -> { Stdout.print(s); resume(()) }
  }
}
```

See `kai info effects`. The discipline is the safety guarantee: the
row type on a public function is a complete, honest list of what it
can do.

## The tuple-ish forms kaikai actually has

Two forms, each with a clear use:

- **Positional n-tuples** — `(a, b)`, `(a, b, c)`, `(a, b, c, d)`.
  They desugar to `Pair` / `Triple` / `Quad` records; fields are
  `.fst`, `.snd`, `.trd`, `.frt`. Use them for a small ad-hoc grouping
  with no good field names (a min/max pair, a key/value).
- **Records** — `type Point = { x: Int, y: Int }`. Use a named record
  when the fields deserve names or the shape recurs. The type prefix
  (`Point { ... }`) is required in expression position.

```kaikai
type Point = { x: Int, y: Int }

fn minmax(xs: [Int]) : (Int, Int) = (1, 9)

fn main() : Unit / Stdout = {
  let mm = minmax([3, 1, 9])
  let p = Point { x: 2, y: 5 }
  Stdout.print("lo=#{int_to_string(mm.fst)} hi=#{int_to_string(mm.snd)} px=#{int_to_string(p.x)}")
}
```

`(e)` is grouping, never a one-tuple; `()` is the unit value. There
are no anonymous record literals — a record is always introduced by
its type name.

## let-pattern binding

`let` takes a pattern on its left, so destructure at the binding site
instead of projecting fields one at a time.

```kaikai
fn bounds() : (Int, Int) = (2, 8)

fn main() : Unit / Stdout = {
  let (lo, hi) = bounds()
  Stdout.print("lo=#{int_to_string(lo)} hi=#{int_to_string(hi)}")
}
```

## match over nested if

When you are discriminating a sum type or several shapes, `match`
states the cases flat. A ladder of nested `if`/`else` over the same
scrutinee is the non-idiomatic reach.

```kaikai
type Light = Red | Yellow | Green

fn act(l: Light) : String = match l {
  Red    -> "stop"
  Yellow -> "slow"
  Green  -> "go"
}

fn main() : Unit / Stdout = Stdout.print(act(Green))
```

`match` is also exhaustiveness-checked: a missing variant is a
compile error, whereas a missing `else` branch silently does nothing.
See `kai info match`.

## clause-block vs match-in-body

When the WHOLE point of a function is to dispatch on its parameters,
write a clause-block — `case` arms in lieu of a body. It reads like
an Elixir/Haskell multi-clause head and drops the `= match arg { ... }`
wrapper.

```kaikai
type Shape = Circle(Int) | Square(Int)

fn area(s: Shape) : Int {
  case Circle(r) -> 3 * r * r
  case Square(w) -> w * w
}

fn main() : Unit / Stdout = Stdout.print(int_to_string(area(Circle(2))))
```

Reach for `match`-in-body instead when you discriminate something
that is NOT directly a parameter — a derived value, a field, an
intermediate result. There is no parameter to put after `case`, so a
`match` over the sub-expression is the honest form.

```kaikai
fn parity(n: Int) : String = match n % 2 {     # match a derived value,
  0 -> "even"                                   #   not a bare argument
  _ -> "odd"
}

fn main() : Unit / Stdout = Stdout.print(parity(7))
```

The criterion: clause-block when the arms ARE the function and dispatch
straight on the arguments; `match`-in-body when you first compute or
project the thing you discriminate. The guard keyword differs too —
`when` inside a clause-block, `if` inside a plain `match` arm. These do
NOT compile (clause-block guard is `when`; signatures take no literal
patterns or guards):

```kaikai-neg
fn classify(n: Int) : String {
  case n if n < 0 -> "neg"                       # clause-block guard is
  case _          -> "pos"                       #   `when`, not `if`
}
```

```kaikai-neg
fn fib(0) : Int = 0                             # no literal-in-signature
fn fib(n: Int) : Int = fib(n - 1)               #   multi-clause head
```

## Option over a sentinel

Absence is `Option[T]`, not a magic value like `-1`, an empty string,
or null (kaikai has no null). Return `None`; the caller `match`es or
propagates with postfix `!`.

```kaikai
fn find_pos(xs: [Int], target: Int) : Option[Int] = match xs {
  []        -> None
  [h, ...t] -> if h == target { Some(h) } else { find_pos(t, target) }
}

fn main() : Unit / Stdout = match find_pos([3, 7, 9], 7) {
  Some(v) -> Stdout.print("found #{int_to_string(v)}")
  None    -> Stdout.print("absent")
}
```

## Short body vs block body

Use the `=` short body when a function is a single expression; use a
`{ }` block when it needs intermediate `let`s or statements; use a
clause-block `{ case … }` when the body dispatches straight on the
parameters (see above). Each has a clear intent — pick the one that
reads cleaner, not a fourth invented one.

```kaikai
fn square(x: Int) : Int = x * x

fn describe(x: Int) : String = {
  let s = square(x)
  "#{int_to_string(x)}^2 = #{int_to_string(s)}"
}

fn main() : Unit / Stdout = Stdout.print(describe(4))
```

## while/until vs recursion vs a pipe

kaikai has mandatory TCO, so tail recursion is a fine loop. But for a
counted side-effecting loop, `while` (from the `loop` package) reads
plainly; and for *transforming* a collection, a map-pipe beats both a
manual loop and hand-rolled recursion. There is no `for x in xs`.

```kaikai
import loop

fn main() : Int / Stdout = {
  var i := 0
  while { i < 3 } {
    Stdout.print("tick #{int_to_string(i)}")
    i := i + 1
  }
  i
}
```

To map over `xs`, write `xs | (x => ...)`, not a `while` that indexes.

## #[doc] on every pub symbol

Public API documentation is the `#[doc("...")]` attribute above the
declaration, with a one-sentence first line. It is required on `pub`
symbols and is extracted into `kai info builtins --json` and
typed-hole reports.

```kaikai
#[doc("Returns the smaller of two integers.")]
pub fn imin(a: Int, b: Int) : Int = if a < b { a } else { b }

fn main() : Unit / Stdout = Stdout.print("#{int_to_string(imin(3, 8))}")
```

Rust-style `///` / `//!` doc comments do not exist — `#[doc]` is the
only form.

## False friends — you might write X, in kaikai write Z

These are the reaches an agent makes from another language. Each
LEFT form does NOT exist in kaikai; write the RIGHT form. The
authoritative list is the NOT-IN-KAIKAI section of `kai info syntax`.

- `fn f(0) = ...` / `fn f(n) when c = ...` (Elixir multi-clause head)
  → a clause-block `fn f(n) { case 0 -> ...; case _ -> ... }`
- `\x -> body` (Haskell) / `fn x -> body` (ML) → `(x) => body`
- `(+ 1)` operator section → `(x => x + 1)`
- `[x * 2 for x in xs]` list comprehension → `xs | (x => x * 2)`
- `do { ... }` (Haskell) → a `{ }` block body; the row type carries
  the effect discipline
- `x where y = ...` (Haskell) → a `let` inside a block
- `Foo<T>` angle generics → `Foo[T]` (angles are reserved for units,
  `Real<m>`)
- `null` / `nil` / `undefined` → `Option[T]`
- `throw` / `try` / `catch` → the `Fail` effect or `Result[a, e]`
  with postfix `!`
- `async` / `await` → the `Spawn` effect (see `kai info fibers`)
- `for x in xs { ... }` → `xs | (x => ...)` or `xs |> each(f)`
- `return expr` → the last expression of a block IS its value
- `self.x` / `this.x` → no implicit receiver; pass the value explicitly

## See also

`kai info syntax`, `kai info pipes`, `kai info effects`,
`kai info units`, `kai info match`, `kai info holes`, `kai info llm`
