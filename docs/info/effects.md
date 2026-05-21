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
callable; calling it continues the body with a value. Not calling it
abandons the continuation (e.g. `Fail`). Calling it more than once
is currently a runtime error.

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

- `return(x) -> ...` is optional. Default is identity.
- Calling `resume(v)` continues the body with `v`.
- Outside the `with { ... }`, `Greeter` is no longer in the row.

## Rebinding the capability name

When two handlers of the same effect nest, use `as`. Inside that body
the original effect name is NOT in scope; only the rebinding is.

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

## Stdlib effects

Stdin, Stdout, Stderr, File, Env, Console, Clock, Random,
SecureRandom, NetTcp, NetUdp, NetDns, Process, Spawn, Cancel,
Actor[Msg], Signal, Mutable, State, Reader, Writer, Fail, Ffi.

Full catalog: `docs/effects-stdlib.md`. The main effect at the
program entry installs Stdin/Stdout/Stderr/Env/File automatically
when inferred.

## NOT IN KAIKAI

- `do { ... }` notation (Haskell). Effect-using code is just a
  block; the row type carries the discipline.
- Multi-shot resume. One-shot only.
- Effect polymorphism via type classes. kaikai uses row variables.
- Throwing/catching exceptions. Use `Fail` effect or `Result[e, a]`.

## See also

`kai info fibers`, `kai info actors`, `kai info syntax`
