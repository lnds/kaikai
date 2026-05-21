EFFECTS(7)                      kaikai                      EFFECTS(7)

NAME
  effects — algebraic effects and handlers

SYNOPSIS
  effect Name { op(args) : T ; ... }
  handle { body } with Name { op(args, resume) -> ... ; return(x) -> ... }
  handle { body } with Name as cap { ... }
  fn f(...) : R / E1 + E2 = ...                     # row in signature

DESCRIPTION
  Effects are first-class. Every function that performs an effect
  declares it in its row type. A handler is the only thing that can
  interpret an effect op; without one lexically in scope, the call
  is a type error (`effect not handled: E`).

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

DECLARING

  effect Logger {
    log(s: String) : Unit
  }

  effect Fail {
    fail(msg: String) : Nothing                     # Nothing = empty type
  }                                                 # → no resume possible

  effect State[T] {
    get() : T
    set(v: T) : Unit
  }

CALLING

  fn greet(name: String) : Unit / Logger {
    Logger.log("hello, #{name}")
  }

HANDLING

  fn main() : Int / Stdout = {
    let result = handle {
      Logger.log("start")
      42
    } with Logger {
      log(s, resume) -> { Stdout.print(s); resume(()) }
      return(x)      -> x
    }
    result
  }

  - `return(x) -> ...` is optional. Default is identity.
  - Calling `resume(v)` continues the body with `v`.
  - Outside `with { }`, `Logger` is no longer in the row.

REBINDING THE CAPABILITY NAME
  When two handlers of the same effect nest, use `as`:

    handle { ... log.log("x") ... } with Logger as log { ... }

  Inside that body `Logger` is NOT in scope; only `log` is.

STDLIB EFFECTS
  Stdin, Stdout, Stderr, File, Env, Console, Clock, Random,
  SecureRandom, NetTcp, NetUdp, NetDns, Process, Spawn, Cancel,
  Actor[Msg], Signal, Mutable, State, Reader, Writer, Fail, Ffi.

  Full catalog: `docs/effects-stdlib.md`. The main effect at the
  program entry installs Stdin/Stdout/Stderr/Env/File automatically
  when inferred — see `kai info main`.

NOT IN KAIKAI
  - `do { ... }` notation (Haskell). Effect-using code is just a
    block; the row type carries the discipline.
  - Multi-shot resume. One-shot only.
  - Effect polymorphism via type classes. kaikai uses row variables.
  - Throwing/catching exceptions. Use `Fail` effect or `Result[e, a]`.

SEE ALSO
  kai info fibers, kai info actors, kai info syntax, kai info main
  docs/effects.md (full semantics), docs/effects-stdlib.md (catalog),
  docs/effects-impl.md (CPS + runtime)
