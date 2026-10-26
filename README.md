# kaikai

[![km score](https://lnds.github.io/kaikai/km-score.svg)](https://kimun.tools)

A **functional**, **statically typed** programming language that compiles to
**native code** via LLVM.

- **Algebraic effects** as a first-class primitive (Effekt-style + inference).
- **Elixir-style pipelines**.
- **Memory** without a global GC or borrow checker: Perceus (compile-time
  optimised RC) + isolated fibers (BEAM-style).
- **Portable bootstrap**: 3-stage compiler (C → kaikai-minimal → full kaikai).

## Status

Pre-1.0, **Hanga Roa** edition. The language is self-hosting and
runs the full toolchain today; it is unstable until the Orongo
edition lands (the 1.0 label is postponed indefinitely), and breaking
changes still land within Hanga Roa — see `docs/editions.md`.

- **Bootstrap** (landed): `stage2/kaic2`, the self-hosted compiler in
  full kaikai, is built by the previous release (`make selfhost`
  proves it is a fixed point); from a bare `cc`, it boots from a
  frozen C seed. Stages 0 and 1 are frozen.
- **Backend** (landed): the in-process libLLVM **native** backend
  is the default; the C-direct backend is the bootstrap path and
  parity oracle (`KAI_BACKEND=c`).
- **Language + runtime** (landed): algebraic effects + handlers,
  fibers / actors / structured concurrency, Perceus RC, typed
  holes (`?` / `?name` with `--holes-json`), units of measure,
  single-dispatch protocols.
- **Tooling** (landed): `kai build` / `run` / `test` / `fmt` /
  `lsp` / `doc`, plus a package manager (`kai add`).
- **Stdlib** (landed): `core`, `math`, the effect catalog, and the
  `Map` / `HashMap` / `Set` / `HashSet` / `Queue` / `Stack`
  collection family — inventory in `docs/stdlib-layout.md`.

See `docs/roadmap.md` for milestone state and `docs/design.md` for
the design context.

# Two ways in, and which one you want

There are two completely separate paths, for two different people. Pick
the one that matches you — they do not overlap, and you almost certainly
want the first.

| You want to… | Path | Needs |
| --- | --- | --- |
| **Use kaikai** (write and run programs) | **A. Install a prebuilt `kai`** | nothing — the binary is self-contained |
| **Hack on the compiler itself** (change the language, the runtime, the codegen) | **B. Build the compiler from source** | a C compiler + LLVM dev libraries |

If you just want to write kaikai, go to **A** and ignore everything under
**B**. The LLVM / `llvm-config` / `make` machinery in **B** is *only* for
rebuilding the compiler — an installed `kai` needs none of it.

---

# A. Install — to use kaikai

Get a prebuilt `kai` on **macOS arm64** (Apple Silicon) or **Linux x86-64**
(and on Windows through WSL2, see below). The binary is
**self-contained**: libLLVM is linked in, so there is **no system LLVM,
no `make`, and no toolchain to install**. This is the path for everyone
who is not changing the compiler.

**curl | sh** (rustup-style, no Homebrew required):

```sh
curl -fsSL https://raw.githubusercontent.com/kaikailang-org/kaikai/main/install.sh | sh
```

This downloads the latest release, verifies its SHA-256, installs to
`~/.kaikai/`, and adds `~/.kaikai/bin` to your `PATH`. Open a new shell
(or `export PATH="$HOME/.kaikai/bin:$PATH"`) and run `kai --version`.
Self-update later with:

```sh
kai upgrade
```

**Homebrew** (coexists with the curl installer):

```sh
brew install kaikailang-org/kaikai/kaikai
brew upgrade kaikai     # to update
```

A `kai upgrade` on a Homebrew install defers to `brew upgrade` rather
than touching the Cellar.

> Released tarballs cover **darwin-arm64**, **linux-x86_64** and
> **linux-aarch64**. The Homebrew formula is macOS-only. On any other
> platform (macOS Intel) the installer reports the gap rather than
> failing on a 404 — build the compiler from source there (path **B**).
>
> The Linux tarballs are built on a current distribution and need its
> system libraries or newer: glibc 2.38 and a libstdc++ providing
> `GLIBCXX_3.4.30`. On an older system the installer stops, says what it
> found and installs nothing; build from source there too.
>
> **Windows:** there is no native build. Use
> [WSL2](https://learn.microsoft.com/windows/wsl/install) with an
> x86-64 Linux distribution and run the curl installer above inside it;
> the linux-x86_64 release works there unchanged.

## Quickstart

Five short programs that cover the language's main shapes:

```sh
kai run examples/quickstart/01_hello.kai        # hello world
kai run examples/quickstart/02_fizzbuzz.kai     # sum types + match
kai run examples/quickstart/03_calculator.kai   # recursive AST + match
kai run examples/quickstart/04_effect.kai       # custom effect + handler
kai run examples/quickstart/05_concurrent.kai   # cooperative fibers
```

Every file is ~30 lines, runnable as-is, and explained in its own
header comment. Read them in order — each one introduces one new
concept (sum types, then recursion + match, then algebraic effects,
then fibers).

---

# B. Build from source — to regenerate the compiler

**You only need this section if you are changing kaikai itself** — the
language, the runtime, the codegen — or running on a platform the
installer does not yet ship a binary for. **If you installed `kai` in
section A, skip all of this.** None of the LLVM / `make` / bootstrap
machinery below is needed to *use* the language; it is the toolchain for
*rebuilding the compiler*.

## Prerequisites (for building from source only)

- **C compiler** (cc/gcc/clang)
- **LLVM** development headers and libraries (CI and the released
  binaries use LLVM 22; the build is version-agnostic via `llvm-config`):
  ```sh
  sudo apt install llvm-dev libzstd-dev
  ```
  Other distributions: install the `llvm-devel` package (Fedora) or
  the equivalent for your distro. The build locates LLVM through
  `llvm-config`, so any reasonably recent version on `PATH` works.

  No system LLVM at hand? `make llvm-prebuilt` downloads the static
  libLLVM the released binaries link (macOS arm64 and Linux x86_64),
  verifies its checksum and unpacks it under `stage0/third_party/llvm`.
  On any other host, or after editing `mk/llvm.mk`, it compiles the
  same archives from LLVM's source instead (needs `cmake` and `ninja`,
  ~25 min). Point the build at it:
  ```sh
  make llvm-prebuilt
  make KAI_LLVM=1 LLVM_CONFIG="$PWD/stage0/third_party/llvm/build/bin/llvm-config" kaic2
  ```

## Building the compiler

Everything builds from the repo root with `make`:

```sh
make all       # kaic1, kaic2 (built by the previous release), bin/kai
make kaic2-fast  # after editing stage2/compiler/*.kai: one hop, changed modules only
make tier0     # pre-commit sanity, selfhost included
make selfhost  # proves kaic2 compiled by itself is a fixed point
```

`kaic2` is built by the previous release's compiler, downloaded once per
release (`KAIC_BOOT=release`, the default). With only a C compiler and
no network, boot from the frozen C seed a release pins in a
`bootstrap-seed-v*` tag:

```sh
make kaic2 KAIC_BOOT=seed     # add KAI_LLVM=1 where libLLVM is available
bin/kai run examples/phase4/hello.kai
```

`make selfhost` is the fixed-point proof that the compiler can rebuild
itself byte-for-byte — the defining property of a self-hosted compiler.
`docs/build-system.md` maps the rest of the build.

## Usage

The `kai` binary (`bin/kai`) drives `kaic2` + `cc`. Run a program:

```sh
kai run examples/phase4/collatz.kai
#  longest collatz in 1..100 is n=97 with length 119
```

Build to a standalone native binary:

```sh
kai build examples/phase4/euler1.kai -o euler1
./euler1
#  sum = 233168
```

There are two backends:

- `native` (default) — the in-process libLLVM backend (builds the module
  via the LLVM C API and emits a native object directly; no `.ll` text,
  no `clang`; docs/kir-design.md §7.2). libLLVM is linked into `kaic2`
  (statically in a release), so it runs out-of-the-box with no system
  LLVM. A `kaic2` built without libLLVM (the cc-only bootstrap) degrades
  the default to `c` with a note.
- `--backend=c` / `KAI_BACKEND=c` — the portable C path (`kaic2` emits C,
  linked with `cc`). It is the bootstrap path and the parity oracle,
  supported across the whole corpus.

The `--backend` flag overrides `KAI_BACKEND`, which overrides the
`native` default.

Run the inline test blocks in a file:

```sh
kai test examples/phase4/factorials.kai
#    ok   factorial base cases
#    ok   factorial small values
#
#  2/2 tests passed
```

The stdlib core modules are loaded into every compilation; any other
stdlib module comes in with `import`.

Declare dependencies in a `kai.toml` next to your source:

```sh
kai init myapp                      # writes kai.toml
# edit kai.toml to add dependencies
kai fetch                           # resolve them
kai run src/main.kai                # auto-injects --path for local-path deps
```

See [`docs/packages.md`](docs/packages.md) for the manifest format,
the supported dependency forms (git sources such as
`github.com/owner/repo@v1.0`, local paths), and `kai.lock`.

Two on-CLI references travel with the binary:

```sh
kai info syntax          # one-page cheat sheet of every form kaikai has
kai doc string.split     # a stdlib symbol's signature + documentation
kai doc stream           # a module's pub items, each with a synopsis
kai doc                  # every stdlib module, one synopsis each
```

`kai info` covers language semantics (effects, match, protocols,
units, pipes); `kai doc` reads the `#[doc(...)]` attributes carried by
stdlib source, so it stays in sync with the code it documents.

## Layout

```
stage0/          C bootstrap compiler for kaikai-minimal.
stage1/          kaikai-minimal compiler (self-hosted in kaikai-minimal).
stage2/          Self-hosted compiler in full kaikai (native libLLVM
                 backend, fmt, lsp, doc).
stdlib/          Standard library in kaikai (core, math, effects,
                 collections).
stage2/runtime.h, stage2/runtime/
                 The C runtime (Perceus RC, fibers, M:N scheduler,
                 reactor), shared by both backends.
tools/kai/       Source of the `kai` driver; `bin/kai` is the built
                 binary. Other tools and CI gates live under `tools/`.
examples/        Fixtures, one directory per area; `minimal/` and
                 `phase4/` hold small runnable programs.
demos/           Demo programs, gated by `make demos-no-regression`.
docs/            Design docs and specs (see Documentation below).
tests/           Shell tests of the driver and the formatter.
scripts/         Release and helper scripts (release build, prebuilt
                 libLLVM, JSON-schema validation).
```

## Documentation

Design docs live in `docs/`. The most relevant ones, grouped
by topic:

**Foundations**
- [`docs/design.md`](docs/design.md) — top-level design,
  principles tier list, decisions, roadmap.
- [`docs/kaikai-minimal.md`](docs/kaikai-minimal.md) — the
  minimal subset stage 0 compiles, with grammar and operator
  precedence.

**Per-stage**
- [`docs/stage0-design.md`](docs/stage0-design.md) — the C
  bootstrap compiler.
- [`docs/stage1-design.md`](docs/stage1-design.md) —
  self-hosted compiler in kaikai-minimal.
- [`docs/stage2-design.md`](docs/stage2-design.md) — the
  definitive compiler, milestones m1–m17.
- [`docs/phase4-design.md`](docs/phase4-design.md) — phase 4
  stdlib and `kai` driver.

**Effects, fibers, actors**
- [`docs/effects.md`](docs/effects.md) — effect-row semantics
  (Doc A).
- [`docs/effects-stdlib.md`](docs/effects-stdlib.md) — stdlib
  effect catalog (Doc B).
- [`docs/effects-impl.md`](docs/effects-impl.md) — CPS
  transform and runtime (Doc C, in progress).
- [`docs/syntax-sugars.md`](docs/syntax-sugars.md) — m7b
  ergonomic sugars (trailing lambdas, `@cap`, `:=`, `var`,
  `a[i]`).
- [`docs/structured-concurrency.md`](docs/structured-concurrency.md) —
  fibers, nurseries, `Spawn` / `Cancel`.
- [`docs/actors.md`](docs/actors.md) — `Actor[Msg]` effect,
  mailboxes, link/monitor supervision.

**Other**
- [`docs/typed-holes.md`](docs/typed-holes.md) — `?` /
  `?name` design (landed in m10).
- [`docs/proposed-extensions.md`](docs/proposed-extensions.md) —
  catalog of post-MVP language and tooling proposals.

## Contributing

kaikai does not yet have a defined process for incorporating improvements
to the language or the standard library. **If you have an idea, open an
issue in the official repository
([kaikailang-org/kaikai](https://github.com/kaikailang-org/kaikai)) first**
to discuss its validity with the team building the language and the standard
packages — please do not start with a pull request for a new feature.

kaikai is **pre-1.0 and unstable until the Orongo edition lands**: an
edition is meant to pin a stable surface *within itself*, but breaking
changes still land within Hanga Roa (see [docs/editions.md](docs/editions.md)),
so treat the language and stdlib as unstable for building on top of.

If your idea is a library, you can **build and publish your own package**
and depend on it from `kai.toml` today — no change to kaikai required. That
is the fastest path to using your idea, and it keeps the standard library
small. See [CONTRIBUTING.md](CONTRIBUTING.md) for the full guidance.

## License

Licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or
  <https://www.apache.org/licenses/LICENSE-2.0>)
- MIT license ([LICENSE-MIT](LICENSE-MIT) or
  <https://opensource.org/licenses/MIT>)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally
submitted for inclusion in the work by you, as defined in the Apache-2.0
license, shall be dual licensed as above, without any additional terms or
conditions.
