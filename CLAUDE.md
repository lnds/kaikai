# kaikai

A functional programming language with static typing, compiled to native code via LLVM, with algebraic effects as a first-class primitive and Elixir-style pipelines.

The project is in a **full redesign phase**. The previous history (a partial compiler in Go) was discarded. The current design lives in `docs/design.md`.

## Project language conventions

- **Commit messages in English.** No exceptions.
- **All documentation in English.** Including `README.md`, `docs/`, code comments, and any user-facing text.
- Conversation with the user (Spanish) is not documentation and does not appear in the repo.

## Cross-cutting principles (non-negotiable)

1. **Fast compilation** (Go-style)
   - Decidable, predictable type system. No costly type-class resolution.
   - Single-pass parsing, no arbitrary macros.

2. **LLM-friendly**
   - Regular and predictable syntax, minimal special cases.
   - One canonical form for each construct (no syntactic redundancy).
   - Self-explanatory compile errors with concrete suggestions.

3. **Safe**
   - Memory safety by default.
   - No-null by types (`Option[T]`, never `null`).
   - Typed effects: IO and errors cannot escape unhandled.

4. **Runtime-efficient**
   - Monomorphization of generics.
   - Mandatory tail-call optimization.
   - Compact representations (unbox when possible).
   - Low-overhead effects (CPS or segmented stacks).

5. **Easy to learn**
   - Familiar syntax (Python / JS / Elixir as references).
   - Few core concepts, orthogonal to each other.
   - Early REPL, readable error messages.

## Baseline architectural decisions

- **Backend**: LLVM directly (stage 2). Stages 0 and 1 emit portable C.
- **Memory**: hybrid **Perceus** (compile-time optimized RC, Koka-style) inside each fiber + **isolated fibers** BEAM-style (private heap, messages copied). No borrow checker.
- **Effects**: capability-passing **Effekt + inference**. Effects as an explicit set; inferred in local bodies; mandatory annotation in public signatures.
- **Concurrency**: actors subsumed under effects. `spawn`/`send`/`receive` are operations of the `Actor`/`Io` effects.
- **FFI**: crossing to C via the `Ffi` effect capability. Declarations use `extern "C" fn name(args) : T`.
- **Tooling**: single `kai` binary with subcommands (`build`/`run`/`test`/`fmt`/`repl`/`lsp`/`doc`).
- **Tests**: builtin syntax (`test "..." { ... }` + `assert`), integrated with `kai test`.

## Three-stage bootstrap

- **Stage 0** — minimal C compiler. Zero dependencies. Compiles **kaikai-minimal** → portable C.
- **Stage 1** — intermediate compiler in kaikai-minimal. Compiles **full kaikai** (effects + handlers + basic Perceus) → C or LLVM IR.
- **Stage 2** — definitive compiler in full kaikai. Direct LLVM backend. Self-hosted.

Any machine with `cc` can bootstrap from scratch:

```sh
cc stage0/*.c -o kaic0
./kaic0 stage1/compiler.kai > stage1.c && cc stage1.c -o kaic1
./kaic1 demos/fizzbuzz.kai -o fizzbuzz
./fizzbuzz
```

## Things to avoid

- **Do not go back to Go** for the compiler. The prior Go frontend was discarded on purpose.
- **Do not introduce a Rust-style borrow checker**. Perceus + fibers resolves memory at compile time without cognitive cost.
- **Do not add redundant syntax**. One canonical form per construct.
- **Do not add dependencies to stage 0**. It must build on any system with an ANSI `cc`.
- **Do not design against WASM, Windows, or other post-MVP targets**, but do not invest effort in them now either.

## Current state

See `docs/design.md` for the full plan, phased roadmap, and MVP specification.
