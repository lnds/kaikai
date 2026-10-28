# examples/unstable

Fixtures exercising the `#[unstable]` attribute and the `[unstable]`
opt-in in `kai.toml`.

## Workflow

A library author marks a `pub fn / pub type / pub const` with
`#[unstable]` to indicate the API is **not** covered by the edition
stability contract. A consumer importing that declaration
acknowledges the risk by adding `<pkg> = true` under `[unstable]`
in their own `kai.toml`. The compiler emits a warning at every
qualified call to an unacknowledged `#[unstable]` export; the build
still succeeds.

Each subdirectory has a `main.kai`, usually a `kai.toml`, and a module
holding the marked declarations. Run with `bin/kai run <dir>/main.kai`.

## Fixtures

- **`pub_fn_with_optin/`** — `[unstable] mylib = true` in
  `kai.toml`. `main.kai` calls `mylib.experimental(...)` and the
  compiler does NOT emit a warning. Expected output: `42`.

- **`pub_fn_no_optin/`** — no `[unstable]` section in
  `kai.toml`. `main.kai` calls `mylib.experimental(...)`; the
  compiler emits a warning anchored at the call site
  (`mylib.experimental is #[unstable] — ...`). The program still
  runs and prints `42`.

- **`pub_type/`** — `#[unstable] pub type Box` plus an
  `#[unstable] pub fn` that reads it. `kai.toml` opts in. Expected
  output: `7`.

- **`multi_unstable_pub_decls/`** — two `#[unstable]` functions and a
  stable one in one module. Nothing opts in; a warning appears for
  each consumed unstable export. Expected output: `103`.

- **`non_pub_ignored/`** — `#[unstable]` on a non-`pub` declaration
  is rejected: `` `#[unstable]` must precede a `pub fn`, `pub type`,
  `pub const`, `pub effect`, or `pub protocol` declaration``.

- **`unknown_pkg_optin/`** — `[unstable] zzz = true` for a
  package that the program never imports. The compiler does
  not validate the opt-in target; the entry is benign and
  the program runs normally.

The corpus parity sweep (`tools/lib/corpus.sh`) builds every fixture
here. See `docs/editions.md` for the edition-stability contract and
`docs/protocols.md` for the attribute.
