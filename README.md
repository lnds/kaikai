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
  the LLVM IR backend, `kai fmt` / `kai repl` / `kai lsp`, and
  the `Map[K, V]` / `Vector[T]` collection family are
  scheduled later milestones — see `docs/stage2-design.md`
  §Milestones for the full list, and `docs/design.md` for the
  roadmap context.

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

**Next step:** follow [`docs/tutorial.md`](docs/tutorial.md) for a
30-minute ramp through the language — install, hello world,
records / sums / pipes, custom effects, protocols, units of measure,
refinements, fibers, and the builtin test runner. Each section runs
against a numbered file in `examples/tutorial/`.

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

To be defined.
