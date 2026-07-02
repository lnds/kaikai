# kaikai-minimal examples

Canonical examples of the kaikai-minimal subset (the language compiled by stage 0). They exist both for human reading and as checkable artifacts once stage 0 is functional.

See [../../docs/kaikai-minimal.md](../../docs/kaikai-minimal.md) for the full specification.

## Files

| File | Demonstrates |
|------|--------------|
| `hello.kai` | Program structure, `main`, `print` |
| `fizzbuzz.kai` | `if`/`else if`, arithmetic, `match` on numbers, pipes, `map` |
| `quicksort.kai` | Pattern matching on lists, spread cons, recursion, higher-order functions, tests |
| `interp.kai` | Sum types (tagged unions), AST representation, recursive interpreter — the dogfooding target |
| `attributes.kai` | `#[...]` attributes skipped by the lexer, including multi-line bodies (triple-string, bracketed) |

## Checking

Until stage 0 is available, these files are verified **by eye** against the grammar in the specification. Once stage 0 builds, `stage0 examples/minimal/*.kai` should accept each one without errors.
