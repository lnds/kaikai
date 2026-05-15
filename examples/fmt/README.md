# `kai fmt` fixtures

Each `*.input.kai` has a paired `*.expected.kai`. The fixture suite runs:

1. `kaic2 --fmt input.kai` must equal `expected.kai` byte-for-byte.
2. `kaic2 --fmt expected.kai` must equal `expected.kai` (idempotency).

Driven by `tests/fmt_fixtures.sh` (also wired into `make tier1`).

Tongariki v1 covers: imports, simple fns (mono, no contracts), type
decls (record/sum/alias), tests, base expressions and patterns,
pipes, ranges, string interpolation, line comments. Out-of-scope
constructs (effects, handlers, protocols, impls, units, refinements,
axioms, generic params, parametric effect rows, `var`) are rejected
by the formatter with an explicit error — fixtures for them belong
to future Tongariki / Hanga Roa lanes.
