# kaikai

A functional programming language with static typing, compiled to native code via LLVM, with algebraic effects as a first-class primitive and Elixir-style pipelines.

The project is in a **full redesign phase**. The previous history (a partial compiler in Go) was discarded. The current design lives in `docs/design.md`.

## Project language conventions

- **Commit messages in English.** No exceptions.
- **All documentation in English.** Including `README.md`, `docs/`, code comments, and any user-facing text.
- Conversation with the user (Spanish) is not documentation and does not appear in the repo.

## Cross-cutting principles

Three tiers, ordered by how non-negotiable they are. When principles conflict, the higher tier wins.

### Tier 1 — Load-bearing

1. **Safe at compile time**
   - Memory safety by default.
   - No null; `Option[T]` always.
   - Effects visible in types: every effect a function uses appears in its row (`/ Console + File + Cancel`, etc.). A function cannot be called from a context that does not handle its effects. Catalog and defaults pinned in `docs/effects-stdlib.md` (Doc B).
   - Explicit runtime escapes, audited not incidental: `panic`, unfilled `?`, `todo!`, unbound `axiom`, FFI crossings, **opaque mutable `Array[T]`**.
   - The Array escape is provisional: `array_make / get / set / grow` mutate in place and the mutation is not visible in the type. It exists so the stage 2 inferencer can index its substitution by TyVar id in O(1). Migration specified in `docs/effects-stdlib.md` §`Mutable`: `array_*` retrofits behind the `Mutable` effect in m7a, with the array-indexing sugar (`a[i]`, `a[i] := v`) shipping in m7b. Must not be used as a general-purpose container in new code.

2. **Runtime-efficient**
   - Monomorphisation of generics.
   - Mandatory tail-call optimisation.
   - Primitives unboxed inside fibers; heap boxing only for compound immutables.
   - Effects compiled with one-shot continuations as the zero-cost default; multi-shot pays on use.

3. **Fast compilation**
   - Single-pass parse, LL(1) grammar with minor bookkeeping.
   - HM-extended types with effect rows; decidable; **no Haskell-style type-class resolution** (no HKT, no constraint propagation in signatures, no functional dependencies, no type families). Single-dispatch protocols Go/Clojure/Elixir-style — `O(1)` impl-table lookup, no constraint solver — are permitted (m12.8, see `docs/protocols.md`).
   - Pipeline `lex → parse → resolve → infer → monomorph → perceus → lower`, dumpable between any two passes.

### Tier 2 — Aspirational (trade-offs allowed; Tier 1 wins ties)

4. **Structured compiler output**
   - Diagnostics and queries come as stable JSON alongside human text. Typed holes + `--holes-json` are the prototype; `kai type --json` and siblings extend the contract.
   - **Not** "one canonical form per construct". The language has intentional redundancies (`=` vs `{}` bodies, `|>` vs `|`, multiple lambda forms) — each form signals intent. The real rule is *few forms, each with clear intent*.

5. **Approachable core, novel where it pays off**
   - Day-to-day syntax (declarations, `let`, `if`, `match`, pipes, pattern matching) stays close to Python / JS / Elixir.
   - Advanced surface — effects, handlers, nursery, fibers, holes — is **novel on purpose**. Expect a one-day ramp for sequential kaikai and a week to internalise the effect model.

6. **Few visible concepts, layered**
   - Floor: ~10 concepts for basic code (types, functions, `let`, `match`, `if`, records, sum types, lists, pipes, strings).
   - Advanced features stack on top; no program pays for every concept.

### Tier 3 — Strategic bet (depends on future conditions)

7. **LLM authorability**
   - Bet: with typed holes + structured JSON + stable rules, LLMs can author kaikai, even though current models know Python / Rust far better than effect-typed languages.
   - Mechanism: shift weight from *the model knowing kaikai* to *the compiler telling the model what goes where* — holes, effect queries, exhaustiveness counterexamples.
   - Acceptance criterion: an LLM with JSON access completes the top 80% of typical functions within one round of compilation.

## Tie-breakers when principles conflict

- Safety beats ergonomics.
- Fast compilation beats generality.
- Runtime efficiency beats expressive novelty.
- Approachability beats one-canonical-form.
- LLM-friendliness is not a veto: a feature good for LLMs but bad for humans does not ship.

## Not principles

Do not cite these in design arguments:
- *One canonical form per construct* — already violated four times deliberately; see #4.
- *Never surprise a Python programmer* — effects surprise them, by design; see #5.
- *Zero-cost abstractions* — effects, fibers, and RC have small but non-zero costs.
- *Backward compatibility* — not promised until post-MVP.

## Baseline architectural decisions

- **Backend**: LLVM directly (stage 2). Stages 0 and 1 emit portable C.
- **Memory**: hybrid **Perceus** (compile-time optimized RC, Koka-style) inside each fiber + **isolated fibers** BEAM-style (private heap, messages copied). No borrow checker.
- **Effects**: capability-passing **Effekt + inference**. Effects as an explicit set; inferred in local bodies; mandatory annotation in public signatures. Three pinned design docs: `docs/effects.md` (Doc A — semantics), `docs/effects-stdlib.md` (Doc B — catalog and defaults), `docs/effects-impl.md` (Doc C — CPS transform and runtime). Sugars (trailing lambdas, `@cap` / `cap := v`, `var`, `a[i]`) live in `docs/syntax-sugars.md` and ship in m7b.
- **Concurrency**: fibers and actors live entirely inside the effect system.
  - `spawn` / `await` / `select` / `cancel` are ops of the **`Spawn`** effect (`docs/structured-concurrency.md`).
  - `send` / `receive` / `self` are ops of the parameterised **`Actor[Msg]`** effect (`docs/actors.md`).
  - `Cancel` is a separate effect for cooperative cancellation.
- **FFI**: crossing to C via the `Ffi` effect capability. Declarations use `extern "C" fn name(args) : T`.
- **Tooling**: single `kai` binary with subcommands (`build`/`run`/`test`/`fmt`/`repl`/`lsp`/`doc`).
- **Tests**: builtin syntax (`test "..." { ... }` + `assert`), integrated with `kai test`.

## Three-stage bootstrap

- **Stage 0** — minimal C compiler. Zero dependencies. Compiles **kaikai-minimal** → portable C.
- **Stage 1** — intermediate compiler in kaikai-minimal. Compiles **enough of full kaikai to compile stage 2** (basic effects + handlers + basic Perceus + monomorphisation) → C. Full effect catalog, m7b sugars, fibers, and actors land in stage 2 (`docs/stage2-design.md`).
- **Stage 2** — definitive compiler in full kaikai. Direct LLVM backend. Self-hosted.

Any machine with `cc` can bootstrap from scratch:

```sh
cc stage0/*.c -o kaic0
./kaic0 stage1/compiler.kai > stage1.c && cc stage1.c -I stage0 -o kaic1
./kaic1 demos/fizzbuzz.kai -o fizzbuzz
./fizzbuzz
```

## Things to avoid

- **Do not go back to Go** for the compiler. The prior Go frontend was discarded on purpose.
- **Do not introduce a Rust-style borrow checker**. Perceus + fibers resolves memory at compile time without cognitive cost.
- **Do not add forms whose intent overlaps with an existing one**. Two bodies for functions is fine (short vs block); two pipes is fine (apply vs map); a third way to do the same thing with no new intent is not. The standard is *few forms, each carrying distinct intent* — not *one form, full stop*.
- **Do not add dependencies to stage 0**. It must build on any system with an ANSI `cc`.
- **Do not design against WASM, Windows, or other post-MVP targets**, but do not invest effort in them now either.
- **Do not cite retired principles** in design arguments (see the "Not principles" section above).

## Current state

See `docs/design.md` for the full plan, phased roadmap, and MVP specification.
