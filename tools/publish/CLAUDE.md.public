# kaikai

A functional language with static typing, LLVM native compilation, algebraic effects as a first-class primitive, and Elixir-style pipelines.

This file is the operational orientation for Claude Code (and any agent-style tool) running on a clone of this repository. For design principles and contribution guidelines, see `CONTRIBUTING.md`. For language semantics, see `docs/design.md`.

## Project language

Commit messages, PR titles/bodies, and all documentation are **English only**.

## Bootstrap

Build the compiler from any machine with `cc`:

```sh
cc stage0/*.c -o kaic0
./kaic0 stage1/compiler.kai > stage1.c && cc stage1.c -I stage0 -o kaic1
./kaic1 demos/fizzbuzz.kai -o fizzbuzz
./fizzbuzz
```

Three stages: `stage0` (minimal C compiler, zero deps), `stage1` (intermediate compiler in kaikai-minimal), `stage2` (definitive compiler in full kaikai, direct LLVM backend, self-hosted).

## How to run the test suite

- `make tier0` — pre-commit fast sanity (~30–60 s).
- `make tier1` — full suite, gated by CI on every PR.
- `make tier1-asan` — path-gated, catches non-portable fixes.

CI green is the merge gate.

## Things to avoid

- **Do not go back to Go** for the compiler. The prior Go frontend was discarded on purpose.
- **Do not introduce a Rust-style borrow checker.** Perceus + fibers resolves memory at compile time without cognitive cost.
- **Do not add forms whose intent overlaps an existing one.** Two function bodies (short vs block) is fine; two pipes (apply vs map) is fine; a third way to do the same thing with no new intent is not.
- **Do not add dependencies to stage 0.** It must build on any system with an ANSI `cc`.
- **Do not design against WASM, Windows, or other post-MVP targets.**
- **Do not cite retired principles** in design arguments. See `CONTRIBUTING.md` §"Not principles".

## Where work happens

- Bug reports and discussion: GitHub Discussions on this repository.
- For the language design rationale: `docs/design.md`, `docs/effects.md`, `docs/effects-impl.md`, `docs/effects-stdlib.md`, `docs/protocols.md`, `docs/structured-concurrency.md`, `docs/actors.md`.
- For contribution mechanics: `CONTRIBUTING.md`.
