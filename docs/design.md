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

Three tiers, ordered by how non-negotiable they are. When principles
conflict, the higher tier wins. This structure replaces the earlier
flat list; the motivation is that the flat list had internal
tensions (e.g. "familiar syntax" vs "one canonical form"; "easy to
learn" vs algebraic effects) that were being resolved case by case
without being named. Tiers make the trade-offs explicit.

### Tier 1 — Load-bearing

The design breaks if any of these is compromised.

1. **Safe at compile time**
   - Memory safety by default (see memory model below).
   - No null; `Option[T]` is the canonical absence type.
   - Effects are visible in types: IO, errors, cancellation, spawn
     appear as rows on the function's signature. A function cannot
     be called from a context that does not handle its effects.
   - Runtime escapes are explicit and audited: `panic(msg)`,
     unfilled typed holes (`?`), `todo!(msg)`, `axiom` without a
     linked body, and FFI crossings. These abort the process; they
     do not silently recover.

2. **Runtime-efficient**
   - Monomorphisation of generics; no dictionary passing.
   - Mandatory tail-call optimisation for self-recursion and mutual
     recursion within a module.
   - Compact representations: primitives unboxed inside each fiber,
     heap boxing only for compound immutable values.
   - Effects compiled with one-shot continuations as the zero-cost
     default; multi-shot is a runtime property paid only on use.

3. **Fast compilation**
   - Single-pass parse, LL(1) grammar with minor bookkeeping.
   - HM-extended type system with effect rows. Decidable. No
     type-class resolution.
   - Pipeline `lex → parse → resolve → infer → monomorph → perceus →
     lower`, each pass a pure function of its input, with
     `--dump=<pass>` between any two.

4. **Stability without stagnation**
   - Adopted from Rust's
     [2014 manifesto](https://blog.rust-lang.org/2014/10/30/Stability/).
     The contract to the user is that upgrading kaikai does not
     break their code without a migration path. The contract to
     the kaikai team is that we can keep iterating fast on
     internals — runtime, codegen, RC discipline, cache layers,
     typer refactors — without that protection blocking progress.
   - The mechanism is **editions**, geographic Rapa Nui names
     matching the kaikai naming family (Tongariki today, Anga Roa
     21 May 2026, Orongo after). An edition is a snapshot of the
     language surface plus the user-facing protocol/stdlib API
     boundaries. Within one edition, breaking changes are
     forbidden. Between editions, breaking changes are allowed
     but require explicit opt-in (the user picks which edition
     their package compiles against in `kai.toml`).
   - Pre-1.0 we still ship `feat:` / `fix:` rapidly per
     Conventional Commits and PATCH/MINOR bumps. The protection
     lives at the **edition boundary**, not at every minor.
   - The `EDITION` file at repo root holds the current edition
     name. `kai --version` surfaces it. `docs/editions.md`
     documents the edition policy: what stability commitments
     each edition makes, what changed between editions, how to
     migrate.
   - The lesson load-bearing here, in the words of the Rust
     team: *"we owe it to users not to dread upgrading"*. That
     phrase is the test for every breaking-change discussion:
     if a user has to dread the upgrade, the change requires an
     edition bump.

### Tier 2 — Aspirational (trade-offs allowed; Tier 1 wins ties)

5. **Structured compiler output**
   - Every diagnostic and query is emitted as stable JSON alongside
     the human-readable text. Typed holes + `--holes-json` are the
     prototype for this contract; `kai type --json`, `kai effects
     --json`, and the counterexample JSON for non-exhaustive matches
     extend it. See `docs/proposed-extensions.md`.
   - **Not** "one canonical form per construct". The language has
     intentional redundancies: `=` expression body vs `{ ... }` block
     body, `|>` (apply) vs `|` (map), and three lambda forms
     (`x => ...`, `(a, b) => ...`, placeholder `.`). Each form
     signals intent. The real rule is *few forms, each with clear
     intent*, not *one form, full stop*.

6. **Approachable core, novel where it pays off**
   - Day-to-day syntax — declarations, `let`, `if`, `match`, pipes,
     pattern matching, interpolation — deliberately stays close to
     Python / JavaScript / Elixir.
   - The advanced surface — effects, handlers, nursery, fibers,
     typed holes — is **novel on purpose**. It has no direct
     analogue in mainstream languages. The cost is a learning bump;
     the payoff is that one coherent mechanism replaces
     async/await + try/catch + cancellation tokens + actor
     frameworks.
   - Concretely: a Python programmer should be productive on
     sequential kaikai in about a day, and take roughly a week to
     internalise the effect model.

7. **Few visible concepts, layered**
   - The floor for basic code is about ten concepts: types,
     functions, `let`, `match`, `if`, records, sum types, lists,
     pipes, strings. Algebraic types compose via `|` (union); see
     `docs/unions.md` for the user-facing reference.
   - Advanced features (effects, handlers, fibers, nursery, holes)
     layer on top and are adopted when needed. A program that does
     not use them pays no cognitive cost for them.
   - Orthogonality is enforced by review: every new feature must
     not duplicate another, must compose cleanly with effects, and
     must live in a single dedicated design doc.

### Tier 3 — Strategic bet (depends on future conditions)

8. **LLM authorability**
   - The bet: with typed holes + structured JSON + a stable
     ruleset, LLMs can author kaikai well, even though current
     models know Python and Rust far better than effect-typed
     languages. The corpus catches up, or the tooling compensates;
     either way the loop closes.
   - The mechanism: shift weight from *the model knowing kaikai*
     to *the compiler telling the model what goes where*. Typed
     holes surface expected types and candidates; `kai type --json`
     answers position queries; counterexample JSON tells the model
     which patterns it missed.
   - Acceptance criterion: an LLM with access to the JSON surface
     can complete the top 80% of typical functions a programmer
     would ask it to write, within one round of compilation. If
     that does not hold in practice, re-evaluate before adding
     features motivated by this principle.

## Tie-breakers when principles conflict

- Safety beats ergonomics.
- Fast compilation beats generality.
- Runtime efficiency beats expressive novelty.
- Approachability beats one-canonical-form.
- LLM-friendliness is not a veto: a feature good for LLMs but bad
  for humans does not ship.

## Not principles

Retired or explicitly rejected. Do not cite these in design
arguments:

- *One canonical form per construct* — already violated four times
  deliberately; see #5. The real standard is *few forms with clear
  intent*.
- *Never surprise a Python programmer* — effects, nursery, and
  typed holes will surprise them, by design; see #6.
- *Zero-cost abstractions* — effects, fibers, and Perceus RC have
  small but non-zero costs. The target is *low* cost, not zero.
- *Full backward compatibility forever* — within an edition, yes
  (per #4 stability without stagnation). Across editions, breaking
  changes are allowed but require explicit opt-in. Pre-1.0 we
  still ship `feat:` / `fix:` rapidly via Conventional Commits and
  PATCH/MINOR bumps; the protection lives at the edition boundary,
  not at every minor version.

## Decisions

- **Scope**: full redesign. The current syntax and semantics are input, not constraint.
- **Backend**: LLVM directly.
- **Concurrency**: subsumed under effects. Fibers and actors are not separate primitives — both ride on top of the effect system.
  - `spawn` / `await` / `select` / `cancel` are operations of the **`Spawn`** effect; `nursery { n -> ... }` installs the handler that scopes them. See `docs/structured-concurrency.md`.
  - `send` / `receive` / `self` are operations of the parameterised **`Actor[Msg]`** effect; `spawn_actor`, `spawn_actor_default`, and `with_mailbox` install the handler with a typed mailbox. See `docs/actors.md`.
  - `Cancel` is its own effect, delivered cooperatively at yield points.
- **Memory model**: hybrid.
  - **Perceus** (compile-time optimized reference counting, Koka-style) within each fiber.
  - **Isolated fibers** BEAM-style: private heap per fiber, messages copied across boundaries.
  - Goal: compile-time memory-lifetime decisions without a visible borrow checker, predictable latencies via fiber-local GC.
  - **Opaque mutable `Array[T]` behind `Mutable`** (issue #251 + #252, 2026-05). `array_make / length / get / set / grow` are builtins; the typer routes writes (`array_set`, `array_grow`, the `a[i] := v` sugar) through the `Mutable` effect on the **observable-effects** discipline (Koka-style): a write requires `Mutable` if and only if the target Array escapes the function (parameter, capture, global, record field). Writes to a locally-constructed Array that never crosses a function-call boundary are *masked* — the row stays clean from the caller's POV. Reads (`array_get`, `array_length`) never raise `Mutable` regardless of provenance. This collapses the prior "audited escape" debt into the standard row discipline: every observable mutation is in the row, every truly-local one is invisible. See `docs/effects-stdlib.md` §`Mutable` for the full spec, three worked examples, and the v1 conservative-escape rule for Arrays passed as helper arguments.
- **Formal effects model**: capability-passing **Effekt + inference**.
  - Effects as an explicit set of capabilities, passed implicitly by the compiler.
  - **Effect inference in local bodies**: the user does not annotate sets in private functions; the compiler infers them.
  - **Mandatory annotation on public signatures** (module exports, APIs): effects are part of the contract.
  - Fits LLVM (CPS over segmented stacks) and the LLM-friendly / easy-to-learn principles.
- **Core tooling**: single `kai` binary with subcommands (Go/Rust/Zig style).
  - MVP essentials: `kai build`, `kai run`, `kai test`, `kai fmt`.
  - Mid-term: `kai lsp` (Language Server Protocol — universal editor support), `kai doc`.
  - Long-term: `kai new`, `kai add` (package manager as separate project).
  - **Out of scope (permanently)**: `kai repl`. Removed from v1.0 per #406 and not planned for v1.x or v2. See `docs/decisions/repl-removal-2026-05-09.md` for the rationale. The `kai run` + `kai watch` workflow replaces the REPL use case.
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
- **Structured concurrency** (stage 2): every fiber lives inside a lexical scope (`nursery`) that waits for its children and propagates cancellation. `spawn` / `await` / `select` are operations of a `Spawn` effect, not built-in primitives; the nursery is literally a handler for that effect. `Fiber[T]` is a region-branded handle (same brand machinery as `Pid[Msg]` from actors) that cannot escape its scope. Cancellation is an effect (`Cancel`) that fibers can handle for cleanup. Full rationale, syntax, and patterns in `docs/structured-concurrency.md`; the actor surface (mailbox, supervision, `Pid[Msg]`) lives in `docs/actors.md`.
- **Kind system**: closed at two kinds — `Type` (default, classifies normal types) and `Measure` (classifies units of measure used in `Real<u>` and friends). Most code never names a kind; the annotation only appears when a tparam ranges over units (`fn area[u: Measure](w: Real<u>, h: Real<u>) : Real<u^2>`). The `Measure` kind, the `Unit` void-return type, and the `unit` declaration keyword are three distinct things that share no semantics — see `docs/kinds.md` for the full reference and disambiguation table.

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
- Tooling: `kai fmt`, `kai lsp`. (`kai repl` is permanently out of scope per #406 and `docs/decisions/repl-removal-2026-05-09.md`.)
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

- **Canonical syntax reference**: `docs/grammar.md` is the single
  reference for the full-kaikai surface grammar (BNF/EBNF) — lexical
  structure, productions, precedence, sugar deltas, ambiguity rules,
  reserved tokens. Stage 0's grammar is a strict subset and continues
  to live in `docs/kaikai-minimal.md`.

- **Concrete syntax consolidation** (kaikai-minimal stabilised; resolved item-by-item):
  - `let` vs `:=` — **resolved**: not redundant. `let` introduces an immutable binding; `:=` mutates a `var` cell or array slot (m7b #5, #6). Distinct operations.
  - `switch` / `cond` / `match` — **resolved**: `match` is the only form. Neither `switch` nor `cond` exist in kaikai-minimal (`kaikai-minimal.md` §`match`). No redundancy to remove.
  - `|` vs `|>` — **resolved**: both retained with distinct intent. `|>` is apply-pipe (first-arg threading); `|` is map-pipe over a list (m7b #9, see `docs/syntax-sugars.md`). Each form signals a different operation.
  - collections `[]` / `()` / `{}` — **resolved**: `[]` for ordered lists, `{}` for records (named fields), `()` for grouping only. The "should `()` gain a tuples meaning" sub-decision was closed on 2026-04-27 with verdict **REJECT** (m8.5 gate; methodology and metrics in `docs/proposed-extensions.md` §9). Anonymous products use `Pair[a, b]` (stdlib); transient pattern-match shapes use the multi-arg match sugar (scheduled).
  - atoms / structs / maps — **resolved**: kaikai has no atoms (no Erlang-style standalone symbols). Records carry nominal identity; `Map[K, V]` is a lookup table landing with the collection-design pass (m14). Distinct concepts, no overlap.

  Net status: every sub-decision in this list is now closed.

- **Extensions catalogue** (`docs/proposed-extensions.md`): two families of proposed additions; status tracked per-item in that document.
  - *LLM-friendly diagnostics* — typed-holes-adjacent features (principled `todo!`, type-query JSON, exhaustiveness counterexamples, `axiom`, effect holes, import holes, canonical-form lints). Share the typed-holes output contract (human text + stable JSON). Most depend on m11 (diagnostics quality pass); `todo!` scheduled for m7d, `axiom` for m12.7.
  - *Language-surface features* — record punning `{ x, y }`, `variants[T]()`, sum types with constant attributes, `!` postfix (Option/Result propagation), `@` as-patterns, `?.` optional chaining, deferred bitwise operators, `Map[K, V]` with hash-map indexing, slice syntax `a[i..j]`, method references as values (`obj.method`), `Range[T]` as a first-class iterable, pipeline placeholder `_`, binary pattern matching `<<...>>`. Tuples `(T1, T2)` were rejected at m8.5 (2026-04-27); see §9. Six items now scheduled (m7d/m7e); the rest split between collection-design (m14), demand-driven (#12, #15, #16), and standalone candidates (#19, #22).

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
1. Stdlib for stage 1: `List`, `String`, `Option`, `Result`, plus the `Io`-aliased granular effects (`Console`, `Stdin`, `Env`, `File`). Today these ship as full effect declarations; the historical "pre-effects-system shape" wording predates the m7a/m7b split that moved them behind the effect system. See `docs/effects-stdlib.md` §`Console` for the v1 monolithic surface and the planned 3-way `Stdout`/`Stderr`/`Stdin` split (#360).
2. `kai build`, `kai run`, `kai test` working against stage 1.
3. Demos rewritten under the new design, running.

`Map[K, V]` and full effects-aware stdlib (`Mutable`, `State[T]`, `Reader[T]`, `Writer[W]`, `Fail`, `Cancel`, `Spawn`, `Ffi`) land in stage 2 — m7a (mechanics) and m7b (sugars). The actor surface (`Actor[Msg]`, `Pid[Msg]`, mailbox policies, link/monitor) lands in m8 alongside the fiber scheduler. See the post-MVP list below for the deliverables.

**Post-MVP** (out of immediate scope):
- Stage 2 with LLVM backend directly, full Perceus, effect inference, fibers, BEAM-style scheduler.
- **Effects system** — three pinned design docs:
  - `docs/effects.md` (Doc A): rows, unification, syntax, `handle` / `resume`, inference. The mental model.
  - `docs/effects-stdlib.md` (Doc B): catalog of stdlib effects (`Console`, `Stdin`, `Env`, `File`, `Fail`, `State[T]`, `Reader[T]`, `Writer[W]`, `Mutable`, `Cancel`, `Spawn`, `Ffi`), their default handlers, the `Io` alias, and the m7a/m7b sub-milestones.
  - `docs/effects-impl.md` (Doc C, in progress): CPS transform, handler-stack runtime, codemod for migrating bare builtins.
- **Syntax sugars** (`docs/syntax-sugars.md`): trailing lambdas, `@cap` / `cap := v`, local `var x = init` cells, array indexing `a[i]` / `a[i] := v`. m7b ships these alongside the effects mechanics.
- **Typed holes** (`docs/typed-holes.md`): `?` / `?name` in expressions and patterns, structured reports (expected type, in-scope bindings, candidates) in text and JSON. First-class integration point for LLM-assisted editing. *Landed in stage 2 m10.*
- **Structured concurrency** (`docs/structured-concurrency.md`): nursery-scoped fibers, `Spawn` / `Cancel` as effects, region-branded `Fiber[T]` that cannot escape its scope. Built on top of the effects + handlers machinery; lands in m8.
- **Actors** (`docs/actors.md`): `Actor[Msg]` effect with typed mailboxes, link/monitor supervision, `Pid[Msg]` as a region-branded handle. Lands in m8 alongside the scheduler.
- Elm/Rust-level error messages as an explicit design investment (not a "feature" — a quality bar for every diagnostic).
- `kai fmt`, `kai lsp`. (`kai repl` removed permanently per #406 / `docs/decisions/repl-removal-2026-05-09.md`.)
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
