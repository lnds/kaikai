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
kai fetch                         # resolve + lock
kai install <spec> [--force]      # build a package, install its binary
                                  #   + any [completions] it declares
kai install --list                # what this prefix has installed
kai update [<name>]               # refresh deps
kai show                          # dump parsed manifest
kai migrate [<file>] [--write]    # migrate source across an edition bump
```

`kai install <spec>` is the `cargo install` / `go install`
analogue: it builds the package and drops its binary in
`$KAIKAI_HOME/bin` (default `~/.kaikai/bin`), already on `PATH`.
The `<spec>` is `.` for the cwd's package or a git source with an
optional `@<ref>`; the binary is named from the manifest's `name`,
not the URL. Replacing an installed binary needs `--force`, names
the toolchain ships are refused, and a library — having no entry
point — is turned away rather than failing at link time. A
`[completions]` table gets its scripts installed too (below).

`kai install` with no argument still resolves dependencies, the
behaviour that is now `kai fetch`, and warns that it does. That
form changes meaning at the next edition boundary.

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
is a manifest error: `kai fetch` reports it and exits non-zero
rather than locking zero entries.

## Native sources (`[native]`)

A package binding C declares its shim in the manifest; consumers
never pass it by hand:

```text
[native]
sources = ["c/term_shim.c"]   # vendored C, relative to this package
include = ["c"]               # -I dirs for compiling those sources
libdirs = ["lib"]             # -L dirs at link, relative or absolute
libs = ["sqlite3"]            # system libraries, -l<name> at link
pkg-config = ["libpq"]        # --cflags for the sources, --libs at link
```

Honoured by every verb (`build`/`run`/`test`/`install`), on both
backends, transitively: if A and B depend on the same shim-binding
package, the shim compiles and links once. Sources are vendored and
always work; `libs` name system libraries that must exist on the
consumer's machine. A library off the default search path is found
through `libdirs` (paths like `include`: relative to the package,
absolute allowed) or `pkg-config`, queried at build time for each
name; a missing `pkg-config` (or `$PKG_CONFIG`) or an unknown name
fails the build naming the package and the `.pc`. Declarative only —
no build scripts, no free-form flags. `CFLAGS` keeps precedence as the
escape hatch; `KAI_NATIVE_DEPS=0` disables the channel.

A package binding several libraries keys a table by module instead,
so a consumer needs only the libraries of the modules it imports:

```text
[native.sqlite]               # module `sqlite` of this package
sources = ["c/sqlite_shim.c"]
libs = ["sqlite3"]

[native."db.pg"]              # a nested module path is quoted
sources = ["c/pg_shim.c"]
pkg-config = ["libpq"]
```

A module table takes the same keys as `[native]` and joins a build
only when the build imports that module, directly or transitively.
The module path is relative to the package (`[native.sqlite]` for its
`sqlite.kai`). A bare `[native]` still applies to every build of the
package, and its `include`/`pkg-config` reach the module tables'
sources too.

## JavaScript module (`[js]`)

A package built with `--target=wasm32-browser` that needs JavaScript which
is not a global names its own ES module; `kai build` copies it beside the
page and its exports win over `globalThis` when the host resolves an
`extern "js"` path (see `kai info ffi`). Only the entry package's module is
used.

```text
[js]
module = "glue.mjs"
```

## Shell completions (`[completions]`)

A package declares the completion scripts it ships; `kai install`
places each one where its shell looks:

```text
[completions]
zsh  = "completions/_foo"      # -> share/zsh/site-functions/_foo
bash = "completions/foo.bash"  # -> share/bash-completion/completions/foo
fish = "completions/foo.fish"  # -> share/fish/vendor_completions.d/foo.fish
```

Paths are relative to the package; destinations sit under
`$KAIKAI_HOME/share`, named from the manifest's `name`. `install.sh`
wires those three directories into the shell's search path once, so
every later `kai install` works with no per-package step; `kai
install` prints the line to add when that wiring is missing, and never
edits a startup file itself.

zsh gets the directory **prepended** to `fpath`: appended, a stock
function of the same name still wins (zsh's `_mh` claims `mark` and
suppresses even file completion when MH is absent).

Declarative only, like `[native]`: three shells, a fixed destination
each, no hooks. A path escaping the package or naming a missing file
is skipped with a warning and the binary still installs. `kai upgrade`
carries these directories across its `share/` wipe.

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
import mathlib                    # whole package: its pub names, bare
import mathlib.vec                # specific module
import mathlib as m               # rename: `m.add` only, nothing bare
import mathlib.{add, mul}         # selective: only `add` and `mul`
```

An import makes names visible only in the file that writes it. A name
another file imports, or one a module you import imports in turn, is
not in scope here: a bare use is an error (`` `add` is not imported in
this file ``) whose help names the import to add. A module's names
shadow a core name of the same spelling only in the files that import
it.

Imports are RESOLVED through `kai.toml` — the name on the right of
`import` must appear as a dep (or be a stdlib name).

An import the file never refers to — neither as a qualifier (`m.f`)
nor through any name it exports — is a warning (`unused import \`m\``);
it becomes an error at the Orongo edition. An import that only brings
impls into scope is not reported.

Files load into one unit, so a module one file imports is loaded for
all of them, and another file can name it qualified (`m.f`) without
importing it. When such a file uses a module that enters the unit only
through an import its own file never uses, that import is not reported
as unused: the warning names the file that needs the module (`import
\`m\` is not used in this file, but \`other.kai\` needs it loaded`), and
the import belongs there. Removing every import reported as unused, all
at once, keeps the program building.

## Import cycles

Modules of one package may import each other. A package is the
directory tree under one `kai.toml`; the files stay separate modules.

```text
# even.kai                      # odd.kai
import odd                      import even
pub fn is_even(n: Int) : Bool   pub fn is_odd(n: Int) : Bool
  = if n == 0 { true }            = if n == 0 { false }
    else { is_odd(n - 1) }          else { is_even(n - 1) }
```

The modules of a cycle are typed together: every top-level signature
(private ones included) is explicit, so all of them are collected
before any body is read, and a body sees the whole cycle. Mutual tail
calls across the files stay jumps on both backends. Editing one member
rebuilds the cycle and the modules that import it, and no others.

A cycle is an error when it crosses packages, when no `kai.toml`
encloses the files, or when it runs through the stdlib. A cycle of
constants (`A = B + 1` in one file, `B = A + 1` in another) is an
error: a constant has no storage, so reading one that reaches itself
never finishes.

## Visibility

```kaikai
pub fn add(a: Int, b: Int) : Int = a + b      # exported
fn helper() : Int = 42                         # private to file

fn main() : Int = add(helper(), 1)
```

`pub` marks every top-level form the same way — `fn`, `type` (which
also exports its variant constructors), `effect`, `protocol`, `unit`,
`axiom` and `const`. A `pub const` is read from an importing module
qualified (`lib.LIMIT`), unqualified, or in match-pattern position.

`pub(pkg)` takes the same forms and exports to the package alone:

```text
pub(pkg) fn helper(n: Int) : Int = n * 2       # every module under this kai.toml
pub fn api(n: Int) : Int = helper(n)            # what other packages see
```

A module of the same package reads a `pub(pkg)` name like a `pub` one,
and so do the files under the package's `tests/`; a module of another
package gets an error naming the declaration (`` `helper` is `pub(pkg)`
in module `lib`, which belongs to another package ``), bare or
qualified. A `pub` signature cannot expose a `pub(pkg)` type, effect or
protocol: the validator rejects the leak, since other packages read the
signature. A `pub(pkg)` item needs no `#[doc]`, and `kai doc` leaves it
out. It needs a package: in a file under no `kai.toml` it is an error.

Private items cannot be imported by other packages. Public items in
internal packages do NOT leak to consumers if the public package
re-exports them through a public surface; the typer's pub-leak
validator rejects code that would leak a private type or effect
through a `pub` signature.

## Stdlib is auto-loaded

The Hanga Roa "core" set is loaded automatically — no import
needed for: Stdin/Stdout/Stderr, File, Env, Console, Option, Result,
List ops, Array, String, Char, Int/Real basics, protocols, complex.
Everything else (decimal, money, math/int, math/bigint, regexp,
collections/*, encoding/*, crypto/*, random, fx, uuid, path) is
opt-in via `import <name>`.

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

The sources may sit in a subdirectory instead, with `entry` pointing
into it. The package's modules then resolve from the directory of the
entry for every file the package owns and for every package that
depends on it, and `kai test` runs the `tests/` beside `kai.toml` as
well as one beside the entry:

```text
mypackage/
  kai.toml                       # entry = "src/main.kai"
  src/
    main.kai                     # import mypackage.core.math
    mypackage/core/math.kai
    mypackage/core/math_test.kai # runs with `kai test`
  tests/
    math_test.kai                # import mypackage.core.math
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
