# Contributing to kaikai

kaikai is a functional, statically typed, native-compiled programming language with algebraic effects as a first-class primitive. This document covers the design principles you should understand before proposing changes, and the mechanics of how to contribute.

## Where work happens

- **Bug reports, Q&A, RFCs, showcase**: GitHub Discussions on this repository. Discussions has the categories needed for each.
- **Issues**: not enabled on this public repository. Maintainer-side tracking happens upstream.
- **Pull requests**: welcome. Please open a Discussion first if your change is non-trivial (new language surface, breaking change, large refactor).

## Cross-cutting principles

Three tiers; the higher tier wins on conflict.

### Tier 1 — Load-bearing

1. **Safe at compile time.** Memory-safe by default; `Option[T]` instead of null; effects visible in row types (catalog in `docs/effects-stdlib.md`); explicit runtime escapes (`panic`, `?`, `todo!`, `axiom`, FFI) are audited, not incidental. `Array[T]` writes ride the `Mutable` effect per `docs/effects-stdlib.md` §`Mutable` on the observable-effects discipline (issue #251 + #252): observable mutations require `Mutable`, locally-constructed Arrays mask it.
2. **Runtime-efficient.** Generics monomorphised; mandatory TCO; primitives unboxed inside fibers; effects compile to one-shot continuations as the zero-cost default.
3. **Fast compilation.** Single-pass parse, LL(1) with minor bookkeeping; HM extended with effect rows, decidable; **no Haskell-style type-class resolution** (no HKT, no constraint propagation, no functional dependencies, no type families). Single-dispatch protocols Go/Clojure/Elixir-style — `O(1)` impl-table lookup — are permitted (`docs/protocols.md`). Pipeline `lex → parse → resolve → infer → monomorph → perceus → lower`, dumpable between any two passes.

### Tier 2 — Aspirational (Tier 1 wins ties)

4. **Structured compiler output.** Diagnostics + queries come as stable JSON alongside human text. Typed holes + `--holes-json` are the prototype. **Not** "one canonical form per construct" — the language has intentional redundancies (`=` vs `{}` bodies, `|>` vs `|`, multiple lambda forms); the rule is *few forms, each with clear intent*.
5. **Approachable core, novel where it pays off.** Day-to-day syntax (declarations, `let`, `if`, `match`, pipes, patterns) stays close to Python/JS/Elixir. Advanced surface — effects, handlers, nursery, fibers, holes — is novel on purpose.
6. **Few visible concepts, layered.** ~10 concepts for basic code; advanced features stack on top.

### Tier 3 — Strategic bet

7. **LLM authorability.** Bet that typed holes + structured JSON + stable rules let LLMs author kaikai despite weak prior exposure. Acceptance: an LLM with JSON access completes the top 80% of typical functions within one round of compilation.

### Tie-breakers

- Safety beats ergonomics.
- Fast compilation beats generality.
- Runtime efficiency beats expressive novelty.
- Approachability beats one-canonical-form.
- LLM-friendliness is not a veto: a feature good for LLMs but bad for humans does not ship.

### Not principles (do not cite)

- *One canonical form per construct* — already violated four times deliberately; see #4.
- *Never surprise a Python programmer* — effects surprise them, by design.
- *Zero-cost abstractions* — effects, fibers, RC have small but non-zero costs.
- *Backward compatibility* — not promised until post-MVP.

## Baseline architecture

- **Backend**: LLVM directly (stage 2). Stages 0–1 emit portable C.
- **Memory**: Perceus (compile-time-optimised RC, Koka-style) inside each fiber + isolated fibers BEAM-style (private heap, messages copied). No borrow checker.
- **Effects**: capability-passing Effekt + inference. Inferred in local bodies; mandatory annotation in public signatures. Three pinned docs: `docs/effects.md` (semantics), `docs/effects-stdlib.md` (catalog + defaults), `docs/effects-impl.md` (CPS + runtime). Sugars in `docs/syntax-sugars.md`.
- **Concurrency**: fibers and actors live inside the effect system. `spawn/await/select/cancel` are ops of `Spawn` (`docs/structured-concurrency.md`); `send/receive/self` are ops of `Actor[Msg]` (`docs/actors.md`); `Cancel` is a separate cooperative-cancellation effect.
- **FFI**: crosses to C via the `Ffi` capability. `extern "C" fn name(args) : T`.
- **Tooling**: single `kai` binary (`build`/`run`/`test`/`fmt`/`lsp`/`doc`). REPL is permanently out of scope — see `docs/decisions/repl-removal-2026-05-09.md`.
- **Tests**: builtin syntax (`test "..." { ... }` + `assert`), via `kai test`.

## Three-stage bootstrap

- **Stage 0** — minimal C compiler, zero deps. Compiles kaikai-minimal → portable C.
- **Stage 1** — intermediate compiler in kaikai-minimal. Compiles enough of full kaikai to produce stage 2 → C.
- **Stage 2** — definitive compiler in full kaikai, direct LLVM backend, self-hosted (`docs/stage2-design.md`).

Bootstrap from any machine with `cc`:

```sh
cc stage0/*.c -o kaic0
./kaic0 stage1/compiler.kai > stage1.c && cc stage1.c -I stage0 -o kaic1
./kaic1 demos/fizzbuzz.kai -o fizzbuzz
./fizzbuzz
```

## Running the test suite

- `make tier0` — pre-commit fast sanity (~30–60 s).
- `make tier1` — full suite (typer, codegen, demos, negative-space tests). Required before opening a PR. Gated by CI on every push.
- `make tier1-asan` — path-gated, catches non-portable fixes that pass on macOS but fail on Linux.

CI green is the merge gate. Selfhost byte-identical is the architectural invariant — your change should not alter the bytes that `kaic2` emits when compiling itself, unless that is the explicit purpose of the change.

## Commit messages — Conventional Commits

`<type>(<scope>)?: <subject>`. Type drives changelog placement and version bump:

- `feat` → CHANGELOG "Added", MINOR bump (pre-1.0).
- `fix` → "Fixed", PATCH.
- `perf` / `refactor` → "Changed", PATCH.
- `docs`, `chore`, `ci`, `test`, `build` → excluded from changelog, no bump.

Domain areas (`typer`, `runtime`, `perceus`, `emit`, `tco`, `stdlib`, `demos`, `fmt`, `unbox`, …) are **scopes**, not types: `fix(typer): correct row inference`, never `typer: correct row inference`.

**Breaking changes**: `feat(typer)!: drop X` plus `BREAKING CHANGE: <why>` footer.

`VERSION` and `CHANGELOG.md` are regenerated by `commitizen` at release time from commit messages. Do not edit them by hand.

## Things to avoid

- **Do not go back to Go** for the compiler. The prior Go frontend was discarded on purpose.
- **Do not introduce a Rust-style borrow checker.** Perceus + fibers resolves memory at compile time without cognitive cost.
- **Do not add forms whose intent overlaps an existing one.** Two function bodies (short vs block) is fine; two pipes (apply vs map) is fine; a third way to do the same thing with no new intent is not.
- **Do not add dependencies to stage 0.** It must build on any system with an ANSI `cc`.
- **Do not design against WASM, Windows, or other post-MVP targets.**
- **Do not cite retired principles** in design arguments (see *Not principles* above).
