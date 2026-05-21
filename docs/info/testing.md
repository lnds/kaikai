TESTING(7)                      kaikai                      TESTING(7)

NAME
  testing — test blocks, assertions, benchmarks, property checks

SYNOPSIS
  test "<name>" { ... assert <expr> ; ... }
  bench "<name>" { ... }
  check "<name>" with <binders> { ... assert <expr> ; ... }

  kai test [<spec>|./...]
  kai bench [<spec>] [--iters N]
  kai check [<spec>]

DESCRIPTION
  Testing is a LANGUAGE BUILT-IN, not a stdlib library. `test`,
  `bench`, `check`, and `assert` are keywords. The `kai test` driver
  compiles the source with a flag that includes the test blocks and
  produces a test runner binary.

  `assert` takes a boolean expression; on false it records a failure
  with the expression's source text and line. There is no special
  matcher DSL — `assert x == 42` is enough.

EXAMPLES

  fn double(n: Int) : Int = n * 2

  test "double doubles" {
    assert double(0) == 0
    assert double(7) == 14
    assert double(-3) == -6
  }

PROPERTY CHECKS

  fn reverse_twice_is_identity[a](xs: [a]) : Bool =
    list.reverse(list.reverse(xs)) == xs

  check "reverse is involutive" with xs: [Int] {
    assert reverse_twice_is_identity(xs)
  }

  Shrinking is built in (#438). On failure, the runner reports the
  minimal counterexample.

BENCHMARKS

  bench "fib 30" {
    fib(30)
  }

  Driver runs N iterations (default 100, override `--iters`),
  reports median and MAD.

WALKING A WORKSPACE
  `kai test ./...` walks every kai.toml under cwd and runs each
  package's tests.

EXIT CODES
  - 0   all tests passed (or no tests defined)
  - 1   one or more failures
  - 2   compile error before tests could run

NOT IN KAIKAI
  - xUnit-style fixtures (`setUp`, `tearDown`). Use a fn that
    returns the fixture; call it from each test.
  - Async test wrappers. Use `Spawn` inside the test body.
  - Mocking frameworks. Pass fakes explicitly or use handler
    substitution (replace the real `Logger` handler with a recording
    one in the test scope).
  - Snapshot testing v1. Deferred.

SEE ALSO
  kai info effects (handler substitution for fakes),
  kai info holes (typed holes for incremental TDD)
