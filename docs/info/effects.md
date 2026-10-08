# effects

Algebraic effects and handlers — kaikai's first-class mechanism for
IO, cancellation, non-determinism, actor messages, and every other
"something other than a pure function from values to values".

## Description

Every function that performs an effect declares it in its row type.
A handler is the only thing that can interpret an effect op; without
one lexically in scope, the call is a type error
(`effect not handled: E`).

kaikai follows Effekt's capability-passing discipline: the effect
name IS the capability (`Stdout.print("hi")`). The handler installs
the capability for its body's lexical scope.

Effects are INFERRED in local bodies. Public signatures MUST annotate
the row — that is Tier 1 #1 "safe at compile time". Rows are sets,
not ordered lists: `Stdout + File` ≡ `File + Stdout`. Duplicates are
idempotent.

Resume is ONE-SHOT and EXPLICIT: a clause receives `resume` as a
callable; calling it continues the body with a value and evaluates to
the handle's result. Not calling it abandons the continuation (e.g. an
op returning `Nothing`). Calling it twice on one path is a compile
error; a second call the compiler cannot see traps at run time.
A clause may also keep `resume` as a value (see *Keeping the
continuation* below).

Abandoning the continuation skips the `return` clauses of the handlers
it jumps over — those do not run, and neither does the abandoning
handler's own `return`: the clause's value is the handle's value as is,
so it has the handle's type. What DOES run on every such path is
a handler's `finally { }` clause (see *Cleanup* below), which is how a
scope releases a non-memory resource it holds. Perceus frees memory
regardless.

## Declaring

```kaikai
effect Logger {
  log(s: String) : Unit
}

effect Failure {
  fail(msg: String) : Nothing                  # Nothing = empty type
}                                              # → no resume possible

effect Counter {
  get() : Int
  set(v: Int) : Unit
}

fn main() : Int = 0
```

## Calling and handling

```kaikai
effect Greeter {
  greet(s: String) : Unit
}

fn say_hi(name: String) : Unit / Greeter = Greeter.greet(name)

fn main() : Int / Stdout = {
  let result = handle {
    say_hi("world")
    Greeter.greet("kaikai")
    42
  } with Greeter {
    greet(s, resume) -> { Stdout.print("hello #{s}"); resume(()) }
    return(x)        -> x
  }
  result
}
```

- `return(x) -> ...` is optional. Default is identity. It runs only when
  the body completes.
- Every op clause evaluates to the handle's type — what `return` yields,
  or the body's type when there is no `return`.
- Calling `resume(v)` continues the body with `v`; its value is the
  handle's result, so code after it runs once the body is done.
- Outside the `with { ... }`, `Greeter` is no longer in the row.

## Cleanup — `initially` / `finally`

A handler may carry an `initially { }` and a `finally { }` clause. The
pair is the acquire/release bracket: `initially` runs once when the
handler is installed and its value becomes the handler state, readable
as `state`; `finally` runs when the scope exits, however it exits.

```kaikai
effect Guard {
  step() : Unit
}

fn risky() : Int / Guard = { Guard.step(); 1 }

fn main() : Int / File + Stdout = {
  let r = handle {
    risky()
  } with Guard {
    # acquire — once, at installation; its value becomes `state`
    initially { File.open_read("data.txt") }
    # release — on every exit path
    finally   { match state { Ok(h) -> File.close_file(h) Err(_) -> () } }
    step(resume) -> resume(())
    return(x)    -> x
  }
  Stdout.print("r=#{int_to_string(r)}")
  0
}
```

`finally` runs on every path that UNWINDS the scope:

- normal exit off the end of the body,
- a handler clause that abandons `resume` (including one installed
  further out, whose jump skips this scope's frame entirely),
- cooperative cancellation delivered to the fiber.

It does NOT run on `panic`, which aborts the process rather than
unwinding it — the OS reclaims fds and memory at exit.

Two rules make the pair usable:

- **`finally` runs in its INSTALL-time evidence context.** An effect it
  performs dispatches to the handlers that were live when the handler
  was installed, not to those at the jump site — which the unwind has
  already torn down. So `finally` cannot perform the very effect its own
  handler discharges; that is a compile-time "effect not handled".
- **`initially` and the `(init)` form are the same slot**, so a handler
  writes one or the other, never both. `with State[Int](0)` is the terse
  spelling; `initially { 0 }` is the block form that may bind and perform.

`finally` takes no parameters and its value is discarded — it runs for
its effect. To transform the handle's result, use `return(x) -> ...`.

This is what lets a producer release a resource without interpreting the
consumer's effect: interpreting it would shadow the consumer's policy,
whereas `finally` only brackets the scope.

## Default handlers

An effect declaration can carry an optional `default { }` block with
fallback clauses the compiler auto-installs at `main` when the
program performs `Eff.op(...)` without an enclosing
`handle ... with Eff`. The block sits next to the op declarations
and uses the same clause shape as `handle`:

```kaikai
effect MyConsole {
  shout(msg: String) : Unit
  default {
    shout(msg, resume) -> $extern_handler("kai_default_stdout_print")
  }
}

fn main() : Unit / MyConsole = {
  MyConsole.shout("hello from a user effect default\n")
}
```

`$extern_handler("c_symbol")` is the compiler intrinsic that bridges
a default clause to a runtime C entry — the only form codegen
currently accepts inside `default { }`. Kaikai-bodied default
clauses parse and store but their codegen path is deferred (Stage C
of issue #533).

Coverage rules (typer):

0. **Every op needs a clause** — a `handle ... with Eff` lists every
   op of `Eff`, even ones nothing performs, except an op the
   `default { }` block bridges with `$extern_handler`. Otherwise:
   `handler for Eff does not cover every op: missing ...`.
1. **Inside a `handle ... with Eff { clauses }`** — the listed
   clauses discharge the op; the `default { }` block is unused at
   that call site.
2. **At `main`, no enclosing `handle`** — the `default { }` block's
   clause fires; the auto-installed handler runs `$extern_handler`.
3. **Partial handle + missing op in default** — typer rejects with
   `effect not handled: Eff.op`. Merging defaults *into* a partial
   handle is Stage C work; today, list the op explicitly in the
   `handle` or let it fall through to the default at `main`.

For builtin effects, the 18 canonical handlers (Stdout, Stderr,
Stdin, Env, File, Clock, Random, SecureRandom, NetTcp, Signal,
Process, Log, Mutable, Cancel, Link, Monitor, Spawn) ship
default installation paths in the runtime — `Log` is one of them
and its default writes to stderr in ISO-8601 form without a literal
`default { }` block in `stdlib/log.kai`. The full catalog lives in
`docs/effects-stdlib.md`; the runtime implementation rationale and
the Stage A/B/C trilogy live in `docs/effects.md`.

## Handler state — `state` and `log`

A stateful handler (`with State[T](init)`, `with Writer[W]([])`, any
`with Eff(init)`) binds its state slot in every clause under two
spellings: `state` and `log`. They name the same slot — `state` reads
naturally for `State[T]`, `log` for accumulator-shaped handlers. The
`return` clause sees them too.

```kaikai
effect Res { acquire() : Int }

fn main() : Int / Stdout = {
  let r = handle {
    Res.acquire()
  } with Res(6) {
    acquire(resume) -> resume(state)
    return(x)       -> x + log            # same slot as `state`
  }
  Stdout.print(int_to_string(r))
  0
}
```

An enclosing binding of either name **wins over the alias**: a clause
under `let log = ...` reads that `log`, and the compiler warns that the
handler state is not reachable under that name there. Rename the
enclosing binding if the clause needs the state. A `var` is unaffected:
it reaches its cell without going through either name.

## Rebinding the capability name

When two handlers of the same effect nest, use `as`. The rebinding
introduces a local capability under the chosen name; the original
effect name remains usable inside the handle body. Convention is to
prefer the local binding for clarity.

```kaikai
effect Logger {
  log(s: String) : Unit
}

fn main() : Int / Stdout = {
  handle {
    log.log("first")                           # via the rebound name
    log.log("second")
    0
  } with Logger as log {
    log(s, resume) -> { Stdout.print(s); resume(()) }
  }
}
```

## Named instances — a capability as a first-class value

`with Eff as a` binds `a` as a value whose type IS the effect
(`Cell`, `State[Int]`) — no `Handler[E]` wrapper. So a capability
appears in two positions, one uniform rule:

- in a row (`fn f() : T / Cell`) — *demanded*, satisfied by a handle;
- as a parameter (`fn f(c: Cell)`) — *provided* by the caller, NOT in
  the row.

This lets several instances of one effect be addressed independently
by passing them *down* into a function:

```kaikai
effect Cell { get() : Int  set(n: Int) : Unit }

fn add(c1: Cell, c2: Cell, dst: Cell) : Unit =
  dst.set(c1.get() + c2.get())            # each performs against its own instance

fn main() : Int = 0
```

A capability is **second-class**: it may appear only as a call
argument or an op receiver. It cannot be returned from its `handle`,
stored in a value, or captured by a closure that outlives the block —
a value that must outlive its scope is a `Ref[T]` under `Mutable`
(within one fiber — a `Ref` does not cross a spawn, see
`kai info fibers`), a dynamic population of stateful entities is
actors/`Spawn`. Instances
are monomorphic (`f(c: State[Int])`, not `f(c: State[T])`); `mask` is
not provided — name the outer instance instead.

## Two instances of one effect in a row

A row may carry one parametric effect at two types (`Box[Int] +
Box[String]`, `Actor[Request] + Actor[Reply]`). Each op reaches the
handler of the instance it is typed at, wherever that handler sits on
the stack. When nothing pins which instance an op means — its result
and arguments fit both — the call is rejected as an `ambiguous effect
instance`, never resolved by handler order.

```kaikai
effect Box[T] {
  peek() : T
}

fn pair() : String / Box[Int] + Box[String] = {
  let n: Int = Box.peek()                  # pinned by its type: Box[Int]
  let s: String = Box.peek()               # Box[String]
  "#{s}#{n}"
}

fn main() : Int = 0
```

## Keeping the continuation — `Cont[T, S, e]`

A clause may store `resume`, return it, or call it from a lambda or a
loop. It is then a value of type `Cont[T, S, e]`: `T` is what the op
returns, `S` the handle's type, `e` what the body performs besides the
handled effect, and `k(v) : S / e` resumes the body. `Cont[T, S]` is
the continuation of a pure body (`e` empty). Such a handle runs its
body on a stack segment; `kai build --explain` notes each one and why.
A dropped continuation discontinues its body: every `finally` on it
runs.

```kaikai
effect Yield { yield(x: Int) : Unit }

type Gen = Done | Next(Int, Cont[Unit, Gen])

fn upto(n: Int, lim: Int) : Unit / Yield =
  if n > lim { () } else { Yield.yield(n); upto(n + 1, lim) }

fn generate(lim: Int) : Gen =
  handle { upto(1, lim); Done } with Yield { yield(x, resume) -> Next(x, resume) }

fn sum_all(g: Gen, acc: Int) : Int = match g {
  Done       -> acc
  Next(x, k) -> sum_all(k(()), acc + x)
}

fn main() : Unit / Stdout = Stdout.print("#{sum_all(generate(10), 0)}")   # 55
```

The body's other effects reach the handlers around wherever `k(v)` is
called, not those around the first run, so `k(v)` needs `e` handled.
Dropping the continuation runs its `finally` blocks where it is dropped,
so a drop needs `e` handled too.

```kaikai
effect Yield { yield(x: Int) : Unit }
effect Log { log(s: String) : Unit }

type Gen[e] = Done | Next(Int, Cont[Unit, Gen[e], e])

fn counted(n: Int) : Unit / Yield + Log =
  if n == 0 { () } else { Log.log("at #{n}"); Yield.yield(n); counted(n - 1) }

fn generate() : Gen[Log] / Log =
  handle { counted(2); Done } with Yield { yield(x, resume) -> Next(x, resume) }

fn sum_all(g: Gen[Log], acc: Int) : Int / Log = match g {
  Done       -> acc
  Next(x, k) -> sum_all(k(()), acc + x)
}

fn main() : Unit / Stdout =
  handle { Stdout.print("#{sum_all(generate(), 0)}") }   # at 2, at 1, 3
  with Log { log(s, resume) -> { Stdout.print(s); resume(()) } }
```

- A continuation stays on the fiber that created it: a thunk handed to
  `spawn`, a fiber's result, or an actor message cannot hold one.
- A handle cannot return a value holding a continuation that performs
  the effect the handle discharges. A continuation that leaves its
  handlers some other way stops the program if resumed or dropped there.

A stateful handle (`with Eff(init)`) keeps its continuation too. It is
then a `Cont[(T, H), S, e]`, with `H` the state type, and `k(v, s)`
resumes the body with `v` and the next state `s`, as `resume(v, s)`
does. Inside the clause, `resume(v)` still means `resume(v, state)`.

```kaikai
effect Yield { yield(x: Int) : Unit }

type Sums = Total(Int) | Next(Int, Cont[(Unit, Int), Sums])

fn upto(n: Int, lim: Int) : Unit / Yield =
  if n > lim { () } else { Yield.yield(n); upto(n + 1, lim) }

fn sums(lim: Int) : Sums =
  handle { upto(1, lim) } with Yield(0) {
    yield(x, resume) -> Next(state + x, resume)    # the running sum so far
    return(_u)       -> Total(state)
  }

fn drive(g: Sums) : Unit / Stdout = match g {
  Total(t)   -> Stdout.print("total #{t}")         # total 10
  Next(s, k) -> { Stdout.print("sum #{s}"); drive(k((), s)) }
}

fn main() : Unit / Stdout = drive(sums(4))
```

## Build your own generator

A generator is three things the program declares itself: an effect the
producer performs, a type that holds the next element and the
continuation, and a handler that keeps `resume`. Nothing more is needed.

The handler in `stream` turns each `Emit.emit(v)` into `Next(v, resume)`.
The producer then waits on its own stack segment until a consumer calls
`k(())`, which runs it to its next `emit`. Because the producer is
suspended rather than run to the end, it may be infinite, like
`fibonacci` here.

Each stage is itself a generator that steps its source from inside its
own producer. `map` re-emits every element through `f`. `zip_with` steps
two sources in lockstep, and `take_until` stops after the first element
that satisfies `p`. A stage's functions run inside the producer, so they
are pure.

The pipeline below compares the squares of 1..99 with the fibonacci
sequence and prints each index and difference, up to the first negative
one: 13 lines, ending in `13 -64`. When `take_until` stops, the rest of
the pipeline goes unreferenced. Dropping a continuation discontinues
its producer, so the endless `fibonacci` never runs again and its
segment is released.

```kaikai
effect Emit { emit(v: Int) : Unit }

type Stream = Done | Next(Int, Cont[Unit, Stream])

fn stream(body: () -> Unit / Emit) : Stream =
  handle { body(); Done } with Emit { emit(v, resume) -> Next(v, resume) }

fn naturals(from: Int, to: Int) : Unit / Emit =
  if from >= to { () } else { Emit.emit(from); naturals(from + 1, to) }

fn fibonacci(a: Int, b: Int) : Unit / Emit = {
  Emit.emit(a)
  fibonacci(b, a + b)
}

fn emit_mapped(s: Stream, f: (Int) -> Int) : Unit / Emit = match s {
  Done       -> ()
  Next(v, k) -> { Emit.emit(f(v)); emit_mapped(k(()), f) }
}

fn map(s: Stream, f: (Int) -> Int) : Stream = stream { emit_mapped(s, f) }

fn emit_zipped(a: Stream, b: Stream, f: (Int, Int) -> Int) : Unit / Emit = match a {
  Done        -> ()
  Next(x, ka) -> match b {
    Done        -> ()
    Next(y, kb) -> { Emit.emit(f(x, y)); emit_zipped(ka(()), kb(()), f) }
  }
}

fn zip_with(a: Stream, b: Stream, f: (Int, Int) -> Int) : Stream =
  stream { emit_zipped(a, b, f) }

fn emit_until(s: Stream, p: (Int) -> Bool) : Unit / Emit = match s {
  Done       -> ()
  Next(v, k) -> {
    Emit.emit(v)
    if p(v) { () } else { emit_until(k(()), p) }
  }
}

fn take_until(s: Stream, p: (Int) -> Bool) : Stream = stream { emit_until(s, p) }

fn each_from(s: Stream, i: Int, f: (Int, Int) -> Unit / Stdout) : Unit / Stdout = match s {
  Done       -> ()
  Next(v, k) -> { f(i, v); each_from(k(()), i + 1, f) }
}

fn each_indexed(s: Stream, f: (Int, Int) -> Unit / Stdout) : Unit / Stdout = each_from(s, 0, f)

fn main() : Unit / Stdout = {
  let squares = map(stream { naturals(1, 100) }, (x) => x * x)
  let fibos = stream { fibonacci(1, 1) }
  let diffs = take_until(zip_with(squares, fibos, (s, f) => s - f), (d) => d < 0)
  each_indexed(diffs, (i, d) => Stdout.print("#{i + 1} #{d}"))
}
```

The stdlib ships the same shape as `gen` (`kai doc gen`): `Yield[t]`,
`Gen[t, e]`, `generate`, and stages that ride the pipes. There the
producer may perform other effects too: they are the `e` of `Gen[t, e]`.

## Stdlib effects

Stdin, Stdout, Stderr, File, Env, Console, Clock, Random,
SecureRandom, NetTcp, Process, Spawn, Cancel, Actor[Msg],
Signal, Mutable, State, Reader, Writer, Log, Link,
Monitor, Ffi.

Full catalog: `docs/effects-stdlib.md`. The main effect at the
program entry installs Stdin/Stdout/Stderr/Env/File automatically
when inferred. NetUdp / NetDns are reserved as part of the planned
`Net` aggregate but are not shipped in v1 — `NetTcp` is the only
network effect available today.

An effect you declare or import with a stdlib effect's name shadows it
for bare uses in that file; the stdlib one stays reachable qualified:

```kai
effect Log { note(n: Int) : Unit }

fn report() : Unit / Log + effects.Log = {
  Log.note(1)                  # this file's Log
  effects.Log.info("done")     # the stdlib Log
}
```

## NOT IN KAIKAI

- `do { ... }` notation (Haskell). Effect-using code is just a
  block; the row type carries the discipline.
- Multi-shot resume. One-shot only.
- Effect polymorphism via type classes. kaikai uses row variables.
- Throwing/catching exceptions. Use `Result[a, e]` with postfix `!`,
  or declare an effect whose op returns `Nothing`.

## See also

`kai info fibers`, `kai info actors`, `kai info syntax`
