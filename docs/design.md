# kaikai design

Living document for the full language redesign. The decisions listed here guide current implementation.

## Context

kaikai is a programming language in full redesign. The previous codebase (a partial Go frontend — lexer + parser → AST, without backend or execution) was discarded. The examples under `demos/` are historical input sketching syntax ideas (Elixir-style pipes, pattern matching, algebraic effects `perform`/`handle`/`resume`, actors, ranges/sequences, records/atoms/tuples, list comprehensions) and will be rewritten under the new design.

The redesign targets:

- A **functional** language with **static typing**
- Compilation to **native code** via **LLVM**
- **Algebraic effects** as a first-class primitive
- **Elixir-style pipelines**

This document is updated as decisions are closed.

## Cross-cutting principles

1. **Fast compilation** (Go model)
   - Decidable, predictable type system (HM-extended or Effekt-style).
   - Single-pass parsing, no arbitrary macros.
   - No costly type-class resolution.

2. **LLM-friendly**
   - Regular, predictable syntax, minimal special cases.
   - One canonical form per construct (no syntactic redundancy).
   - Self-explanatory compile errors with concrete suggestions.

3. **Safe**
   - Memory safety by default (see memory model).
   - No-null by types (`Option[T]`, never `null`).
   - Typed effects: IO and errors cannot escape unhandled.

4. **Runtime-efficient**
   - Monomorphization of generics.
   - Mandatory tail-call optimization.
   - Compact representations (unbox when possible).
   - Low-overhead effects (CPS or segmented stacks).

5. **Easy to learn**
   - Familiar syntax (Python/JS/Elixir as references).
   - Few core concepts, orthogonal to each other.
   - Early REPL, readable error messages.

## Decisions

- **Scope**: full redesign. The current syntax and semantics are input, not constraint.
- **Backend**: LLVM directly.
- **Concurrency**: subsumed under effects. Actors = effect capability, not a separate primitive. `spawn`/`send`/`receive` are operations of the `Actor`/`Io` effects.
- **Memory model**: hybrid.
  - **Perceus** (compile-time optimized reference counting, Koka-style) within each fiber.
  - **Isolated fibers** BEAM-style: private heap per fiber, messages copied across boundaries.
  - Goal: compile-time memory-lifetime decisions without a visible borrow checker, predictable latencies via fiber-local GC.
- **Formal effects model**: capability-passing **Effekt + inference**.
  - Effects as an explicit set of capabilities, passed implicitly by the compiler.
  - **Effect inference in local bodies**: the user does not annotate sets in private functions; the compiler infers them.
  - **Mandatory annotation on public signatures** (module exports, APIs): effects are part of the contract.
  - Fits LLVM (CPS over segmented stacks) and the LLM-friendly / easy-to-learn principles.
- **Core tooling**: single `kai` binary with subcommands (Go/Rust/Zig style).
  - MVP essentials: `kai build`, `kai run`, `kai test`, `kai fmt`, `kai repl`.
  - Mid-term: `kai lsp` (Language Server Protocol — universal editor support), `kai doc`.
  - Long-term: `kai new`, `kai add` (package manager as separate project).
  - Motivation: LLM-friendly (predictable commands, LSP provides immediate feedback), zero ecosystem fragmentation, trivial install.
- **FFI / Interop**: crossing to C is expressed as the **`Ffi` effect capability**.
  - Declarations use `extern "C" fn name(args) : T`.
  - Calling a C function is an operation of the `Ffi` effect (appears in the type of every function that uses it).
  - Incoming FFI (kaikai as a C library): minimal subset in MVP — exportable pure functions, initializable runtime.
  - Binding generator (`kai bindgen foo.h`): post-MVP.
  - WASM as a target: post-MVP; do not design against it but no dedicated effort for now.
  - Motivation: consistency with the effects system, type-level safety (pure code cannot accidentally call C).
- **Distribution and bootstrapping**: 3 stages, portable bootstrap from C.
  - **Stage 0** (in C, ~5–10K lines, no dependencies): compiles an austere subset **kaikai-minimal** → **portable C**. Any machine with `cc` can build kaikai from scratch.
  - **Stage 1** (in kaikai-minimal, compiled by stage 0): compiles full kaikai (effects + inference + Perceus) → emits C or textual LLVM IR.
  - **Stage 2** (in full kaikai, compiled by stage 1): definitive compiler with direct LLVM, optimizations, Perceus, fiber scheduler. Self-hosted.
  - **Verification**: stage 2 compiled by stage 1 must produce the same binary as stage 2 compiled by itself (fixed-point bootstrap).
  - **Final format**: single static binary with embedded runtime.
  - **MVP targets**: Linux x86_64, macOS arm64. Linux arm64, macOS x86_64, Windows post-MVP.
- **Tests as first-class**: **builtin** syntax, recognized by the compiler.
  - MVP: `test "description" { ... }` + `assert cond, "msg"?` + `kai test [pattern]` runner.
  - Tests are automatically excluded from production builds.
  - Post-MVP: property testing (`check ... with a: Int`), benchmarks (`bench`), snapshots, fuzzing, doctests.
  - Motivation: single canonical form, integration with `kai test`, LLM-friendly (LLMs write tests without picking frameworks).
- **Typed holes** (stage 2): `?` and `?name` are first-class expressions/patterns. At check time the compiler reports the expected type, bindings in scope, and synthesis candidates, both as human-readable text and as JSON (`kai build --holes-json`). Unfilled holes don't break the build — they become a runtime panic — so partial programs run. Designed as the integration point for LLM-assisted editing. Full rationale and syntax in `docs/typed-holes.md`.
- **Structured concurrency** (stage 2): every fiber lives inside a lexical scope (`nursery`) that waits for its children and propagates cancellation. `spawn` / `await` / `select` are operations of a `Spawn` effect, not built-in primitives; the nursery is literally a handler for that effect. `Fiber[T]` is a region-branded capability that cannot escape its scope. Cancellation is an effect (`Cancel`) that fibers can handle for cleanup. Full rationale, syntax, and patterns in `docs/structured-concurrency.md`.

## MVP scope

**Medium MVP**: stage 0 working + stage 1 compiling a significant subset of full kaikai.

**Verifiable milestone**: any machine with `cc` can execute non-trivial kaikai programs.

```sh
cc stage0/*.c -o kaic0
./kaic0 stage1/*.kai > stage1.c && cc stage1.c -o kaic1
./kaic1 demos/fizzbuzz.kai -o fizzbuzz
./fizzbuzz
```

### kaikai-minimal (subset compiled by stage 0)

- Functions, `if`/`match`, `let`, recursion.
- Primitive types: `Int`, `Bool`, `String`, `Char`.
- Strings with decent manipulation (critical for writing a compiler).
- Immutable lists.
- Tagged sum types (for AST representation): `type Expr = Lit(Int) | Add(Expr, Expr)`.
- Basic records.
- Minimal file IO (`read_file`, `write_file`, `print`).
- `Result[E, T]` and `Option[T]` for error handling.
- Pattern matching over sum types and lists.
- Mandatory type annotations (no inference yet).
- Simple RC (no full Perceus).
- **Not included**: effects system (only a primitive `Io`), inference, complex generics, fibers, pipe operator, full Perceus.

### Stage 1 (compiles a significant subset of full kaikai)

Must support, at minimum:
- Full pattern matching.
- Effects with handlers (no inference yet — mandatory annotations).
- Basic Perceus (simple reuse analysis).
- Monomorphized generics.
- Builtin tests (`test`/`assert`) and runner.
- A stdlib subset sufficient to run demos ported to the new design.

It does not need to compile 100% of full kaikai — what matters is that it compiles **a version of stage 2** that does.

### Post-MVP

- Stage 2 with direct LLVM backend, full Perceus, effect inference, fibers, scheduler.
- Tooling: `kai fmt`, `kai repl`, `kai lsp`.
- Property testing, benchmarks, snapshots.
- FFI binding generator.
- More targets (Linux arm64, macOS x86_64, Windows).
- Package manager (`kai new`, `kai add`).

## Repository layout

```
/
  stage0/          # compiler in C (kaikai-minimal → C)
  stage1/          # compiler in kaikai-minimal (full kaikai → C/LLVM IR)
  stage2/          # compiler in full kaikai (LLVM directly) — post-MVP
  stdlib/          # standard library (kaikai)
  runtime/         # runtime in C: GC (Perceus), fiber scheduler, etc.
  demos/           # examples (current ones will be ported to the new design)
  tests/           # compiler tests (in kaikai, using `test`/`assert`)
  docs/            # language and compiler documentation
  CLAUDE.md        # project principles and conventions
  README.md
```

## Open decisions

- **Concrete syntax consolidation**: eliminate redundancies (`let`/`:=`, `switch`/`cond`/`match`, `|` vs `|>`, collections `[]`/`()`/`{}`, atoms/structs/maps). *Deferred until kaikai-minimal stabilizes.*

## Roadmap

**Phase 0 — Preparation** (in progress):
1. Create `CLAUDE.md` with cross-cutting principles.
2. Reinit git and initial commit.
3. Force-push to remote.

**Phase 1 — kaikai-minimal specification**:
1. `docs/kaikai-minimal.md` with grammar, semantics, types.
2. Examples: hello world, fizzbuzz, quicksort in kaikai-minimal.
3. Verification: grammar reviewed against examples.

**Phase 2 — Stage 0 in C**:
1. Lexer in C (no deps, `stage0/lexer.c`).
2. Recursive-descent parser (`stage0/parser.c`).
3. Simple type checker (`stage0/check.c`).
4. C emitter (`stage0/emit.c`).
5. Minimal RC runtime in `stage0/runtime.h`.
6. Tests: hello world and fizzbuzz compile and run end-to-end.

**Phase 3 — Stage 1 in kaikai-minimal**:
1. Rewrite stage 0 in kaikai-minimal (dogfooding — validates the language).
2. Extend with full-kaikai features: full pattern matching, effects with handlers (no inference yet), monomorphized generics, basic Perceus.
3. Backend: emit C (simpler) or textual LLVM IR.
4. Builtin tests (`test`/`assert`) working + `kai test` runner.
5. Verification: stage 1 compiled by stage 0 compiles the ported demos.

**Phase 4 — Basic stdlib and MVP tooling**:
1. Stdlib: `List`, `Map`, `String`, `Io`, `Option`, `Result`.
2. `kai build`, `kai run`, `kai test` working against stage 1.
3. Demos rewritten under the new design, running.

**Post-MVP** (out of immediate scope):
- Stage 2 with LLVM backend directly, full Perceus, effect inference, fibers, BEAM-style scheduler.
- **Typed holes** (`docs/typed-holes.md`): `?` / `?name` in expressions and patterns, structured reports (expected type, in-scope bindings, candidates) in text and JSON. First-class integration point for LLM-assisted editing.
- **Structured concurrency** (`docs/structured-concurrency.md`): nursery-scoped fibers, `Spawn` / `Cancel` as effects, region-branded `Fiber[T]` that cannot escape its scope. Built on top of the effects + handlers machinery.
- Elm/Rust-level error messages as an explicit design investment (not a "feature" — a quality bar for every diagnostic).
- `kai fmt`, `kai repl`, `kai lsp`.
- Property testing (`check`), benchmarks (`bench`), snapshots.
- FFI binding generator.
- More targets (Linux arm64, macOS x86_64, Windows).
- Package manager (`kai new`, `kai add`).
- Final syntax consolidation.
- WASM.

## End-to-end MVP verification

A user on Linux x86_64 or macOS arm64 with only `cc` installed must be able to:

```sh
git clone github.com/lnds/kaikai
cd kaikai
cc stage0/*.c -o kaic0
./kaic0 stage1/compiler.kai > stage1.c && cc stage1.c -o kaic1
./kaic1 demos/fizzbuzz.kai -o fizzbuzz
./fizzbuzz   # expected output: fizzbuzz for 0..100
./kaic1 test tests/   # builtin tests pass
```

If this works, the medium MVP is fulfilled.
