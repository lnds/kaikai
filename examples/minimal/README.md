# kaikai-minimal examples

Canonical examples of the kaikai-minimal subset (the language compiled by stage 0 and stage 1).

See [../../docs/kaikai-minimal.md](../../docs/kaikai-minimal.md) for the full specification.

## Files

| File | Demonstrates |
|------|--------------|
| `hello.kai` | Program structure, `main`, `print` |
| `fizzbuzz.kai` | `if`/`else if`, arithmetic, `match` on numbers, pipes, `map` |
| `quicksort.kai` | Pattern matching on lists, spread cons, recursion, higher-order functions, tests |
| `interp.kai` | Sum types (tagged unions), AST representation, recursive interpreter |
| `capture.kai` | Closures capturing a parameter |
| `encode_decode_user_fn.kai` | User functions named `encode` / `decode` lower to the user symbol |

## Checking

`make -C stage0 test` compiles each file with `kaic0`, runs it, and runs its
`test` blocks; `make -C stage1 test` does the same through `kaic1` and checks
token parity with stage 0.
