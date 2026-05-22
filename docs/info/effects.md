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

1. **Inside a `handle ... with Eff { clauses }`** — the listed
   clauses discharge the op; the `default { }` block is unused at
   that call site.
2. **At `main`, no enclosing `handle`** — the `default { }` block's
   clause fires; the auto-installed handler runs `$extern_handler`.
3. **Partial handle + missing op in default** — typer rejects with
   `effect not handled: Eff.op`. Merging defaults *into* a partial
   handle is Stage C work; today, list the op explicitly in the
   `handle` or let it fall through to the default at `main`.

For builtin effects, the 16 canonical handlers (Stdout, Stderr,
Stdin, File, Env, Console, Clock, Random, …) ship default
installation paths in the runtime — `Log` is one of them and its
default writes to stderr in ISO-8601 form without a literal
`default { }` block in `stdlib/log.kai`. The full catalog lives in
`docs/effects-stdlib.md`; the runtime implementation rationale and
the Stage A/B/C trilogy live in `docs/effects.md`.

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
