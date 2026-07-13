# packages

`kai.toml`, imports, visibility.

## Description

A kaikai package is a directory with a `kai.toml` manifest. The
package name in the manifest is what other packages import.

Files inside a package are merged at build time. Each file declares
what it imports from other packages and what it exports.

## Driver commands

```text
kai init <name>                   # create a kai.toml in cwd
kai add <source>[@<ref>]          # add a dep
kai install                       # resolve + lock
kai update [<name>]               # refresh deps
kai show                          # dump parsed manifest
kai migrate [<file>] [--write]    # migrate source across an edition bump
```

## Manifest

```text
name = "mathlib"                  # top-level, not under a [section]
version = "0.1.0"
entry = "main.kai"                # optional; defaults to main.kai

[dependencies]
jsonlib = { source = "github.com/x/jsonlib", ref = "v1.2.0" }
utilslib = { path = "../utilslib" }
```

Dependency forms:

- **Git**: `{ source = "<url>", ref = "<tag|branch|sha>" }` — the
  canonical form `kai add <url>@<ref>` writes. `{ git = ..., tag = ... }`
  is accepted as an alias for the same. The `ref` is pinned to a commit
  SHA in `kai.lock` for reproducible builds.
- **Local path**: `{ path = "../relative/or/absolute" }` — resolved
  through the manifest directly, not locked.

An unrecognised dependency table (neither `path` nor `source`/`ref`)
is a manifest error: `kai install` reports it and exits non-zero
rather than locking zero entries.

## Migrating across an edition

An edition bump can change the language surface (renames, signature
shifts). `kai migrate` rewrites a package's source from one edition's
surface to the next, applying each mechanical change. It parses, walks
the AST, and re-emits through the formatter — never a textual pass —
so it does not corrupt code, and it never emits source that fails to
parse.

```text
kai migrate src/main.kai            # dry-run: print migrated source, write nothing
kai migrate --write src/main.kai    # apply the rewrite in place
kai migrate --from hanga-roa --to orongo src/main.kai
```

- **Dry-run by default** — prints the migrated source to stdout and
  touches no files. Pass `--write` to rewrite in place. Running
  `--write` twice is a byte-stable no-op (idempotent).
- **Editions default** to the package/repo edition as `--from` and its
  successor as `--to`. Today only `hanga-roa -> orongo` has a rule set.
- **Un-migratable changes are reported, not dropped** — a change with
  no automatic rule prints a `manual: <line>:<col> — …` pointer to
  stderr so you fix it by hand.

## Imports

```text
import mathlib                    # whole package
import mathlib.vec                # specific module
import mathlib as m               # rename
import mathlib.{add, mul}         # selective: only `add` and `mul`
```

Imports are RESOLVED through `kai.toml` — the name on the right of
`import` must appear as a dep (or be a stdlib name).

## Visibility

```kaikai
pub fn add(a: Int, b: Int) : Int = a + b      # exported
fn helper() : Int = 42                         # private to file

fn main() : Int = add(helper(), 1)
```

Private items cannot be imported by other packages. Public items in
internal packages do NOT leak to consumers if the public package
re-exports them through a public surface; the typer's pub-leak
validator rejects code that would leak a private type or effect
through a `pub` signature.

## Stdlib is auto-loaded

The Hanga Roa "core" set is loaded automatically — no import
needed for: Stdin/Stdout/Stderr, File, Env, Console, Option, Result,
List ops, Array, String, Char, Int/Real basics, protocols, complex.
Everything else (decimal, money, math/int, regexp, collections/*,
encoding/*, crypto/*, random, fx, uuid, path) is opt-in via
`import <name>`.

Full split: `docs/stdlib-layout.md` §Core vs opt-in.

## Package layout

Convention:

```text
mypackage/
  kai.toml
  main.kai                       # entry point
  lib.kai                        # other modules
  examples/                      # example programs (own kai.toml
                                 #   per example, or sibling files
                                 #   that import the parent package)
  tests/                         # test files — `kai test .` auto-runs
                                 #   each `*.kai` here; imports
                                 #   resolve to the parent's modules
                                 #   via sibling resolution (no
                                 #   manifest needed inside tests/)
```

## Spec arg to kai build / run

```text
kai build .                        # cwd's package
kai build ./sub                    # sub-package
kai build foo.kai                  # single-file mode
kai build                          # same as `kai build .`
```

## NOT IN KAIKAI

- Cargo-style `[features]`. No feature flags.
- npm-style version ranges (`^1.2`). Pin exactly.
- Build scripts (`build.rs`). No code runs at build time outside
  the compiler.
- C-style header includes. Each file's import is explicit.
- Re-exports inside the same package — files merge automatically.

## See also

`kai info syntax`, `docs/library-mode.md`, `docs/stdlib-layout.md`
