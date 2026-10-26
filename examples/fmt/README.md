# `kai fmt` fixtures

Each `*.input.kai` has a paired `*.expected.kai`. The fixture suite runs:

1. `kaic2 --fmt input.kai` must equal `expected.kai` byte-for-byte.
2. `kaic2 --fmt expected.kai` must equal `expected.kai` (idempotency).

Driven by `tests/fmt_fixtures.sh` (`make test-fmt`, part of tier 1).

## Coverage

`kai fmt` formats every kaikai surface construct; the writer refuses nothing, so a file is rejected only when it fails to parse. The fixtures cover:

- **Base grammar**: imports, simple fns, type decls (record / sum / alias), tests, base expressions and patterns, pipes, ranges, string interpolation, line comments, lambdas, records, lists, nested match, trailing commas.
- **Advanced surface**: generic params on fns and types (`fn id[T]`, `Box[T]`, `[U: Measure]`), `effect` declarations, `protocol` declarations, `impl` blocks, `#[derive(...)]`, `#[unstable]`, `unit` declarations + `unit_expr`, file-scope `use`, `axiom` declarations, `extern "C"` (including `("symbol")` overrides), `var` bindings, `a[i] := v` index assignment, `where`-refinement types, dimensioned types (`Real<m>`), unit-literal annotations (`2.0<s>`), `variants[T]()`, full `handle` blocks with `[tyargs]` / `(init)` / `as alias` / `return(x)` clauses, parametric effect labels (`Reader[Int]`), open effect rows (`/ Io + e`).

`help_scope.input.kai` carries one construct per name in the `kai fmt --help` scope paragraph. It is also driven by `tests/fmt_help_scope.sh`, which fails if one of them stops formatting or if the help text claims the formatter refuses a construct.

Failure to round-trip any new kaikai construct should land as a fixture pair here.
