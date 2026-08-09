# testing

Test blocks, assertions, benchmarks, property checks.

## Description

Testing is a LANGUAGE BUILT-IN, not a stdlib library. `test`,
`bench`, `check`, and `assert` are keywords. The `kai test` driver
compiles the source with a flag that includes the test blocks and
produces a test runner binary.

`assert` takes a boolean expression; on false it records a failure
with the expression's source text and line. There is no special
matcher DSL — `assert x == 42` is enough. `assert` belongs inside
`test` and `bench` bodies; `check` bodies are different (see below).

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
  double(double(n)) == n * 4
}

fn main() : Int = 0
```

A `check` body is a **Bool expression**, not a statement block:
the runner generates inputs for the `with`-clause params, evaluates
the body, and treats `true` as the property holding for that
sample. Do not use `assert` inside `check` — `assert` is a statement
with no value, so the body has nothing to witness and the runner
records a counterexample on every iteration. Express the property
directly: `n + 1 == 1 + n`, `not p(x) or q(x)`, etc.

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

## `*_test.kai` files

In package mode (`kai test`, `kai test .`, `kai test ./<sub>`, and
each package `./...` visits), every `*_test.kai` file the package
owns runs as its own test binary — nothing needs to import it. Test
files are leaves by nature, so reachability from the entry point
plays no part in whether they run; a file already in the entry's
import graph runs once, through the entry binary.

`kai check` and `kai bench` discover the same files: one convention
serves all three keywords, so a `*_test.kai` need not be split per
keyword to be found.

A package with no entry point — a library — runs its tests the same
way. Since reachability plays no part, an entry is not needed to
find them: `kai test`, `kai test .` and each package `./...` visits
run the suite, and only the entry binary is skipped. `kai build` and
`kai run` still require an entry, having nothing to build without
one.

A file outside the import graph that declares blocks but is not
named `*_test.kai` cannot safely run as a root, so the driver warns,
naming the file. A package with no blocks of the kind being run also
warns instead of passing silently — `0/0 checks passed` no longer
stands in for "found nothing to run".

## The `tests/` directory

When a package (a directory with `kai.toml`) carries a sibling
`tests/` directory, `kai test .` compiles and runs every `*.kai`
file under `tests/` in addition to the entry point. Each test file
becomes its own test binary; imports inside the file resolve to
the parent package's modules via sibling resolution — no extra
`kai.toml` inside `tests/`.

```text
mypkg/
  kai.toml             # name = "mypkg"
  main.kai
  mathlib/adder.kai    # pub fn add, pub fn double
  tests/
    test_adder.kai     # `import mypkg.mathlib.adder`
```

```sh
kai test .             # runs main.kai's blocks + tests/*.kai
```

Files inside `tests/` follow the same rules as a top-level test
file: at least one `test "..." { ... }` block plus an
`fn main() : Int = 0` placeholder (the `--test` driver replaces
`main` with the test runner). Non-`*.kai` files (fixtures, golden
files) under `tests/` are ignored by the runner.

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
