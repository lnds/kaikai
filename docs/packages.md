# Package management

> **v1 status (2026-05-09):** the v1 surface in this document
> ships in incremental commits on issue #405. Local-path deps,
> manifest parsing, driver auto-resolution, and the
> `kai init` / `kai install` / `kai show` driver commands are
> live. **Git-based deps, lockfile (`kai.lock`), cache layout,
> and minimum-version selection are deferred** — see
> *Out of scope (this commit)* below for the inventory.

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

### `kai install`

Reads `kai.toml` from the current directory (or the nearest
ancestor) and resolves dependencies. v1 reports local-path deps
as resolved and emits a deferred-note for git deps. Idempotent.

```sh
$ kai install
kai-pkg: resolving 2 dependency(ies)
  resolved local: greet -> ../lib_greet
  deferred git: manutara (github.com/lnds/manutara @ v0.1.0) — git fetch lands in a follow-up
```

### `kai show`

Reads `kai.toml` and prints its parsed contents. Useful for
debugging the parser; not part of the long-term command surface.

### `kai run` / `kai build` (auto-resolution)

`kai run` and `kai build` walk up from the entry file looking for
a `kai.toml`. If found, the driver invokes `kai-pkg paths` to
emit one line per local-path dependency, then injects `--path
<abs>` flags for kaic2. No flag is needed on the user's part; the
manifest is the single source of truth.

```sh
$ kai run examples/packages/local_path/app/main.kai
Hello, kaikai!
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

## Out of scope (this commit)

The following items are recognised by the design but deferred to
follow-up commits:

- **Git-based deps**: `{ source = "...", ref = "..." }` and the
  `"<source>@<ref>"` shorthand. The parser recognises them and
  `kai install` reports them as deferred; the actual `git clone
  --depth 1 --branch <ref>` lands once the cache layout is
  finalised.
- **`kai.lock`**: deterministic dependency pinning (full git SHA
  + tarball SHA-256 per package). Required for reproducible
  builds; not needed for local-path deps.
- **Cache layout**: `~/.cache/kai/pkg/` with content-addressed
  directories. Plan: `$KAIKAI_CACHE` →
  `$XDG_CACHE_HOME/kai/pkg` → `~/.cache/kai/pkg` (Linux) /
  `~/Library/Caches/kai/pkg` (macOS).
- **Transitive resolution + minimum-version selection** (Go MVS).
  v1 reads only the immediate manifest.
- **`kai add <source>`** and **`kai update [<pkg>]`**. v1 expects
  manifest edits by hand; once `kai install` covers git fetches,
  `add` becomes a thin wrapper over manifest mutation + install.
- **Registry / publish / search**, HTTP-based sources beyond git,
  workspace mode (multi-package monorepos), build-time scripts.
  All explicitly out of scope per the lane brief.

## Reference fixture

`examples/packages/local_path/` is the canonical end-to-end
fixture for this commit. It contains:

```
examples/packages/local_path/
  lib_greet/
    greet.kai          # pub fn greet(name) : String
  app/
    kai.toml           # greet = { path = "../lib_greet" }
    main.kai           # import greet; print(greet.greet("kaikai"))
    main.out.expected  # "Hello, kaikai!\n"
```

`bin/kai run examples/packages/local_path/app/main.kai` prints
`Hello, kaikai!` — round-trip-verified at every commit on the
lane.

## See also

- `docs/effects-stdlib.md` §`File` and §`Process` — the effects
  the package manager rides on.
- `stdlib/encoding/toml.kai` (header) — supported TOML subset.
- `tools/kai-pkg/main.kai` (header) — CLI subcommand catalog.
