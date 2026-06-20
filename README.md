# kaikai

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
(1.0) edition lands — see `docs/editions.md`.

- **Bootstrap** (landed): the 3-stage chain builds from any `cc`.
  `stage1/kaic1` is the self-hosting kaikai-minimal compiler;
  `stage2/kaic2` is the self-hosted compiler in full kaikai
  (`make selfhost` proves it is a fixed point).
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

## Prerequisites

- **C compiler** (cc/gcc/clang)
- **LLVM** development headers and libraries (CI builds against
  LLVM 18; the build is version-agnostic via `llvm-config`):
  ```sh
  sudo apt install llvm-dev libzstd-dev
  ```
  Other distributions: install the `llvm-devel` package (Fedora) or
  the equivalent for your distro. The build locates LLVM through
  `llvm-config`, so any reasonably recent version on `PATH` works.

## Build

Everything builds from the repo root with `make`:

```sh
make all       # stage 0 (C), stage 1 (kaikai-minimal), bin/kai
make test      # runs stage 0, stage 1, and phase 4 demo suites
make selfhost  # proves kaic1 compiled by kaic1 is a fixed point
```

On a fresh checkout, only a C compiler is required:

```sh
cc stage0/*.c -o stage0/kaic0
./stage0/kaic0 stage1/compiler.kai > /tmp/stage1.c
cc /tmp/stage1.c -I stage0 -o stage1/kaic1
bin/kai run examples/phase4/hello.kai
```

## Usage

The `bin/kai` driver wraps `kaic1` + `cc`. Run a program:

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

The driver auto-builds `stage0/kaic0` and `stage1/kaic1` on first use
and prepends every file under `stdlib/core/` to every compilation
(set `KAI_NO_STDLIB=1` to turn this off).

Declare dependencies in a `kai.toml` next to your source:

```sh
kai init myapp                      # writes kai.toml
# edit kai.toml to add dependencies
kai install                         # resolve them
kai run src/main.kai                # auto-injects --path for local-path deps
```

See [`docs/packages.md`](docs/packages.md) for the manifest format,
the supported dependency forms (string shorthand, inline table,
local path), and the v1 status (local-path deps are live; git-based
deps are deferred to a follow-up).

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
bin/             Shell driver (`kai build/run/test`).
examples/minimal/  Canonical minimal examples used for regression.
examples/phase4/   Small demos against stdlib.
demos/           Pre-redesign sketches (do not compile today).
docs/            Design docs and specs (see Documentation below).
tests/           Reserved for future .kai-level test suites.
runtime/         Reserved for the stage 2 runtime (Perceus, fibers).
scripts/         Helper scripts (e.g. JSON-schema validation
                 for typed-holes output).
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

kaikai is **pre-1.0 and unstable until the Orongo edition lands**: each
edition pins a stable surface *within itself* (see [docs/editions.md](docs/editions.md)),
but the project is still maturing, so treat the language and stdlib as
unstable for building on top of. Pin your edition in `kai.toml`.

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
