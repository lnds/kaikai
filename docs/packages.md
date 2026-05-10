# Package management

> **v1 status (2026-05-09):** the v1 surface in this document is
> live (issue #405). Manifest parsing, lockfile, cache, git-based
> resolution with transitive deps + minimum-version selection,
> and the full `kai init` / `add` / `install` / `update` /
> `show` command surface ship together. Out-of-scope items are
> listed at the end and pinned to follow-up issues.

kaikai uses a Go-style package manager: a `kai.toml` manifest
declares dependencies, the driver resolves them on demand, and
each dependency exposes `pub` modules that the consumer imports
under the dep's declared name.

## Manifest format (`kai.toml`)

The minimal manifest:

```toml
name = "myapp"
version = "0.1.0"

[dependencies]
greet = { path = "../lib_greet" }
manutara = { source = "github.com/lnds/manutara", ref = "v0.1.0" }
kohau = "github.com/lnds/kohau@v0.2.0"
```

Three forms are accepted for a dependency value:

1. **String shorthand** — `"<source>@<ref>"`. Equivalent to the
   inline-table form below; the `@` splits at the first
   occurrence.
2. **Inline table with `source` + `ref`** — explicit form, useful
   when the source URL itself contains `@`.
3. **Local path override** — `{ path = "../local-thing" }`. The
   driver resolves the path relative to the manifest's directory
   and injects an absolute `--path` flag for it. Useful for
   workspace-style development before publishing.

Top-level keys recognised by the v1 parser:
`name`, `version`, `[dependencies]`. Any other top-level keys are
ignored (forwards-compatible with future extensions).

The manifest is parsed by `stdlib/encoding/toml.kai` (a hand-
written subset decoder; see the module header for the supported
subset). The driver shells out to `tools/kai-pkg` (a
kaikai-compiled CLI) for everything that touches the manifest.

## Driver commands

### `kai init <name>`

Writes a stub `kai.toml` in the current directory with the given
package name, version `0.1.0`, and an empty `[dependencies]`
section. Errors out if `kai.toml` already exists.

```sh
$ kai init myapp
kai-pkg: wrote kai.toml for package 'myapp'
```

The package name must match the grammar

```
package_name := [a-z][a-z0-9_-]*
```

i.e. lowercase ASCII alphanumerics plus `_` and `-`, starting
with a letter. The same shape is used by Cargo, Go modules, and
Hex.pm and avoids every downstream pitfall: spaces and slashes
break import resolution, `@` collides with the `kai add foo@v1`
syntax, leading dashes parse as flags, and `..` enables path
traversal in the cache layout. `kai init` rejects names that
fall outside this grammar with a non-zero exit and leaves
`kai.toml` untouched (issue #419).

### `kai install`

Reads `kai.toml` from the current directory (or the nearest
ancestor) and resolves dependencies. Local-path deps are reported
as resolved; git-source deps are cloned (via
`git clone --depth 1 --branch <ref>`) into the package cache, the
HEAD commit is captured, and `kai.lock` is rewritten with each
package's `(name, source, ref, sha)`. Idempotent: a second run
with a populated cache prints `cached <name>` instead of
`fetching`.

```sh
$ kai install
kai-pkg: resolving 2 dependency(ies)
  resolved local: greet -> ../lib_greet
  fetching manutara (github.com/lnds/manutara @ v0.1.0)
kai-pkg: wrote kai.lock with 1 entry(ies)
```

`kai run` and `kai build` auto-run `kai install` when they
detect a manifest with git deps but no `kai.lock` (or when a
pinned cache directory has gone missing). Users do not need to
run `install` manually before the first `run`.

### `kai add <source>[@<ref>]`

Append a dependency to `kai.toml` and refresh the lockfile in
one step. Source forms:

```sh
kai add github.com/lnds/manutara@v0.1
kai add github.com/lnds/kohau                # ref defaults to "main"
kai add /abs/path/to/local-bare-repo@v1.0    # local clone for testing
```

The package name is derived from the last `/`-separated segment
of the source URL.

`kai add` is atomic with respect to manifest mutation: the source
is cloned first, and `kai.toml` is only updated if the clone
succeeds. A failed clone (bad URL, missing ref, network error)
exits non-zero and leaves `kai.toml` and `kai.lock` untouched, so
the working tree never drifts into an inconsistent state.

### `kai update [<name>]`

Re-fetch dependencies and refresh `kai.lock`. With no argument,
all deps are refreshed; with one argument, only that package is
dropped from the lock and re-resolved.

### `kai show`

Reads `kai.toml` and prints its parsed contents. Useful for
debugging the parser; not part of the long-term command surface.

### `kai run` / `kai build` (auto-resolution)

`kai run` and `kai build` walk up from the entry file looking
for a `kai.toml`. If found, the driver:

1. Reads both manifest and `kai.lock`.
2. If git deps are declared but the lock is missing or any
   pinned cache directory has gone missing, runs `kai install`
   first (transparent auto-install).
3. Invokes `kai-pkg paths` to emit one `<name>\t<abs-path>` line
   per local-path dep AND every entry in the lockfile (so
   transitive git deps are included automatically).
4. Injects `--path <abs>` flags for kaic2.

No flag is needed on the user's part; the manifest is the single
source of truth.

```sh
$ kai run examples/packages/local_path/app/main.kai
Hello, kaikai!

$ kai run examples/packages/transitive/main.kai
  fetching util (... @ v0.1.0)
  fetching greet (... @ v0.1.0)
kai-pkg: wrote kai.lock with 2 entry(ies)
Hello from git, transitive! (shouted)
```

## Architecture

The package manager is split across three components:

1. **`stdlib/encoding/toml.kai`** — hand-written TOML decoder for
   the manifest / lockfile subset. Exposed publicly so user code
   can also parse TOML; **not** loaded by `stage2/Makefile`
   (kaic2 self-host doesn't depend on it). Style mirrors
   `stdlib/encoding/json.kai`.
2. **`tools/kai-pkg/main.kai`** — kaikai-compiled CLI that owns
   `kai.toml` reads/writes and dependency resolution. The driver
   auto-builds it on first use (dev layout) or relies on a
   pre-built binary at `libexec/kaikai/kai-pkg` (installed
   layout).
3. **`bin/kai`** — POSIX shell driver. `find_manifest_dir` walks
   up looking for `kai.toml`; `manifest_path_flags` consumes
   `kai-pkg paths` and emits absolute `--path` flags for kaic2.
   `cmd_init` / `cmd_install` / `cmd_show_pkg` are thin
   shell-outs to the kai-pkg binary.

This split keeps the parser reusable as a stdlib component, gives
the package manager a real type system to work with (instead of
shell), and avoids touching kaic2 itself.

## Cache layout

```
$KAIKAI_CACHE / $XDG_CACHE_HOME/kai/pkg /
~/.cache/kai/pkg (Linux) / ~/Library/Caches/kai/pkg (macOS)
  └── <slug-of-source>/
      └── <sha>/
          └── (clone of the repo pinned at that commit)
```

Slugification: protocol prefixes (`https://`, `ssh://`,
`file://`, `git@`) and leading `/` are stripped; remaining
filesystem-unfriendly characters become `_`. Path separators
inside the source are kept so the cache mirrors upstream
structure (`github.com/lnds/manutara/abc123def456.../`).

The leaf is the resolved commit SHA, not the user-facing ref
(issue #421). Content addressing means each entry's name matches
its contents: branch refs that move upstream do not overwrite
older cached SHAs, and a lockfile-pinned SHA always finds its own
tree on disk regardless of where the branch tip is now.

The `cache` field is **not** written to `kai.lock` — it is
derived from `KAIKAI_CACHE_ROOT + source + sha` at install time.
This keeps the lockfile reproducible across machines with
different cache roots.

## Lockfile format

`kai.lock` is generated TOML with one `[[package]]`
array-of-tables entry per resolved git dependency. Local-path
deps are not pinned (they have no SHA).

```toml
# kai.lock — generated by kai-pkg. Do not edit by hand.

[[package]]
name = "manutara"
source = "github.com/lnds/manutara"
ref = "v0.1.0"
sha = "abc123def456..."
```

The lockfile is canonical: parsing then re-encoding via
`toml_round_trip` produces a byte-identical file (the
`lockfile_reproducibility` fixture asserts this across two
clean caches).

## Minimum-version selection

For each unique `source`, the resolver picks the **maximum** of
all declared `ref` values (Go MVS). This handles diamond deps
where two transitive deps require different versions of the
same module: the resolution promotes everyone to the higher
version. Comparison is lexicographic — for semver tags
(`v0.1.0` < `v0.2.0` < `v0.10.0` ⚠️) this matches semver order
when zero-padding is consistent (`v0.10.0` would lose to
`v0.2.0` lexicographically; bump the minor before the tag is
double-digit if you care). A full semver parser is a follow-up.

## Reference fixtures

Four fixtures live under `examples/packages/`:

| Fixture | Demonstrates |
|---|---|
| `local_path/` | `{ path = "..." }` override; relative path resolved against the manifest dir. |
| `simple_dep/` | Single git-source dependency cloned to cache, SHA pinned in lock. |
| `transitive/` | `util` depends on `greet`; both fetched, lockfile contains both, transitive `--path` injection works. |
| `lockfile_reproducibility/` | Two clean `kai install` runs from the same manifest produce byte-identical `kai.lock`. |

Git-based fixtures depend on bare repos generated by
`tests/fixtures/git-fixtures/setup.sh`; manifest paths render
from `*.toml.template` via `examples/packages/render-fixtures.sh`.
This setup keeps fixtures self-contained and free of external
network dependencies.

```sh
# One-time setup:
tests/fixtures/git-fixtures/setup.sh
examples/packages/render-fixtures.sh

# Then any fixture works:
bin/kai run examples/packages/transitive/main.kai
```

## Out of scope (post-1.0)

- **Registry / publish / search**.
- **HTTP-based sources** beyond git (raw archives, etc.).
- **Workspace mode** (multi-package monorepos with shared lock).
- **Build-time scripts** / generated code.
- **Pre-`kai install` SHA-256 of the cloned tree** for tamper
  detection — the git SHA already pins the content, but a
  post-clone hash would catch local cache corruption.
- **Semver-aware ref comparison**. Current MVS is lexicographic
  on the ref string.

## Publishing a package — migration guide

For an existing kaikai project that wants to become a publishable
package others can `kai add`, the migration is mostly file layout.

### Recommended layout

```
mypkg/
├── kai.toml              # manifest of the package
├── README.md
├── LICENSE
├── lib/                  # importable code; this is what consumers see
│   ├── core.kai
│   ├── parser.kai
│   └── ...
└── examples/             # demos using the package locally
    ├── kai.toml          # manifest with `mypkg = { path = ".." }`
    └── basic/
        └── main.kai
```

The resolver looks for the package's importable code under `lib/`.
When a consumer runs `kai add github.com/<owner>/mypkg@<ref>`, the
driver injects `<cache>/mypkg/<sha>/lib` into the compile path, so
`import mypkg.core` resolves to `<cache>/mypkg/<sha>/lib/core.kai`.

If your project today has lib + demos at the repo root (a common
pre-package-manager pattern), the migration is:

1. Move every `.kai` file that is part of the public surface into
   `lib/`.
2. Move demos / showcases into `examples/<name>/main.kai`.
3. Write a `kai.toml` at the repo root with `name`, `version`, and
   any dependencies the library itself needs.
4. Write `examples/kai.toml` declaring the package as a local-path
   dependency: `mypkg = { path = ".." }`. This lets your demos
   compile during development against the in-tree code, using the
   same import paths an external consumer will use.
5. Verify a demo compiles: `cd examples/basic && kai run main.kai`.
6. Tag the release: `git tag v0.1.0 && git push origin v0.1.0`.
7. Announce the source URL: `github.com/<owner>/mypkg@v0.1.0`.

Source code does **not** change — `import mypkg.core` works
identically pre- and post-migration. Only the file layout shifts.

### Internal imports inside the package

A file in `lib/parser.kai` that needs `lib/core.kai` writes
`import mypkg.core` — same as an external consumer would. The
package always names itself by the manifest `name`. This keeps
your code uniform: no special "internal" import paths.

### Versioning

Pre-1.0, follow `0.MINOR.PATCH` with breaking changes bumping
MINOR (cz convention). Tag every release. Consumers pin to a tag
(`@v0.2.0`), a branch (`@main`), or a SHA (`@abc1234`). Tags are
the convention; branches and SHAs are escape hatches.

## Consuming a package — quick reference

### New project from scratch

```sh
mkdir myapp && cd myapp
kai init myapp                                      # writes kai.toml
kai add github.com/<owner>/uira@v0.1.0              # adds dep + writes lock
echo 'import uira.core
fn main() = println("hi")' > main.kai
kai run main.kai                                    # auto-resolves
```

`kai run` and `kai build` walk up from the entry file looking for
`kai.toml`. When found, they read `kai.lock` (running `kai install`
first if it is missing or stale) and inject every cached
dependency's `lib/` directory into the compile path.

### Local-path dependency for cross-development

Editing `uira` and `myapp` simultaneously? Skip the cache by
using a path override:

```toml
# myapp/kai.toml
[dependencies]
uira = { path = "../uira" }
```

`kai run main.kai` compiles against the live source tree of
`../uira/lib/`. Edits in `uira` show up immediately in `myapp`
without reinstall, retag, or recommit.

When ready for release, swap the path entry for a git source
entry. Code does not change; only the manifest does.

### Updating

```sh
kai update              # refetches all deps to latest matching ref
kai update uira         # refetches just uira
kai add github.com/<owner>/uira@v0.2.0    # explicit version bump
```

`kai update` rewrites `kai.lock`. Commit the new lock to share
the resolution with your team.

### Transitive resolution

If `uira` depends on `kupenga`, your project pulls both
automatically. Minimum-version selection picks the highest
required ref string lexicographically across the graph. Pin
specific versions in your own manifest to override.

## See also

- `docs/effects-stdlib.md` §`File` and §`Process` — the effects
  the package manager rides on.
- `stdlib/encoding/toml.kai` (header) — supported TOML subset.
- `tools/kai-pkg/main.kai` (header) — CLI subcommand catalog.
- `examples/packages/local_path/` — minimal worked example of a
  package + consumer pair, useful as a template.
- `examples/packages/transitive/` — multi-level dependency
  resolution example.
