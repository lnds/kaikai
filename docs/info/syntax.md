# syntax

One-page reference of the forms kaikai actually has. Every form on
this page is real. Forms NOT on this page do not exist in kaikai —
see the [NOT IN KAIKAI](#not-in-kaikai) section at the bottom.

## Description

kaikai is statically typed with HM extended by effect rows. Day-to-day
syntax stays close to Python/Elixir/JS; advanced surface (effects,
handlers, fibers, holes) is novel by design.

Comments start with `#`. Files have no module header — the path is
the package name (see `kai info packages`).

## Declarations

```kaikai
type Color = Red | Green | Blue                # sum type
type Point = { x: Int, y: Int }                # record type

effect Logger {                                # effect declaration
  log(s: String) : Unit
}

unit m                                         # unit of measure
unit s

fn add(a: Int, b: Int) : Int = a + b           # short body
fn add3(a, b, c: Int) : Int = a + b + c        # grouped: a, b, c all Int
fn double(n: Int) : Int { n * 2 }              # block body
fn greet(name: String) : Unit / Logger {       # with effect row
  Logger.log("hi #{name}")
}
pub fn area(w: Int, h: Int) : Int = w * h      # exported

fn fib(n: Int) : Int {                         # clause-block body:
  case 0 -> 0                                  #   `case` arms dispatch
  case 1 -> 1                                  #   on the parameter(s)
  case _ -> fib(n - 1) + fib(n - 2)
}
fn classify(a: Int, b: Int) : Int {            # multi-arg: comma-separated
  case 0, y          -> y                      #   patterns per arm,
  case x, y when x < 0 -> y                    #   `when` guard AFTER them
  case x, y          -> x + y                  #   (not `if` — that is the
}                                              #   plain-`match` guard kw)

const MAX : Int = 100                          # module constant

protocol Named {                               # single-dispatch protocol
  name(self: Int) : String
}
impl Named for Int {                           # protocol implementation
  fn name(self: Int) : String = "int"          #   `fn` REQUIRED on methods
}

axiom unchecked(x: Int) : Int                  # body-less audited escape

extern "C" fn cos(x: Real) : Real / Ffi        # FFI — Ffi in row REQUIRED

fn main() : Int = fib(10) + classify(0, 5) + MAX
```

The clause-block body (`{ case <pat> (when <guard>)? -> <body> ... }`)
desugars to a `match` over the parameter(s) — it is the kaikai form
for Elixir/Haskell-style multi-clause function heads. One parameter
matches it directly; 2–4 parameters match a comma-separated tuple per
arm. A clause-block holds ONLY `case` arms: mixing a `let`/statement
with `case` is a parse error.

## Imports and effect opening

```kaikai
import loop                                     # whole module
import loop as lp                               # aliased
import loop.{while}                             # selective

effect Clock {
  now() : Int
}
use Clock                                        # open ops for bare names

fn read() : Int / Clock = now()                  # `now`, not `Clock.now`

fn main() : Int = 0
```

`import` brings in a module (optionally aliased, or restricted to a
selection); `use E` opens an effect's operations so they resolve by
bare name. `use` takes an effect (PascalCase), never a module.

## Tests

```kaikai
test "addition" {                               # builtin test block
  assert 1 + 1 == 2                             # run by `kai test`
}

fn main() : Int = 0
```

`test "..." { }` (and the parallel `bench` / `check` forms) are
top-level declarations, not function calls. `assert cond` inside a
test body checks a condition; see `kai info testing`.

## Attributes

Three `#[...]` meta-instructions exist: `#[derive(...)]`,
`#[unstable]`, and `#[doc("...")]`. Attributes come BEFORE `pub`.

```kaikai
#[doc("Adds one to its argument.")]            # item doc, single line
pub fn addone(x: Int) : Int = x + 1

#[doc("""
Multi-line doc. The body is CommonMark.
""")]
pub fn double(x: Int) : Int = x * 2

#[derive(Eq, Show)]                            # structural impls
pub type Color = Red | Green | Blue

#[unstable]                                    # outside edition stability
pub fn experimental() : Int = 0

fn main() : Int = addone(double(20)) + experimental()
```

A `#[doc(...)]` at file top, separated from the first declaration by a
blank line, is the **module doc** — at most one per file. One doc per
item; `#[doc(hidden)]` / `alias` / `since` forms do not exist yet
(post-1.0). Doc text is extracted by `kai info builtins --json`,
`kaic2 --doc-json`, and rides typed-hole reports (`--holes-json`).

## Bindings

```kaikai
fn main() : Int / Stdout = {
  let x = 42                                   # immutable
  let y: Int = 100                             # with annotation

  var counter := 0                             # local mutable cell
                                               # (State[Int] handler)
  counter := counter + 1                       # write needs `:=`
  let result = counter                         # naked read
  Stdout.print("x=#{int_to_string(x)} y=#{int_to_string(y)} c=#{int_to_string(result)}")
  0
}
```

## Lambdas

```kaikai
fn run_with(f: (Int) -> Int, x: Int) : Int = f(x)

fn main() : Int = {
  let f1 = (x) => x + 1                        # unary arrow
  let f2 = (a, b) => a * b                     # multi-arg arrow
  let r1 = run_with(. + 10, 5)                 # placeholder `.`
                                               # only in arg position
  let r2 = f1(1) + f2(2, 3) + r1
  r2
}
```

## Trailing lambdas

```kaikai
fn each_of[a, e](xs: [a], f: (a) -> Unit / e) : Unit / e = match xs {
  []         -> ()
  [h, ...t]  -> { f(h); each_of(t, f) }
}

fn main() : Unit / Stdout = {
  each_of([1, 2, 3]) { x ->                    # block lambda
    Stdout.print("got #{int_to_string(x)}")
  }
}
```

## Control flow

```kaikai
import loop

fn classify(n: Int) : String = {
  if n == 0 {
    "zero"
  } else {
    match n {
      k if k > 0  -> "pos"
      _           -> "neg"
    }
  }
}

fn count_to(n: Int) : Int / Stdout = {
  var i := 0
  while { i < n } {
    Stdout.print("#{int_to_string(i)}")
    i := i + 1
  }
  i
}

fn main() : Int / Stdout = {
  Stdout.print(classify(0))
  Stdout.print(classify(5))
  let _ = count_to(3)
  0
}
```

`while` and `until` live in the `loop` stdlib package — `import loop`
at the top of the file makes them available.

There is no `for x in xs` loop. Iterate with the map-pipe (`xs | (x =>
...)`) or `xs |> each(f)` from the prelude.

## Pipes

Four pipes, each with one intent. See `kai info pipes` for the full
treatment.

```kaikai
fn double(x: Int) : Int = x * 2
fn gt1(x: Int) : Bool = x > 1
fn single(x: Int) : [Int] = [x, x]
fn sub(a: Int, b: Int) : Int = a - b

fn main() : Unit / Stdout = {
  let a = 5 |> double                          # |> apply
  let b = [1, 2, 3] | double                   # | map (head-type)
  let c = [1, 2] || single                     # || flat-map
  let d = [1, 2, 3] |? gt1                     # |? filter
  let e = 5 |> sub(7, _)                       # _ placeholder
  Stdout.print("a=#{int_to_string(a)} b len=#{int_to_string(b.length())} c len=#{int_to_string(c.length())} d len=#{int_to_string(d.length())} e=#{int_to_string(e)}")
}
```

## Point-free sections

A leading `.` in function position is a point-free section: it stands
for a one-argument lambda whose body is the rest of the chain applied
to the implicit argument. The chain may be a field, a field path, a
method call, or a method call with arguments.

```kaikai
type Addr = { city: String }
type Person = { name: String, addr: Addr }

fn main() : Unit / Stdout = {
  let people = [Person { name: "ana", addr: Addr { city: "Hanga Roa" } }]
  let names  = people | .name                    # (p) => p.name
  let cities = people | .addr.city               # (p) => p.addr.city
  let lens   = (people | .name) | .length()      # (s) => s.length()
  let init   = people | .name |? .starts_with("a")  # (s) => s.starts_with("a")
  let upper  = Some("hi").map(.length())         # UFCS arg position
  Stdout.print("ok")
}
```

The receiver is supplied implicitly (first argument by UFCS), so
`.starts_with("a")` reads its written argument after the receiver.
Point-free sections work as the function of `|`, `||`, `|?` and as a
combinator argument (`.map`, `.and_then`, `.filter`). A point-free
section is unary; to drop a positional argument, use the `|>`
placeholder `_` instead — the two do not mix in one section.

## Effects

```kaikai
effect Greeter {
  greet(s: String) : Unit
}

fn say(name: String) : Unit / Greeter = Greeter.greet(name)

fn main() : Int / Stdout = {
  let result = handle {
    say("world")
    42
  } with Greeter {
    greet(s, resume) -> { Stdout.print("hello #{s}"); resume(()) }
    return(x)        -> x
  }
  result
}
```

In a function type, the row appears after `/`: `: T / E1 + E2`.
Empty row means pure.

## Literals and operators

```kaikai
type Color = Red | Green | Blue

fn main() : Unit / Stdout = {
  let n = 42 + 0xFF + 0b1010                   # numbers
  let r = 3.14 + 2.5e-2                        # reals
  let c = 'A'                                  # chars (Unicode scalar
  let acc = 'á'                                #   value; 'á' '▸'
  let emo = '\u{1F389}'                        #   '\u{1F389}' all lex)
  let s = "hello, #{int_to_string(n)}"         # interpolation
  let xs = [1, 2, 3]                           # list
  let r1 = [1..10]                             # range
  let r2 = [1..10..2]                          # range w/ step
  let opt = Some(42)                           # variant
  let combined = [1, 2] ++ [3, 4]              # ++ concat (right-assoc)
  let all = variants[Color]()                  # enumerate ctors of a
                                               #   nullary sum type
  Stdout.print("n=#{int_to_string(n)} s=#{s} xs len=#{int_to_string(xs.length())} r1 len=#{int_to_string(r1.length())} r2 len=#{int_to_string(r2.length())} combined len=#{int_to_string(combined.length())} colors=#{int_to_string(all.length())}")
  match opt {
    Some(v) -> Stdout.print("v=#{int_to_string(v)}")
    None    -> Stdout.print("none")
  }
}
```

List destructuring (`[head, ...tail]`) only parses where a pattern
is expected — `match` arms, `let` LHS (`let [h, ...t] = xs`), fn
parameters. It is not an RHS expression form. See the
[Patterns section](#patterns-highlights-see-kai-info-match).

Operators by precedence (looser at the bottom):

```text
postfix       f(args)  a.b  a[i]  expr!  trailing-lambda
power         a ^ b                                  (right-assoc)
unary         -x  not x
multiplicative *  /  %
additive      +  -
concat        ++                                     (right-assoc)
comparison    ==  !=  <  >  <=  >=                   (non-associative)
logical and   and                                    (short-circuit)
logical or    or                                     (short-circuit)
pipes         |>  |  ||  |?                          (left-assoc)
```

`a < b < c` is a syntax error (non-associative). Use `a < b and b < c`.
Boolean operators are the keywords `and`, `or`, `not` — not `&&`,
`||`, `!`. (`||` is the flat-map pipe; `!` is postfix Option/Result
propagation.)

## Records

```kaikai
type Point = { x: Int, y: Int }

fn make_point(x: Int, y: Int) : Point = Point { x, y }       # punning

fn area_at_origin(p: Point) : Int = match p {
  { x: 0, y: 0 } -> 0                                        # match values
  { x, y }       -> x * y                                    # pattern punning
}

fn main() : Int / Stdout = {
  let p = Point { x: 3, y: 4 }                               # type prefix REQUIRED
                                                             # in expression position
  Stdout.print("p.x=#{int_to_string(p.x)}")
  area_at_origin(p)
}
```

## N-tuples

`(a, b)`, `(a, b, c)`, `(a, b, c, d)` desugar to `Pair`, `Triple`,
`Quad` records. Field access via `.fst`, `.snd`, `.trd`, `.frt`.

```kaikai
fn make_pair() : (Int, Int) = (10, 20)
fn make_triple() : (Int, Int, Int) = (1, 2, 3)

fn main() : Unit / Stdout = {
  let pr = make_pair()
  let tr = make_triple()
  Stdout.print("pair=#{int_to_string(pr.fst)},#{int_to_string(pr.snd)}")
  Stdout.print("triple=#{int_to_string(tr.fst)},#{int_to_string(tr.snd)},#{int_to_string(tr.trd)}")
}
```

`(e)` is grouping; never a 1-tuple. `()` is the unit value.

## Patterns (highlights — see `kai info match`)

```kaikai
type Point = { x: Int, y: Int }

fn first(xs: [Int]) : Option[Int] = match xs {
  []             -> None                        # empty list
  [x, ..._]      -> Some(x)                     # head + ignored rest
}

fn sign(n: Int) : String = match n {
  0  -> "zero"                                  # int literal
  -1 -> "minus-one"                             # negative literal
  k if k > 0 -> "pos"                           # `if` guard (plain match)
  _  -> "neg"
}

fn origin(p: Point) : Bool = match p {
  { x: 0, y: 0 } -> true                        # record pattern w/ values
  _              -> false
}

fn swap(pr: (Int, Int)) : (Int, Int) = match pr {
  (a, b) -> (b, a)                              # tuple pattern
}

fn list_info(xs: [Int]) : String = match xs {
  whole @ [_, ..._] -> "non-empty len=#{int_to_string(whole.length())}"
                                                # `whole` binds the
                                                # entire scrutinee
  []                -> "empty"
}

fn main() : Unit / Stdout = {
  match first([1, 2, 3]) {
    Some(n) -> Stdout.print("first=#{int_to_string(n)}")
    None    -> Stdout.print("empty")
  }
  Stdout.print(sign(-1))
  Stdout.print(list_info([1, 2]))
  let _ = origin(Point { x: 0, y: 0 })
  let _ = swap((1, 2))
}
```

Literal patterns (`0`, `-1`, `"hi"`, `'c'`, `true`), record patterns
(`{ x: 0, y }`), and tuple patterns (`(a, b)`) all appear in `match`
arms, `let` LHS, and fn parameters. The guard keyword in a plain
`match` arm is `if`; in a clause-block fn body it is `when`.

## Postfix `!` — Option/Result propagation

```kaikai
fn lookup_a() : Option[Int] = Some(3)
fn lookup_b() : Option[Int] = Some(4)

fn add_them() : Option[Int] = {
  let a = lookup_a()!                          # unwrap or early-return None
  let b = lookup_b()!
  Some(a + b)
}

fn main() : Unit / Stdout = match add_them() {
  Some(n) -> Stdout.print("sum=#{int_to_string(n)}")
  None    -> Stdout.print("none")
}
```

The dispatch (Option vs Result) is by the type the typer assigns to
the expression; the enclosing function's return type must match.

## UFCS

`receiver.fn(args)` desugars to `fn(receiver, args)`, dispatched by
receiver type. Chains left-associative.

```kaikai
fn main() : Unit / Stdout = {
  let r : Result[Int, Int] = Ok(5)
  let b = r.map((x) => x + 1).map_err((e) => e * 10).is_ok()
  let xs = [1, 2, 3]
  let n = xs.length()
  let label = if b { "yes" } else { "no" }
  Stdout.print("ok=#{label} n=#{int_to_string(n)}")
}
```

## Units of measure

```kaikai
unit m
unit s

fn area(w: Real<m>, h: Real<m>) : Real<m^2> = w * h
fn area_of[u: Measure](w: Real<u>, h: Real<u>) : Real<u^2> = w * h

fn main() : Unit / Stdout = {
  let g = 9.81<m/s^2>                          # literal with unit
  let t = 2.0<s>
  let v = g * t                                # inferred Real<m/s>
  let a1 = area(3.0<m>, 4.0<m>)
  let a2 = area_of(3.0<s>, 4.0<s>)
  Stdout.print("ok")
}
```

`^` is the power operator. The exponent MUST be `Int` (literal or
non-literal); the base may be `Int`, `Real`, or `Real<u>`. The result
keeps the base type, with the unit lifted to `u^n` when the base is
dimensioned (in that case `n` must be an `Int` literal — there are no
dependent units). For `Int` base, negative exponents truncate to `0`;
for `Real` base, negative exponents compute `1.0 / base^|e|`. There
is no `Real ^ Real` operator; for non-integer exponents use a stdlib
helper (e.g. `Numeric.pow_int` for the `Int`-exponent case in
`stdlib/math/numeric.kai`).

Inside `<...>` (unit expressions) `^` also denotes power, but the
exponent there is a unit-level integer literal — see `kai info units`.

## Capability sugar

Capability read / write — naked read and `cap := v`:

```kaikai
fn main() : Int / Stdout = {
  var tally := 0
  tally := tally + 1                           # `:=` writes
  tally := tally + 1                           # `tally` reads naked
  let v = tally
  Stdout.print("tally=#{int_to_string(v)}")
  v
}
```

`:=` is the single mark of mutability: `var x := init` declares the
cell, `x := v` writes it, and a bare `x` reads it. A naked cell read
flows through string interpolation (`"#{tally}"`) like any other name.

Array indexing — `a[i]` reads, `a[i] := v` writes:

```kaikai
fn main() : Unit / Stdout = {
  let arr = array_make(3, 0)                   # local array
  arr[0] := 42                                 # array index write
  let n = arr[0]                               # array index read
  Stdout.print("arr[0]=#{int_to_string(n)}")
}
```

## Typed holes

```kaikai
fn maybe(s: String) : Int = {
  let n = ?                                    # anonymous hole
  let m = ?fallback                            # named hole
  n + m
}

fn main() : Int = 0
```

The checker reports each hole with expected type and bindings in
scope under `kai build --holes file.kai`. JSON via `--holes-json`.

## Regions

`region { ... }` is an expression-block. List/record/variant
constructors written **lexically inside** the block are bump-allocated
in a per-region arena that frees in one shot at the closing brace,
skipping reference counting entirely. Opt-in — you write `region`; the
compiler never infers it.

```kaikai
fn sum_list(xs: [Int]) : Int = match xs {
  []        -> 0
  [h, ...t] -> h + sum_list(t)
}

fn main() : Unit / Stdout = {
  let total = region {                          # bump-arena scope
    let a = [1, 2, 3, 4, 5]                      # built in the arena
    let b = [10, 20, 30]                         # built in the arena
    sum_list(a) + sum_list(b)                    # scalar result
  }                                              # whole arena freed here
  Stdout.print("#{int_to_string(total)}")
}
```

The block's value is wrapped in a deep-copy-out: a **scalar passes
through free** (nothing crosses the arena boundary), but a **pointer
that escapes** the brace is deep-copied onto the RC heap before the
arena dies. That copy is the cost — so regions pay off only when the
result is a scalar (or nothing escapes).

Two limits to know:

- **Escaping data is a net loss.** If the block returns a list/record
  that outlives it, the deep-copy-out makes `region` *slower* than the
  plain RC heap. The niche is escape-free scratch.
- **Only lexically-inside constructors are arena-routed.** A
  `region { build(...) }` where `build` is a helper allocates nothing in
  the arena — the helper's constructors run under the normal heap. To
  benefit, write the throwaway allocation inline in the block.

The sweet spot: a hot loop that builds throwaway aggregates inline and
folds them to a scalar. See `examples/perceus/region_scratch.kai` and
the benchmark in `docs/benchmarks/region-120-2026-05-31.md`.

## NOT IN KAIKAI

These look plausible but DO NOT EXIST. Do not write them.

```kaikai-neg
fn main() : Int = (\x -> x + 1)(5)            # Haskell lambda
```

```kaikai-neg
fn main() : Int = (fn x -> x + 1)(5)          # ML lambda
```

```kaikai-neg
fn main() : [Int] = [1, 2, 3] | (+ 1)         # operator section
```

```kaikai-neg
fn main() : [Int] = [x * 2 for x in [1, 2, 3]]  # Python list comp
```

```kaikai-neg
fn main() : Int = do { let x = 1; let y = 2; x + y }  # Haskell do
```

```kaikai-neg
fn main() : Int = x + y where { let x = 1; let y = 2 }  # Haskell where
```

```kaikai-neg
type Foo<T> = T                                # angle generics
                                               # (use [T] / Foo[T])
```

```kaikai-neg
/// Rust-style doc comment                     # no /// or //! docs
pub fn documented() : Int = 1                  # (use #[doc("...")])
```

```kaikai-neg
fn fib(0) : Int = 0                            # no literal-in-signature
fn fib(n: Int) : Int = fib(n - 1)              #   multi-clause; use a
                                               #   clause-block `{ case … }`
```

```kaikai-neg
fn classify(n: Int) when n > 0 : String = "p"  # no `when` guard in the
                                               #   signature; guard lives
                                               #   in a `case` arm
```

```kaikai-neg
fn classify(n: Int) : String {
  case n if n < 0 -> "neg"                      # clause-block guard is
  case _          -> "pos"                      #   `when`, not `if`
}
```

```kaikai-neg
fn pick() : Int = null                         # no null
```

```kaikai-neg
fn risky() : Int = throw "boom"                # no throw
```

```kaikai-neg
fn main() : Int = {
  for x in [1, 2, 3] { print(x) }              # no for-in
  0
}
```

```kaikai-neg
fn main() : Int = {
  return 42                                    # no return statement
}
```

Use instead:

- `fn f(0) = ...` / `fn f(n) when ... = ...` (Elixir multi-clause head) → a clause-block `fn f(n) { case 0 -> ...; case _ -> ... }`
- `case n if cond ->` (clause-block guard) → `case n when cond ->` (`if` is the plain-`match` guard)
- `\x -> body` / `fn x -> body` → `(x) => body`
- `(+ 1)` → `(x => x + 1)`
- `[x*2 for x in xs]` → `xs | (x => x * 2)`
- `do { ... }` → block body
- `where x = ...` → `let` inside block
- angle generics `Foo<T>` → bracket generics `Foo[T]` (angles are reserved for `Real<m>`)
- `/// doc` / `//! doc` / `#: doc` → `#[doc("...")]` above the declaration
- `null` / `undefined` / `nil` → `Option[T]`
- `throw` / `try-catch` → `Fail` effect or `Result[e, a]` (with `expr!`)
- `async` / `await` → `Spawn` effect (see `kai info fibers`)
- `interface I { ... }` → `protocol P { ... }`
- `for x in xs { ... }` → `xs | (x => ...)` or `xs |> each(f)`
- `return expr` → the last expression of a block IS the value
- `self.x` / `this.x` → no implicit receiver; methods take explicit params

## See also

`kai info effects`, `kai info fibers`, `kai info match`,
`kai info pipes`, `kai info protocols`, `kai info units`,
`kai info packages`, `kai info testing`, `kai info holes`
