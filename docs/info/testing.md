# testing

Test blocks, assertions, benchmarks, property checks.

## Description

Testing is a LANGUAGE BUILT-IN, not a stdlib library. `test`,
`bench`, `check`, and `assert` are keywords. The `kai test` driver
compiles the source with a flag that includes the test blocks and
produces a test runner binary.

`assert` takes a boolean expression; on false it records a failure
with the expression's source text and line. There is no special
matcher DSL — `assert x == 42` is enough.

## Driver commands

```text
kai test [<spec>|./...]            # run tests
kai bench [<spec>] [--iters N]     # run benchmarks
kai check [<spec>]                 # run property blocks
```

## Tests

```kaikai
fn double(n: Int) : Int = n * 2

test "double doubles" {
  assert double(0) == 0
  assert double(7) == 14
  assert double(0 - 3) == 0 - 6
}

fn main() : Int = 0
```

## Property checks

```kaikai
fn double(n: Int) : Int = n * 2

check "double of double is times 4" with n: Int {
  assert double(double(n)) == n * 4
}

fn main() : Int = 0
```

Shrinking is built in. On failure, the runner reports the minimal
counterexample.

## Benchmarks

```kaikai
fn fib(n: Int) : Int = if n < 2 { n } else { fib(n - 1) + fib(n - 2) }

bench "fib 10" {
  fib(10)
}

fn main() : Int = 0
```

The driver runs N iterations (default 100, override `--iters`),
reports median and MAD.

## Walking a workspace

`kai test ./...` walks every `kai.toml` under cwd and runs each
package's tests.

## Exit codes

```text
0   all tests passed (or no tests defined)
1   one or more failures
2   compile error before tests could run
```

## NOT IN KAIKAI

- xUnit-style fixtures (`setUp`, `tearDown`). Use a fn that
  returns the fixture; call it from each test.
- Async test wrappers. Use `Spawn` inside the test body.
- Mocking frameworks. Pass fakes explicitly or use handler
  substitution (replace the real `Logger` handler with a recording
  one in the test scope).
- Snapshot testing. Deferred.

## See also

`kai info effects` (handler substitution for fakes),
`kai info holes` (typed holes for incremental TDD)
