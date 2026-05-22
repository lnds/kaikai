# `kai fmt` fixtures

Each `*.input.kai` has a paired `*.expected.kai`. The fixture suite runs:

1. `kaic2 --fmt input.kai` must equal `expected.kai` byte-for-byte.
2. `kaic2 --fmt expected.kai` must equal `expected.kai` (idempotency).

Driven by `tests/fmt_fixtures.sh` (also wired into `make tier1`).

## Coverage

Issue #670 lane (2026-05-21) filled every remaining `fmt_panic_unsupported` arm; `kai fmt` now formats every kaikai surface construct. The fixtures cover:

- **Tongariki baseline**: imports, simple fns, type decls (record / sum / alias), tests, base expressions and patterns, pipes, ranges, string interpolation, line comments, lambdas, records, lists, nested match, trailing commas.
- **Closed by #670**: generic params on fns and types (`fn id[T]`, `Box[T]`, `[U: Measure]`), `effect` declarations, `protocol` declarations, `impl` blocks, `#[derive(...)]`, `#[unstable]`, `unit` declarations + `unit_expr`, file-scope `use`, `axiom` declarations, `extern "C"` (including `("symbol")` overrides per #261), `var` bindings, `a[i] := v` index assignment, `where`-refinement types, dimensioned types (`Real<m>`), unit-literal annotations (`100<USD>`), `variants[T]()`, full `handle` blocks with `[tyargs]` / `(init)` / `as alias` / `return(x)` clauses, parametric effect labels (`Reader[Int]`), open effect rows (`/ Io + e`).

Failure to round-trip any new kaikai construct should land as a fixture pair here.
