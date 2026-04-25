# stage 2 design

Architectural decisions for the **definitive compiler** — the one written in
full kaikai, compiled by stage 1, targeting LLVM directly, with effects,
fibers, and all the things the MVP promised.

Stage 2 is the end state. Stage 0 is throwaway; stage 1 is transitional;
stage 2 is what the rest of the project lives on.

## Relationship to stages 0 and 1

- Stage 0 (`kaic0`) keeps existing as the C-only ingress. It is the only
  binary a user needs to install from source.
- Stage 1 (`kaic1`) keeps existing as the kaikai-minimal compiler and
  serves as the **bootstrap target** for stage 2 — the first time stage
  2 is compiled, it is by stage 1, not by itself.
- Stage 2 (`kaic2`) is the final compiler. Once it compiles itself,
  stage 1 is retired for dev work (kept in the repo for reproducible
  bootstraps from C, but not maintained).

## Non-goals

- **Backwards compatibility with stage 1's output**. Stage 2 may emit
  wildly different C/LLVM IR; what matters is that programs run.
- **Full Haskell-level type inference polymorphism**. Stage 2 sticks to
  the plan: decidable, predictable, fast. Principal types where
  possible, explicit annotations at module boundaries.
- **Dependent types, refinement types, linear types as the core**.
  Affine types are internal (Perceus needs linearity analysis); they
  do not surface to the user yet.
- **Package manager**. Separate project, explicitly post-stage-2.

## What stage 2 adds over stage 1

Grouped by subsystem:

### 1. LLVM backend

- Emit **textual LLVM IR** (`.ll`) as the canonical output.
  `kaic2 foo.kai -o foo.ll`; `kaic2 foo.kai -o foo` links via
  `llc` + `clang`.
- A thin C fallback (`--emit=c`) kept for bootstrapping from stage 1
  and for platforms where LLVM is absent.
- Target triples: `x86_64-linux-gnu`, `aarch64-apple-darwin`, and
  `aarch64-linux-gnu` for the MVP+1 set. WASM and Windows stay
  post-stage-2.

### 2. Full Perceus memory management

- **Reuse analysis**: the same reuse-specialised Koka paper, applied
  as a separate pass over the typed IR. In-place update when the
  compiler can prove no other live reference exists.
- **Drop specialisation**: decref chains generated per type, unboxed
  inline instead of going through a dispatch table.
- **Unboxing** of `Int`, `Real`, `Bool`, `Char` into native machine
  registers inside each fiber. Heap boxing only for compound
  immutable values. Messages across fiber boundaries copy.
- **Region regions** for specific cases where RC is demonstrably
  worse (intermediate parser buffers, say) — opt-in via attribute.

### 3. Effects system — full

The full design lives in three pinned docs:

- `docs/effects.md` (Doc A): rows, unification, syntax,
  `handle`/`resume`, inference. The mental model.
- `docs/effects-stdlib.md` (Doc B): catalog of stdlib effects,
  default handlers, the `Io` alias, m7a/m7b split.
- `docs/effects-impl.md` (Doc C): CPS transform, handler-stack
  runtime, codemod.

Stage 2 ships the implementation; this section summarises the
shape.

- **Capability-passing Effekt + inference**. Effects are
  first-class rows on function types; the checker infers them
  in private bodies and requires them on public signatures.
- `Eff.op(args)` does the call (no `perform` keyword — ops are
  invoked as methods on the effect or its `as`-bound capability,
  per Doc A §*Calling an operation*). `handle { body } with
  Effect { op(args, resume) -> expr }` installs a handler;
  `resume(v)` continues, one-shot by default.
- One-shot/multi-shot is a runtime property, not a type: one-shot
  resume is a tail call, multi-shot pays copy cost via opt-in
  `resume_multishot`.
- Row polymorphism over effect rows: `map[A, B, e](xs: [A], f:
  (A) -> B / e) : [B] / e`.
- **Stdlib effect catalog** (Doc B): `Console`, `Stdin`, `Env`,
  `File` (granularised from a coarse `Io`, with `type Io =
  Console + Stdin + Env + File` as a closed alias); `Fail`,
  `State[T]`, `Reader[T]`, `Writer[W]`, `Mutable`, `Cancel`,
  `Spawn`, `Ffi`. Mailboxes (`Actor[Msg]`) live in
  `docs/actors.md`. The runtime installs default handlers for
  `Console`/`Stdin`/`Env`/`File`/`Mutable`/`Cancel`/`Spawn`/`Ffi`
  when `main`'s row contains them; `Fail`/`State[T]`/`Reader[T]`/
  `Writer[W]` always require an explicit handler.

### 4. Fibers + scheduler

- Every fiber owns a private, movable stack segment (CPS or
  segmented-stack representation — picked at implementation time).
- Per-fiber heap (Perceus RC-scoped); messages are **deep-copied**
  across fiber boundaries and there is no aliasing between fibers.
  Matches BEAM's isolation guarantee.
- **Cooperative scheduler** in the runtime. There is no
  pre-emption: cancellation is delivered only at yield points
  (effect-op call sites). A tight CPU loop with no effect ops is
  not interrupted until it reaches one — `Spawn.yield()` is the
  canonical pinning point. See `docs/structured-concurrency.md`
  §*Non-goals* for the rule.
- `Fiber[T]` is a region-branded **handle** tagged with the
  brand of the enclosing nursery (same machinery as `Pid[Msg]`
  in `docs/actors.md`); it cannot escape its scope.

### 5. Structured concurrency

Design already adopted in `docs/structured-concurrency.md`.
Stage 2 is where it lands (m8).

- `nursery { n -> ... }` is a trailing-lambda call (per
  `docs/syntax-sugars.md` §1) to a stdlib helper that installs
  the `Spawn` effect handler.
- `Spawn` and `Cancel` effects defined in the stdlib.
- Region-branding of `Fiber[T]` lives in the type checker; the
  same brand machinery is shared with `Pid[Msg]` from
  `docs/actors.md`.

### 5b. Actors

Design pinned in `docs/actors.md`. Lands in m8 alongside the
scheduler and `Spawn`.

- `Actor[Msg]` parameterised effect with `self`, `send`,
  `receive` ops; `send` and `receive` carry `Cancel` because
  blocking on a full mailbox or empty receive is a yield point.
- Mailbox policies: `Unbounded`, `Bounded(capacity, on_full)`
  with three overflow rules (`DropOldest`, `DropNewest`,
  `BlockSender`).
- Stdlib helpers: `spawn_actor` (explicit policy),
  `spawn_actor_default` (`Bounded(1024, BlockSender)`),
  `with_mailbox` (mailbox in the current fiber), all passing
  the capability as `m: ActorCap[Msg]` to the body.
- Supervision: `Link` (bidirectional, auto-cancels peer) and
  `Monitor` (unidirectional, sends `MonitorDown` to observer).

### 6. Typed holes

Design already adopted in `docs/typed-holes.md`. Stage 2 is where it
lands.

- `?` and `?name` as first-class expressions/patterns.
- Reports emitted as diagnostics and as `--holes-json`.
- Synthesis bounded to one function-application level; everything
  deeper is the job of the LLM on the other side of the JSON.

### 7. Monomorphised generics

- Every call site of a generic function picks its instantiation; the
  emitter generates one specialised copy per distinct instantiation.
- Stage 1's uniform-boxing approach is retired here. Registers are
  typed; generics are specialised.
- Instantiation keys share structure across compilation (cache),
  keeping binary size reasonable.

### 8. Diagnostics at Elm/Rust quality

Commitment, not a feature: every diagnostic in stage 2 must

- Name the expected type vs the actual type with a visual diff when
  they disagree structurally.
- Suggest a concrete fix where possible ("did you mean `list_map`?",
  "wrap this in `Some(...)`?").
- Point at the root cause, not the symptom; unify propagation
  failures walk back to the origin.
- Include a one-line "why" that explains the rule violated, not the
  mechanism.

This is enforced by treating each new diagnostic as a design item
that lands with its message text, not a TODO.

### 9. Tooling

- `kai fmt`: canonical formatter. Single style, no options. LLM-
  friendly deterministic output.
- `kai repl`: online session with module reload and `?`-hole
  completion.
- `kai lsp`: LSP server talking diagnostics + hover + completion +
  go-to-definition. `--holes-json` doubles as completion source.
- `kai doc`: extract `pub` signatures and doc comments; emit HTML.
- `kai bench`: alongside `kai test`; reuses the test syntax but
  measures time.

## Compilation pipeline

```
.kai source
  → lex        (shared with stage 1 — subset-compatible)
  → parse      (extended grammar: effects, handlers, nursery, holes)
  → desugar    (trailing lambdas, @cap / cap := v, var, a[i] —
                see docs/syntax-sugars.md §Migration and diagnostics)
  → resolve    (module-aware: imports become edges)
  → infer      (HM-extended with effect rows, region branding)
  → monomorph  (instantiate generics, specialise drops)
  → perceus    (reuse analysis, insert incref/decref)
  → lower      (typed IR → LLVM IR or C; CPS transform of effect ops
                lives in this pass — see docs/effects-impl.md)
  → link       (ld / clang wrapper)
```

Each pass is a pure function of the previous AST/IR. A debug dump
between any two passes is writable via `--dump=<pass>`.

## Module system

Phase 4's single-file `--prelude` retires in favour of real imports:

```kai
import math.vector
import math.vector as V
import math.vector.{dot, cross}
```

- Module resolution: a module name `a.b.c` maps to `a/b/c.kai` under
  the project root (or the standard library root).
- Dependency graph is built; compilation is topologically sorted.
  Circular imports are a hard error, explained with a suggestion.
- Each module is compiled to its own translation unit in the output
  (either a `.c` or a `.ll`); the linker glues them.
- Stdlib shipped as a compiled archive plus sources for hacking.

## Test and bench

- `test "..."` — exactly as in stage 1.
- `check "..." with a: Int, b: [Int]` — property test; the runner
  generates random inputs, shrinks failures. The generator is
  derivable for any type whose declarations allow it (all sum/record
  types do by default).
- `bench "..."` — same syntax as test, but the runner times the body
  and reports ns/iter + outlier-robust stats.
- `kai test`, `kai check`, `kai bench` — three subcommands; `kai
  test` runs `test` blocks, `kai check` runs `check` blocks, `kai
  bench` runs `bench` blocks.

## FFI

- `extern "C" fn name(args...) : T / Ffi` — declaration.
- Calling an extern is an op of `Ffi`. Pure kaikai code cannot
  reach it without `Ffi` in its row.
- The `Ffi` "handler" is **compiler-synthesised**, not stdlib
  code: `Ffi` operations lower directly to the C ABI call at
  the declared symbol. There is no user-written clause to run
  (Doc B §`Ffi` *Default handler*).
- `kai bindgen foo.h` — reads a C header, emits an `extern`
  module against the project's conventions. Post-stage-2
  deliverable but keep the door open.

## Bootstrapping

```sh
# fresh machine, only cc
cc stage0/*.c -o kaic0                         # existing
./kaic0 stage1/compiler.kai > /tmp/s1.c
cc /tmp/s1.c -I stage0 -o kaic1                # existing

# stage 2 comes online
./kaic1 stage2/compiler.kai > /tmp/s2.c        # first cross-compile
cc /tmp/s2.c -I stage0 -o kaic2
./kaic2 stage2/compiler.kai > /tmp/s2b.c       # self-compile
diff /tmp/s2.c /tmp/s2b.c                      # optional: fixed-point sanity

# then kaic2 becomes the compiler for everything
./kaic2 demos/my_actor_system.kai -o actor-sys
./actor-sys
```

Once stage 2 is compiled with itself and produces a fixed point,
stage 1 is frozen. Any language-level change lands only in stage 2.

## Milestones within stage 2

High level — each item gets its own sub-design-doc when it comes
up. Milestone numbers are stable (referenced from other docs);
order is indicative and items can land in parallel once 1–4 are
in.

1. **m1 — Stage 2 skeleton**: `stage2/compiler.kai`, CLI, file
   IO, minimal pipeline that calls stage 1's existing pieces
   (lex + parse + check) and emits the same C stage 1 does.
   Proves the wiring.
2. **m2 — HM-extended type checker**: replace stage 1's
   name-resolution-only check with a real inference pass that
   returns a typed AST.
3. **m3 — LLVM IR backend (no optimisations)**: emit `.ll` that
   produces the same output as the existing C backend. All four
   minimal examples round-trip.
4. **m4 — Monomorphisation**: retire uniform boxing; emit
   specialised functions per generic instantiation. Verify perf
   on phase-4-demo workloads.
5. **m5 — Basic Perceus**: reuse analysis + drop insertion in
   the typed IR pass. Measure allocation reduction vs stage 1.
6. **m6 — Module resolution**: cross-file imports, topological
   compilation, standard library loaded from a search path.
7. **m7 — Effects + handlers** (split in two sub-milestones —
   see `docs/effects-stdlib.md` §*Next steps* for the full
   plan):
   - **m7a — mechanics**: row unification, `TyFnT` with
     effect-row slot, CPS transform, handler-stack runtime,
     default handlers for `Console`/`Stdin`/`Env`/`File`/
     `Mutable`/`Cancel`/`Spawn`/`Fail`/`Ffi`, basic
     diagnostics for row-mismatch and effect-not-handled.
     End state: `fn main() : Unit / Console { Console.print("hi") }`
     compiles and runs end-to-end.
   - **m7b — ergonomics**: closed effect aliases (`type Io =
     Console + Stdin + Env + File`), per-operation type
     generics (Doc A amendment), trailing lambdas, `@cap` /
     `cap := v` capability sugar, local mutable cells (`var x
     = init`), array indexing (`a[i]` / `a[i] := v`),
     `Reader[T]` / `Writer[W]` as their own effects.
8. **m8 — Fibers + structured concurrency + actors**: CPS
   scheduler, `Spawn` / `Cancel` effects with default
   handlers, `nursery { n -> ... }` helper, region-branded
   `Fiber[T]`. Same milestone delivers `Actor[Msg]`,
   `Pid[Msg]`, mailbox policies, link/monitor supervision —
   designs in `docs/structured-concurrency.md` and
   `docs/actors.md`.
9. **m9 — Supervisor DSL** (post-actors): `one_for_one` /
   `rest_for_one` / `one_for_all` patterns as a stdlib module,
   built on `Monitor` + `Spawn`. Lands once usage data from m8
   stabilises the right shape.
10. **m10 — Typed holes** *(landed)*: `?` / `?name` expressions
    and patterns, text and `--holes-json` reports. See
    `docs/typed-holes.md` and the validation script
    `scripts/validate_holes_json.py`.
11. **m11 — Diagnostics quality pass**: every error message
    reviewed, rewritten, and tested against an Elm/Rust bar.
12. **m12 — Self-hosting checkpoint**: `kaic2
    stage2/compiler.kai` produces a byte-identical output.
    Stage 1 retired from the dev loop.
13. **m13 — Property testing + bench**: `check` and `bench`
    blocks, matching runners.
14. **m14 — Stdlib expansion**: stage-2-native stdlib,
    module-organised. `Map[K, V]`, `Vector[T]`, `Range[T]`
    (collection-design pass — see
    `docs/proposed-extensions.md` §17, §20).
15. **m15 — `kai fmt`** using the stage 2 parser. Canonical,
    no options (gofmt-style discipline).
16. **m16 — `kai lsp`** using the stage 2 pipeline.
17. **m17 — `kai repl`** using the stage 2 pipeline + holes.

## What stage 2 deliberately does not ship

- Gradual typing, dependent types, refinement types.
- A macro system beyond `nursery`-style sugar that desugars in the
  parser.
- Garbage collection as the default memory strategy.
- Thread-level parallelism that is not fiber-based.
- Distributed runtime. Remote actors are a post-stage-2 research
  project, if at all.
- Self-hosted LLVM backend in pure kaikai; we use the C API from
  kaikai via `Ffi`, same as every other compiler.

The list exists so every new feature request has a clear "yes/no/
later" anchor.
