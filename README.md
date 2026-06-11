# kaikai

A **functional**, **statically typed** programming language that compiles to
**native code** via LLVM.

- **Algebraic effects** as a first-class primitive (Effekt-style + inference).
- **Elixir-style pipelines**.
- **Memory** without a global GC or borrow checker: Perceus (compile-time
  optimised RC) + isolated fibers (BEAM-style).
- **Portable bootstrap**: 3-stage compiler (C → kaikai-minimal → full kaikai).

## Status

MVP in progress.

- **Phase 1–3** (landed): kaikai-minimal language and a
  self-hosting `stage1/kaic1` compiler.
- **Phase 4** (landed): small stdlib (`stdlib/core/`) and the
  `kai` driver so programs can be built with a single command.
- **Stage 2** (in progress, `stage2/kaic2`): self-hosted
  compiler in full kaikai. **Typed holes** are landed in m10
  (`?` / `?name` with `--holes-json`); the effects mechanics
  (m7a) and ergonomic sugars (m7b) are pinned across
  `docs/effects.md`, `docs/effects-stdlib.md`,
  `docs/effects-impl.md`, and `docs/syntax-sugars.md`. Fibers,
  actors, and the structured-concurrency runtime ship in m8;
  the LLVM IR backend, `kai fmt` / `kai lsp`, and
  the `Map[K, V]` / `Vector[T]` collection family are
  scheduled later milestones — see `docs/stage2-design.md`
  §Milestones for the full list, and `docs/design.md` for the
  roadmap context.

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

The default backend is the portable C path (`kaic2` emits C, linked
with `cc`) — supported across the whole corpus. Two LLVM backends are
opt-in:

- `--backend=native` / `KAI_BACKEND=native` — the in-process libLLVM
  backend (builds the module via the LLVM C API and emits a native
  object directly; no `.ll` text, no `clang`). This is the project's
  intended default destination (docs/kir-design.md §7.2); it is opt-in
  until its full-corpus parity with the C oracle is complete
  (KIR Lane 1.5).
- `--backend=llvm` / `KAI_BACKEND=llvm` — the legacy LLVM-text path
  (emits `.ll` via `kaic2 --emit=llvm`, links with `clang`).

The `--backend` flag overrides `KAI_BACKEND`, which overrides the `c`
default.

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
stage2/          Stage 2 compiler in full kaikai (in progress;
                 typed holes landed in m10).
stdlib/          Core stdlib in kaikai-minimal (List/String/Option/...).
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
