PACKAGES(7)                     kaikai                     PACKAGES(7)

NAME
  packages — kai.toml, imports, visibility

SYNOPSIS
  kai init <name>                   # create a kai.toml in cwd
  kai add <source>[@<ref>]          # add a dep
  kai install                       # resolve + lock
  kai update [<name>]               # refresh deps
  kai show                          # dump parsed manifest

DESCRIPTION
  A kaikai package is a directory with a `kai.toml` manifest. The
  package name in the manifest is what other packages import.

  Files inside a package are merged at build time. Each file declares
  what it imports from other packages and what it exports.

MANIFEST

  # kai.toml
  [package]
  name = "mathlib"
  version = "0.1.0"
  entry = "main.kai"                # optional; defaults to main.kai

  [dependencies]
  jsonlib = { git = "https://github.com/x/jsonlib", tag = "v1.2.0" }
  utilslib = { path = "../utilslib" }

IMPORTS

  import mathlib                    # whole package
  import mathlib.vec                # specific module
  import mathlib as m               # rename
  import mathlib.{add, mul}         # selective: only `add` and `mul`

  Imports are RESOLVED through `kai.toml` — the name on the right of
  `import` must appear as a dep (or be a stdlib name).

VISIBILITY

  pub fn add(a: Int, b: Int) : Int = a + b      # exported
  fn helper() : Int = 42                         # private to file

  Private items cannot be imported by other packages. Public items in
  internal packages do NOT leak to consumers if the public package
  re-exports them through a public surface; the typer's pub-leak
  validator rejects code that would leak a private type or effect
  through a `pub` signature.

STDLIB IS AUTO-LOADED
  The Hanga Roa "core" set is loaded automatically — no import
  needed for: Stdin/Stdout/Stderr, File, Env, Console, Option, Result,
  List ops, Array, String, Char, Int/Real basics, protocols, complex.
  Everything else (decimal, money, math/int, regexp, collections/*,
  encoding/*, crypto/*, random, fx, uuid, path) is opt-in via
  `import <name>`.

  Full split: `docs/stdlib-layout.md` §Core vs opt-in.

PACKAGE LAYOUT
  Convention:

    mypackage/
      kai.toml
      main.kai                       # entry point
      lib.kai                        # other modules
      examples/                      # example programs (own kai.toml)
      tests/                         # tests (own kai.toml)

SPEC ARG TO kai BUILD/RUN

  kai build .                        # cwd's package
  kai build ./sub                    # sub-package
  kai build foo.kai                  # single-file mode
  kai build                          # same as `kai build .`

NOT IN KAIKAI
  - Cargo-style `[features]`. No feature flags.
  - npm-style version ranges (`^1.2`). Pin exactly.
  - Build scripts (`build.rs`). No code runs at build time outside
    the compiler.
  - C-style header includes. Each file's import is explicit.
  - Re-exports inside the same package — files merge automatically.

SEE ALSO
  kai info syntax, docs/library-mode.md, docs/stdlib-layout.md
