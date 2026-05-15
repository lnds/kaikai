# examples/unstable

Fixtures exercising the `#unstable` annotation + `[unstable]`
opt-in workflow (issue #602, first lane of the Hanga Roa edition).

## Workflow

A library author marks a `pub fn / pub type / pub const` with
`#unstable` to indicate the API is **not** covered by the edition
stability contract. A consumer importing that declaration must
acknowledge the risk by adding `<pkg> = true` under `[unstable]`
in their own `kai.toml`. The compiler emits a warning at every
qualified call to an unacknowledged `#unstable` export; the build
still succeeds.

Each subdirectory has its own `kai.toml`, a `mylib/` (or similar)
holding the marked declarations, and `main.kai` consuming them.
Run with `bin/kai run <dir>/main.kai`.

## Fixtures

- **`pub_fn_with_optin/`** — `[unstable] mylib = true` in
  `kai.toml`. `main.kai` calls `mylib.experimental(...)` and the
  compiler does NOT emit a warning. Expected output: `42`.

- **`pub_fn_no_optin/`** — no `[unstable]` section in
  `kai.toml`. `main.kai` calls `mylib.experimental(...)`; the
  compiler emits a warning anchored at the call site. Expected
  stderr contains `mylib.experimental is #unstable`. Program
  still runs and prints `42`.

- **`pub_type/`** — `#unstable pub type T = ...` plus a `pub fn`
  that returns it. `kai.toml` opts in. Expected output: `7`.

- **`multi_unstable_pub_decls/`** — three `#unstable` decls
  (fn, type, const) in one module. `kai.toml` does NOT opt in;
  warnings appear for each consumed unstable export.

- **`non_pub_ignored/`** — `#unstable fn` on a NON-`pub`
  declaration is a parse error (annotation only applies to pub
  surface). The fixture documents the negative behaviour.

- **`unknown_pkg_optin/`** — `[unstable] zzz = true` for a
  package that the program never imports. The compiler does
  not validate the opt-in target; the entry is benign and
  the program runs normally.

See `docs/editions.md` for the surrounding edition-stability
contract and `docs/protocols.md` for the annotation grammar.
