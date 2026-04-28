# kaikai — getting started

A 30-minute tour of kaikai. By the end you will have built the
compiler from C source, run a handful of programs, and seen the
five things that make kaikai different from Python or JavaScript:
**effects in types**, **protocols + derive**, **units of measure**,
**refinements**, and **fibers that actually suspend**.

This is a tutorial, not a reference. The full design lives under
`docs/`; the [final section](#11-where-to-next) points you there.

## Sections

1. [Install kaikai from C](#1-install-kaikai-from-c)
2. [Your first program](#2-your-first-program)
3. [The `kai` driver](#3-the-kai-driver)
4. [Day-to-day kaikai](#4-day-to-day-kaikai)
5. [Effects](#5-effects)
6. [Protocols and `#derive`](#6-protocols-and-derive)
7. [Units of measure](#7-units-of-measure)
8. [Refinements and contracts](#8-refinements-and-contracts)
9. [Fibers and structured concurrency](#9-fibers-and-structured-concurrency)
10. [Testing](#10-testing)
11. [Where to next](#11-where-to-next)

Every section ends with a runnable command pointing at a file under
`examples/tutorial/`. Run them as you read.

## 1. Install kaikai from C

kaikai is a 3-stage compiler. Stage 0 is a tiny C bootstrap that
compiles a kaikai subset; stage 1 is written in that subset and
compiles full kaikai; stage 2 is written in full kaikai and is the
production compiler. Any machine with a C compiler can build all
three.

```sh
git clone https://github.com/lnds/kaikai
cd kaikai
make all
```

`make all` builds stage 0, then stage 1, then stage 2, then leaves
you with a `bin/kai` driver wrapper. Zero dependencies beyond `cc`
and `make`. No Rust toolchain. No package manager. No JVM.

If you'd rather see the chain by hand:

```sh
cc stage0/*.c -o stage0/kaic0
./stage0/kaic0 stage1/compiler.kai > /tmp/stage1.c
cc /tmp/stage1.c -I stage0 -o stage1/kaic1
./stage1/kaic1 stage2/compiler.kai > /tmp/stage2.c
cc /tmp/stage2.c -I stage0 -o stage2/kaic2
```

You're done.

## 2. Your first program

`examples/tutorial/01_hello.kai`:

```kai
fn main() = {
  print("hello, kaikai")
}
```

Run it:

```sh
bin/kai run examples/tutorial/01_hello.kai
# hello, kaikai
```

`print` is a flat builtin. `main` declares no return type and no
effect row; the typer infers both. The runtime detects that
`main`'s inferred row contains `Stdout` and installs a default
handler around it.

Here is the same program, with the effect made visible
(`examples/tutorial/02_hello_effects.kai`):

```kai
fn main() : Unit / Stdout = {
  Stdout.print("hello, kaikai")
}
```

The runtime behavior is identical. What changes:

- `: Unit / Stdout` declares the return type and the effect row.
- `Stdout.print(s)` calls `print` as the `Stdout` op.

In kaikai, every effect a function uses appears in its row, and the
compiler refuses to call an effectful function from a context that
does not handle the effect. `main` is the one place where the row
may stay implicit; in the rest of your code, public signatures
should declare what they touch.

That's the central design choice. Every other section is downstream
of it.

## 3. The `kai` driver

`bin/kai` is a shell wrapper around `kaic2 + cc + your binary`. Three
subcommands cover day-to-day work:

```sh
bin/kai run    file.kai           # compile and execute
bin/kai build  file.kai -o out    # compile to a native binary
bin/kai test   file.kai           # compile in test mode and run
```

The driver also auto-builds stages 0 / 1 / 2 the first time you call
it on a fresh checkout, and prepends every `stdlib/core/*.kai`
prelude plus `stdlib/protocols.kai` and the m14 v1 qualified-call
modules to every compilation.

```sh
bin/kai --version    # kaikai 0.4.1 (stage 2, self-hosted)
bin/kai help         # subcommand summary
```

`KAI_NO_STDLIB=1 bin/kai run ...` turns the auto-prelude off if you
want a bare compilation (useful when reading compiler error
messages).

## 4. Day-to-day kaikai

The first day's surface stays close to Python / JavaScript / Elixir.
Skim this section, run the examples, and you're set for sequential
code.

### Records, sum types, match

Records are immutable structures with named fields. Sum types are
tagged unions where each variant either is nullary or carries one
payload value. `match` is exhaustive — the compiler refuses to
build if a case is missing.

`examples/tutorial/03_records_match.kai`:

```kai
#derive(Show)
type Rect = { w: Real, h: Real }

#derive(Show)
type Tri = { base: Real, height: Real }

#derive(Show)
type Shape
  = Circle(Real)
  | Rectangle(Rect)
  | Triangle(Tri)

fn area(s: Shape) : Real = match s {
  Circle(r)    -> 3.14159 * r * r
  Rectangle(r) -> r.w * r.h
  Triangle(t)  -> 0.5 * t.base * t.height
}
```

To carry several fields on a variant, declare a record type and put
the record in the variant. Inline records inside a variant payload
are not currently supported.

```sh
bin/kai run examples/tutorial/03_records_match.kai
```

### Lists and pipes

`map`, `filter`, `each`, `reduce` are flat builtins (no module
prefix). The qualified-call surface from `stdlib/core/list.kai`
gives every other helper a `list.` prefix: `list.head`,
`list.take`, `list.sum`, `list.contains`, ...

Two pipe forms:

```
x |> f       # apply-pipe: f(x). Reads left-to-right.
xs | f       # map-pipe:   map(xs, f). One element at a time.
```

`examples/tutorial/04_lists_pipes.kai`:

```kai
fn main() : Unit / Stdout = {
  let nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
  let evens   = filter(nums, (x) => x % 2 == 0)
  let squared = map(evens, (x) => x * x)
  each(squared, (x) => print("#{x}"))

  # Same pipeline, pipe form.
  let piped = nums |> filter((x) => x % 2 == 0) | (x) => x * x
  each(piped, (x) => print("#{x}"))
}
```

```sh
bin/kai run examples/tutorial/04_lists_pipes.kai
```

### Option, Result, the postfix `!`

There is no `null`. Absence is `Option[T] = Some(T) | None`.
Errors are `Result[T, E] = Ok(T) | Err(E)`.

The postfix `!` short-circuits inside a function that returns
`Option` or `Result`: on `Some(x)` / `Ok(x)` it unwraps to `x`; on
`None` / `Err(e)` it returns from the enclosing function with the
same `None` / `Err(e)`. Use it to avoid match pyramids.

`examples/tutorial/05_option_result.kai`:

```kai
fn name_for(s: String) : Option[String] = {
  let id = parse_id(s)!
  let n  = lookup_name(id)!
  Some("user##{id}: #{n}")
}
```

If either `parse_id` or `lookup_name` returns `None`, `name_for`
returns `None` immediately — no nested match.

```sh
bin/kai run examples/tutorial/05_option_result.kai
```

That's the sequential surface. Records, sums, `match`, pipes,
`Option` / `Result`, `!`. About a day to internalize. The rest of
the tutorial is the novel surface that earns its keep.

## 5. Effects

This is the differentiator. An effect declaration introduces a
capability and a set of operations:

```kai
effect Logger {
  log(level: String, msg: String) : Unit
}
```

Three pieces, in order:

1. **Producer**. Any function that calls `Logger.log` must declare
   `/ Logger` in its row. The typer enforces this. There is no way
   to log without declaring the capability.

2. **Consumer**. `handle { body } with Logger { ... }` removes
   `Logger` from `body`'s row by interpreting every `log` op call.
   Each clause receives the op's arguments plus a `resume` thunk
   that continues the producer past the op call.

3. **Choice of handler**. The same producer code runs against any
   handler. Print to stdout, drop to silence, prefix with a tag,
   write to a file — none of that touches the producer.

`examples/tutorial/06_custom_effect.kai`:

```kai
effect Logger {
  log(level: String, msg: String) : Unit
}

fn process(ev: Event) : Unit / Logger = match ev {
  Login(u)  -> Logger.log("INFO",  "user login: #{u}")
  Logout(u) -> Logger.log("INFO",  "user logout: #{u}")
  Error(m)  -> Logger.log("ERROR", m)
}

fn main() : Unit / Stdout = {
  let events = [Login("alice"), Logout("alice"), Error("disk full")]
  handle {
    each(events, process)
  } with Logger {
    log(level, msg, resume) -> {
      print("[#{level}] #{msg}")
      resume(())
    }
  }
}
```

`use Logger` (file-level or block-level) lets you call `log(...)`
without the `Logger.` prefix while keeping the row intact.

```sh
bin/kai run examples/tutorial/06_custom_effect.kai
```

Effects in the stdlib catalog: `Stdout`, `Stderr`, `Stdin`, `File`,
`Env`, `Random`, `Clock`, `Spawn`, `Cancel`, `Fail`, `State[T]`,
`Reader[T]`, `Writer[W]`. The first six get a default handler
installed by the runtime when they appear in `main`'s row. The
others must be handled explicitly.

## 6. Protocols and `#derive`

A protocol is a set of operations a type can implement —
single-dispatch, table-lookup style (Go / Clojure / Elixir), not
type-class resolution. The stdlib ships four: `Show`, `Eq`,
`Hash`, `Ord`.

`#derive(Show, Eq, Hash, Ord)` on a record or sum type generates
the obvious impls. After that, `show(x)`, `eq(a, b)`, `hash(x)`,
`cmp(a, b)` dispatch through the type's table; the `#{x}`
interpolation form calls `show` on `x`.

`examples/tutorial/07_protocols.kai`:

```kai
#derive(Show, Eq, Hash, Ord)
type Point = { x: Int, y: Int }

#derive(Show, Eq, Ord)
type Priority = Low | Medium | High
```

```sh
bin/kai run examples/tutorial/07_protocols.kai
```

Two practical notes:

- `#{...}` interpolation re-parses the spanned text and does not
  resolve protocol-op calls inside the braces. Bind the result to a
  `let` first: `let n = hash(p1); print("#{n}")`.
- Passing a protocol op as a function value (e.g. as the comparator
  to `list.sort_by`) is not currently supported. Wrap it in a
  lambda: `list.sort_by(xs, (a, b) => cmp(a, b))`.

Records are ordered field-by-field in declaration order; sum types
are ordered variant-by-variant in declaration order, then by
payload.

## 7. Units of measure

Declare a unit symbol with `unit X`. Annotate values with
`<unit-expr>`: `1.0<USD>`, `9.81<m / sec^2>`. The typer treats
units as a separate algebra: same-unit addition is fine,
mixed-unit addition is a compile error, multiplication composes
the unit expression.

`examples/tutorial/08_uom.kai`:

```kai
unit USD
unit EUR

fn convert_usd_to_eur(amount: Real<USD>, rate: Real<EUR / USD>) : Real<EUR> =
  amount * rate

fn main() : Unit / Stdout = {
  let salary    : Real<USD> = 2500.0<USD>
  let groceries : Real<USD> = 320.5<USD>
  let remaining = salary - groceries     # OK: same unit
  let in_eur = convert_usd_to_eur(remaining, 0.92<EUR / USD>)
  print("remaining: #{remaining}")
  print("in EUR: #{in_eur}")

  # let bad = salary + 50.0<EUR>
  # ^ compile error: cannot add Real<USD> and Real<EUR>
}
```

Runtime values that come from outside (user input, file IO) start
dimensionless. Multiply by a dimensioned factor to attach a unit:
`amount * 1.0<USD>`.

```sh
bin/kai run examples/tutorial/08_uom.kai
```

## 8. Refinements and contracts

A refinement narrows a base type to values satisfying a predicate.
Two forms:

```kai
fn factorial(n: Int where n >= 0) : Int = ...    # inline on the parameter

type Port = Int where MIN_PORT <= self and self <= MAX_PORT
```

The leading-comparator form `where >= 0` desugars to
`where self >= 0`. Inline parameters check the predicate at the
call site; named refinements show up in match-arm patterns and as
documentation.

Function contracts:

- `requires <pred>` — precondition; checked on entry.
- `ensures  <pred>` — postcondition; checked on return. The keyword
  `result` binds the return value inside the predicate.

`examples/tutorial/09_refinements.kai`:

```kai
const MAX_PORT : Int = 65535
const MIN_PORT : Int = 1

type Port = Int where MIN_PORT <= self and self <= MAX_PORT

fn factorial(n: Int where n >= 0) : Int
  ensures result >= 1
  = if n == 0 { 1 } else { n * factorial(n - 1) }

fn main() : Unit / Stdout = {
  let f5 = factorial(5)
  print("factorial(5) = #{f5}")

  let classify = (n: Int) => match n {
    p : Port -> "port"
    _        -> "out-of-range"
  }
  print(classify(80))
  print(classify(70000))
}
```

```sh
bin/kai run examples/tutorial/09_refinements.kai
```

Both `requires` and `ensures` desugar to runtime asserts in v1.
The static refinement-narrowing pass (m12.6.x) lifts them into
caller-side type evidence as it ships.

## 9. Fibers and structured concurrency

`Spawn` is the effect that powers fibers. `import spawn` brings
in the typed wrappers (`fiber_spawn`, `fiber_await`,
`fiber_yield`).

Under the m8.x cooperative scheduler (kaikai 0.4.0+), `fiber_yield()`
parks the current fiber and lets the scheduler pick the next ready
one. Two workers that print marks with a yield between every print
*interleave* — not run-to-completion in sequence.

`examples/tutorial/10_fibers.kai`:

```kai
import spawn

fn worker_loop(name: String, n: Int, i: Int) : Unit / Spawn + Stdout = {
  if i >= n { () }
  else {
    print("#{name}#{i}")
    fiber_yield()
    worker_loop(name, n, i + 1)
  }
}

fn main() : Unit / Stdout + Spawn = {
  let fa = fiber_spawn(() => worker_loop("A", 3, 0))
  let fb = fiber_spawn(() => worker_loop("B", 3, 0))
  fiber_await(fa)
  fiber_await(fb)
}
```

```sh
bin/kai run examples/tutorial/10_fibers.kai
# A0
# B0
# A1
# B1
# A2
# B2
```

Fibers are isolated — each one has its own private heap, and
messages between them are copied (BEAM-style). Cancellation is
delivered cooperatively at yield points. Actors and supervision
build on the same machinery; see `docs/actors.md`.

## 10. Testing

Tests are first-class syntax: `test "description" { body }` plus
`assert <bool>`. They live next to production code in the same
file. `kai build` and `kai run` ignore them; only `kai test`
enables them.

`examples/tutorial/11_test.kai`:

```kai
fn collatz_length(n: Int) : Int =
  if n <= 1            { 0 }
  else if n % 2 == 0   { 1 + collatz_length(n / 2) }
  else                 { 1 + collatz_length(3 * n + 1) }

test "collatz of 6 is 8" {
  assert collatz_length(6) == 8
}

test "collatz of small inputs is bounded" {
  assert collatz_length(7)  == 16
  assert collatz_length(27) == 111
}

fn main() : Unit / Stdout = {
  print("collatz(27) = #{collatz_length(27)}")
}
```

Run the tests:

```sh
bin/kai test examples/tutorial/11_test.kai
#   ok   collatz of 1 is 0
#   ok   collatz of 6 is 8
#   ok   collatz of small inputs is bounded
#
# 3/3 tests passed
```

Run the program (tests are skipped):

```sh
bin/kai run examples/tutorial/11_test.kai
# collatz(27) = 111
```

## 11. Where to next

You've seen the language. From here:

**More examples in this repo**

- `examples/portfolio/portfolio.kai` — small accounting demo with
  `#derive(Show)` records, sum types, `Real<USD>`, `reduce`.
- `examples/usd_to_eur/usd_to_eur.kai` — currency converter with
  `Stdin` + `Stdout`, demonstrates the row inferred from `main`.
- `examples/aspirational/event_logger/event_logger.kai` — a custom
  `Logger` effect over a stream of typed events; combines `#derive`,
  custom effects, and HOFs.
- `examples/effects/m8x_2_yield_interleave.kai` — the canonical
  scheduler-interleave demo for fibers.

**Design docs**

- `docs/design.md` — top-level design, Tier 1/2/3 principles,
  decisions, roadmap.
- `docs/effects.md`, `docs/effects-stdlib.md`,
  `docs/effects-impl.md` — the effect model in three layers
  (semantics, stdlib catalog, CPS transform).
- `docs/structured-concurrency.md`, `docs/actors.md` — fibers,
  nurseries, mailboxes, links, supervision.
- `docs/units-of-measure.md` — UoM algebra and inference.
- `docs/refinements-and-contracts.md` — refinements,
  `requires` / `ensures`, the entailment story.
- `docs/protocols.md` — single-dispatch protocols and `#derive`.
- `docs/typed-holes.md` — `?` and `?name` typed holes,
  `--holes-json`, the LLM-authoring story.

**What does not ship yet**

Honest list:

- No HTTP / DB / web framework. Networking, persistence, and the
  Phoenix-style `ahu-web` framework are designed (see
  `docs/proposed-extensions.md`) but not implemented.
- `kai fmt`, `kai repl`, `kai lsp` are scheduled milestones
  (m15 / m16 / m17), not yet in `bin/kai`.
- Package manager (`kai new`, `kai add`) is post-MVP.
- Backwards compatibility across phases is not promised until
  post-MVP — the language and stdlib still move.
- Reference counting and the fiber scheduler have known cost in
  the v1 runtime; see `docs/m5x-followup.md` and
  `docs/m8x-followup.md` for inventories.

If something here surprises you, that's working as intended — the
effect surface, fibers, holes, and refinements are deliberately
novel. The familiar parts (records, sums, pipes, `Option`,
`match`) keep the day-one ramp short.
