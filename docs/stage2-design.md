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
5. **m5 — Basic Perceus** *(landed in main as rounds 1-3,
   2026-04-25/26)*: walker scaffold + last-use analysis on fn
   parameters + drop unused fresh-allocation let-bindings + dup/drop
   runtime infrastructure (inert until stage 1 also has perceus).
   Measured impact: -77.4% self-compile allocations via m5 #7
   (constant pool for nullary primitives). Items deferred to a
   follow-up: stage-1 perceus port, runtime linear consumption,
   kai_closure capture incref, m4c real specialisation, LLVM
   mirror of m5 #3 drops, full Perceus optimisations
   (reuse-in-place, drop specialisation, unboxing, regions).
   Tracked as the `perceus`-labelled issues (#77–#82).
6. **m6 — Module resolution** (split in two sub-milestones):
   - **m6.1 — concat semantics** *(landed)*: `import a.b.c`
     loads `<dir>/a/b/c.kai` and concatenates its decls into the
     program. DFS topological order, visited-set for cycles,
     `--path <dir>` repeatable search-path flag. Parser accepts
     `import path as Alias` and `import path.{f, g}` shapes but
     this pass treats them as plain imports — every imported
     symbol lands in the flat global scope. Sufficient for the
     m7e/m7f effect lanes (`import spawn`, `import actor`,
     `import loop`) where the imported file declares effects /
     helpers that the importing file calls **unqualified**.
   - **m6.2 — qualified calls** *(v1 landed 2026-04-27,
     prerequisite for m14)*: real namespacing. `import list`
     makes `list.map(xs, f)` a qualified call distinct from any
     unqualified `map`. Selective `import list.{map}` brings
     `map` into the unqualified scope while leaving the rest
     under the prefix. `import list as L` binds `L.map`.
     Resolver rewrites `EField(EVar(mod), fn)` to a new
     `EModCall(mod, fn)` AST variant when `mod` is in the module
     table and not shadowed locally (rule B: locals shadow
     imports — Rust/Python/OCaml-style). Typer treats `EModCall`
     as a function reference; codegen v1 shares the C symbol
     with the legacy unqualified form. Fixtures live under
     `examples/modules-qualified/{basic, alias, selective,
     local_shadow, two_modules, record_projection}/`.
     Full design + decisions in `docs/m6.2-design.md`. v2
     (universal prefixed minting `kai_<module>__<name>` and
     diagnostic for two-modules-export-same-name) is a follow-up
     lane.
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
   - **m7c — LLVM effects port**: replicate the m7a/m7b
     effects codegen in the LLVM backend. Today
     `llvm_emit_expr` falls through to "unsupported expression
     kind" on `EHandle` — every effect-bearing program is
     C-only. Port `emit_effect_struct`, `emit_handle`,
     `emit_call_expr`'s op-dispatch branch, `emit_clause_*`,
     and the default-handler wrapper to LLVM IR. End state:
     `m7a_*.kai` and `m7b_*.kai` demos round-trip identically
     between `--emit=c` and `--emit=llvm`, and
     `bench-effects` runs against the LLVM-emitted binary as
     well as the C one. Re-measure Doc C OQ #6 — hypothesis:
     ratio drops from m7a's ~4× (vs C-direct on `gcc -O2`)
     toward ~2× because the LLVM optimiser can see through
     the lookup + indirect call when the trivial-clause shape
     is statically visible.
   - **m7d — ergonomic sugars**: five parser-only additions
     from `docs/proposed-extensions.md`:
     - §1 `todo!(msg) : T` — principled unimplemented marker,
       grep-able replacement for `// TODO`, reuses the typed
       holes runtime.
     - §10 record punning `{ x, y }` — drops the
       `{ x: x, y: y }` boilerplate.
     - §14 `@` as-patterns in `match` — bind a sub-pattern
       and the whole scrutinee in one arm.
     - §21 pipeline placeholder `_` — `x |> fn(a, _, b)`
       lands the piped value in arbitrary positions.
     - §23 `++` operator — string and list concatenation,
       migrating away from `string_concat` / `list_append` as
       the idiomatic surface. ~30 lines parser/typer/desugar.
     Independent commits, ~1-2 days total. Lowers the "this
     feels unfinished" perception for seniors trying v1.
   - **m7e — error propagation polish**: two additions from
     `docs/proposed-extensions.md` once `Option` / `Result`
     stabilise in prelude:
     - §13 `!` postfix — `expr!` propagates `Option` /
       `Result` errors to the enclosing `Fail` handler.
       Syntax already reserved in `kaikai-minimal.md`.
     - §11 `variants[T]()` builtin — enumerate sum-type
       constructors at compile time; useful for
       discriminator code and exhaustive testing helpers.
     - §24 `main()` row inference — drop the mandatory row
       annotation on the entry point; typer infers, runtime
       loads defaults from the inferred row. Removes the
       "every demo declares its full effect chain on main"
       papercut.
     - §25 `use Effect` — open an effect's operations at
       file or fn/block scope so call sites can drop the cap
       selector (`println("hi")` instead of
       `Console.println("hi")`). Composes with §24 to give
       the one-line hello-world: `use Console` at file top
       + `fn main() = println("hi")`.
     ~1.5-2 days total. `!` postfix is the highest-value
     pending item after UoM and refinements; without it,
     error handling reads as verbose match boilerplate.
   - **m7f — LLM affordances + method refs**: four
     additions from `docs/proposed-extensions.md` whose
     dependencies are already satisfied (m7a effects, m10
     typed holes, m6 module resolution, m7b post-state):
     - §5 `kai effects <target> --json` — emit the effect
       graph as structured JSON; demonstrable proof of bet B
       (effects as compile-time audit trail) for fintech.
     - §6 `?e` — effect holes complementing `?` (m10) and
       `axiom` (m12.7); rounds out the holes family.
     - §7 `import ?name` — dependency holes that emit JSON
       hints for missing modules; m6 cross-file resolution
       already in place.
     - §19 Method references as values — `options | n.spawn`
       over the explicit lambda; doc explicitly defers this
       to post-m7b, which is the m7f window.
     ~2-3 days total. Independent JSON emitters + parser
     sugar; zero typer changes. Post-m7e, pre-m8.
8. **m8 — Fibers + structured concurrency + actors**: CPS
   scheduler, `Spawn` / `Cancel` effects with default
   handlers, `nursery { n -> ... }` helper, region-branded
   `Fiber[T]`. Same milestone delivers `Actor[Msg]`,
   `Pid[Msg]`, mailbox policies, link/monitor supervision —
   designs in `docs/structured-concurrency.md` and
   `docs/actors.md`.

   **m8 v1 (landed)** ships the full *type surface* and an
   inline-eager scheduler — every spawned thunk runs to
   completion synchronously inside `Spawn.spawn`. The runtime
   swap (real cooperative scheduler, cooperative `Cancel`
   delivery, `BlockSender`, blocking `receive()`, cross-fiber
   Link/Monitor delivery, region brand v1) landed in `0.4.0`–
   `0.21.0` (R2 lane + Tier 2 retrofit); residual typer-side
   items are inventoried in `docs/fibers-honesty-targets.md`
   §*Residual m8.x items*. The user-facing API was the final
   shape from the start, so the runtime swap was invisible to
   user code.
8.5. **m8.5 — Tuples decision gate** *(landed 2026-04-27 — verdict
    REJECT)*: one-day measurement task against the open
    `proposed-extensions.md` §9 decision. Ran over a parser
    combinator suite (n = 7 multi-return parsers); generic-record
    baseline beat tuples on both gates (−2.2% LOC vs ≥10% required;
    +5.8% sig length vs ≥30% required reduction). Verdict and
    methodology pinned in `proposed-extensions.md` §9 "Decision —
    REJECTED (2026-04-27)". Stress-test surfaced three follow-up
    items as the canonical answer (none introduces tuples-as-a-
    type): `Pair[a, b]` in stdlib (1-line shipped), multi-arg
    match sugar (~1 day mini-lane), record destructuring in `let`
    + record punning §10 (already scheduled). Tuples as a second
    product form remain rejected.
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
12.5. **m12.5 — Units of Measure**: F#-style first-class units
    on numeric primitives (`Real<USD>`, `Int<Seconds>`,
    `Real<m / sec^2>`), unit-polymorphic functions
    (`fn area[u: Measure](w: Real<u>, h: Real<u>) : Real<u^2>`),
    abelian-group unification in the typer, units erased before
    codegen. Independent of effects/fibers; benefits the C2
    Fintech toolkit (`Money<USD>` ≠ `Money<EUR>` as a compile
    error). Possibly brought forward if Money/Decimal becomes a
    priority. Full design in `docs/units-of-measure.md`.
12.6. **m12.6 — Refinements + Contracts** *(parser-side landed
    2026-04-27, sub-lanes a–e; see the "Recommended ordering"
    section §7 for what shipped and what remains deferred)*.
    Pony/Ada-style refinement types lite (`Int where >= 0`,
    `String where matches ~r/.../`) + Eiffel/Ada 2012-style
    `requires` / `ensures` clauses on functions. Decidable
    subset, no SMT; static proof where reducible to interval
    propagation + regex subsumption, runtime checks otherwise.
    Closes the third leg of the audit-trail-compile-time pitch
    (effects + refinements + contracts). Composes with m12.5
    (UoM): a single field can carry both `Decimal<USD>` and
    `where >= 0`. Independent of effects/fibers. Full design in
    `docs/refinements-and-contracts.md`.
12.7. **m12.7 — Bootstrap helpers**: `axiom name : T` from
    `docs/proposed-extensions.md` §4 — postulate a typed symbol
    without a definition, useful for stubbing intrinsics, FFI
    declarations, and selfhost scaffolding while a real
    definition is pending. ~0.5 day; reuses the typed-holes
    runtime infrastructure. Optional but cheap.
12.8. **m12.8 — Single-dispatch protocols**: explicit `protocol`
    declarations + `impl` blocks for ad-hoc polymorphism by
    type tag. Modeled on Clojure / Elixir protocols, Go
    interfaces, and Rust traits without higher-kinded types.
    Stdlib ships `Show` / `Eq` / `Ord` / `Hash` / `Serialize`
    with default impls for primitives. `#derive(...)` annotation
    auto-generates structural impls for records and sum types.
    **Not** Haskell typeclasses: no HKT, no constraints in
    signatures, no functional dependencies, no overlapping
    instances; coherence enforced by orphan rule + single-impl
    check. Resolution is `O(1)` per call site, no constraint
    solver. Independent of effects/fibers; subsumes the magic
    `to_string` / auto-derivation pseudo-typeclass discussed
    earlier. Full design in `docs/protocols.md`. ~2-3 days.
13. **m13 — Property testing + bench + bit ops**: `check` and
    `bench` blocks, matching runners. Plus the bit-operations
    module per `proposed-extensions.md` §16: `stdlib/core/bit.kai`
    with `bit.and` / `bit.or` / `bit.xor` / `bit.shl` / `bit.shr`
    / `bit.ushr` / `bit.not` as **compiler intrinsics** (function
    syntax at the surface, lowered directly to the backend op
    with zero call overhead). No new operators. Demand surfaced
    from `demos/9d9l/huffman` bit-packing and the planned `crypto`
    / `encoding` stdlib modules.

    **bit ops chunk landed.** Twelve flat-prefix intrinsics —
    `bit_and` / `bit_or` / `bit_xor` / `bit_not` / `bit_shl` /
    `bit_shr` / `bit_ushr` / `bit_count` / `bit_test` / `bit_set`
    / `bit_clear` / `bit_toggle` — registered in the typer's
    intrinsic table and lowered inline by `emit_call_value` to
    the matching C operator (`<<`, `>>`, `&`, `|`, `^`, `~`,
    `__builtin_popcountll`). Same pattern as `unit_name` /
    `__strip_unit`: no runtime helper, no stage 1 mirror — the
    emitted C never contains the intrinsic name. Documentation
    in `stdlib/math/bits.kai` (header-only); fixture in
    `examples/stdlib/bits_basic.kai`; structural grep in
    `stage2/Makefile` `test-stdlib` asserts the lowering.

    **Dotted `bit.*` surface landed** as a follow-up chunk:
    `bit.and(a, b)` / `bit.or(a, b)` / `bit.xor(a, b)` /
    `bit.not(a)` / `bit.shl(a, n)` / `bit.shr(a, n)` /
    `bit.ushr(a, n)` / `bit.count(a)` / `bit.test(a, n)` /
    `bit.set(a, n)` / `bit.clear(a, n)` / `bit.toggle(a, n)` are
    sugar for the flat-prefix names. `rqc_kind` rewrites
    `EField(EVar("bit"), fname)` to `EVar("bit_" ++ fname)`
    before the m14 ModuleEntry lookup, so the existing emit-time
    intrinsic path handles the dotted form with byte-identical
    C. Fixture `examples/stdlib/bits_dotted.kai`. The
    `check` / `bench` blocks and the auxiliary `bit.*` helpers
    (`leading_zeros`, `trailing_zeros`, `rotate_left`,
    `rotate_right`, plus the ergonomic alias `bit.popcount` for
    `bit.count`) remain open for a future m13 chunk.
14. **m14 — Stdlib expansion**: stage-2-native stdlib,
    module-organised under `stdlib/core/{list,string,option,result,
    char,tuple,ordering}.kai` per `docs/stdlib-layout.md`. The
    file split landed 2026-04-27 (function names still flat:
    `list_*`, `string_*`, …). **m14 v0 + v1 landed 2026-04-28**:
    the user-visible **qualified-call surface** is in place —
    `list.take(xs, n)`, `string.trim(s)`, `option.map(o, f)`,
    `result.is_ok(r)`, `char.is_digit(c)` resolve directly against
    the existing flat-prefix definitions through a prefix-fallback
    in `me_lookup_export` (with per-module legacy overrides for
    `option` -> `opt` and `char` -> `ch`). v0 wired the
    `--prelude` loader to register each prelude file as a
    `ModuleEntry` so the resolver can dispatch qualified calls
    without an explicit `import`. See `docs/lane-experience-m14-v1.md`
    for the full sequence and the discoveries (preludes were not
    modules, the codegen `emit_ident_value` shadowing bug, the
    list-def rename). The shadow bug was fixed in **v1.x**
    (threaded `lcs: [String]` through every C-emit helper,
    mirroring the LLVM backend's `e.locals`); the
    **list rename landed in v1.A** — `pub fn list_take` ->
    `pub fn take` plus 38 sibling ops, internal `*_loop` helpers
    demoted to private, 29 legacy `pub fn list_X = X(...)` aliases
    at the bottom for backward compat. Equivalent renames for
    `string` / `option` / `result` / `char` are deferred —
    bare `repeat` (list vs string), `map` / `and_then` /
    `unwrap_or` (option vs result) collide on `kai_<name>`
    minting and need m6.2 v2 universal prefixed minting first. Adds the missing
    list ops noted in `stdlib-layout.md` §`core.list` (sort,
    sort_by, max, min, count, contains, flat_map, take_while,
    drop_while, repeat, head, tail, uniq, zip_with). Also lands
    the **`print` / `println` consolidation**: the plain builtins
    `print(s)` / `println(s)` (legacy from pre-m7a, before
    `Console` existed as an effect) retire from surface, replaced
    by `Console.print(s)` / `Console.println(s)`. Two forms with
    identical intent collapse into one — the effect-bearing one
    consistent with the rest of the I/O surface. **Status:** the
    consolidation is **deferred** in m14 v1; it is independent
    of the qualified-call work and lands on a separate lane.
    Lands the collection-design pass on top: `Map[K, V]`,
    `Vector[T]`, `Range[T]` (see `docs/proposed-extensions.md`
    §17, §20).
15. **m15 — `kai fmt`** using the stage 2 parser. Canonical,
    no options (gofmt-style discipline).
16. **m16 — `kai lsp`** using the stage 2 pipeline.
17. **m17 — `kai repl`** using the stage 2 pipeline + holes.

## Recommended ordering — the direct path

The `m1, m2, …, m17` numbering is stable for cross-references
but it is **not** the order we are landing them in. After m1–m4
are stable, the recommended path optimises for "kaikai
exists as a usable language as soon as possible" and treats
performance work as a follow-up:

```
m7a → m7b → m7c → m7d → m7e → m7f → m8 → m8.5 → m12 → m12.5 → m12.6 → m12.7 → m12.8 → m5 → full Perceus → m6.2 → m11/m13/m14/m15-17
```

Rationale:

1. **m7a (effects mechanics)** — already in progress. Unblocks
   every subsequent doc (effects, sugars, fibers, actors)
   because they all assume row inference + handler runtime.
2. **m7b (effects ergonomics)** — ships the sugars (trailing
   lambdas, `@cap`/`:=`, `var`, `a[i]`, aliases). The code in
   `effects-stdlib.md` and `actors.md` reads as written only
   after m7b lands.
3. **m7c (LLVM effects port)** — closes the asymmetry the C
   backend opened. Without it, every effect-bearing demo is
   C-only and the bench in m7a #9 cannot speak about the LLVM
   path. Lands before m8 because m8's CPS-reified
   continuations are simpler to add on top of an LLVM backend
   that already handles the m7a/m7b shape.
3.5. **m7d (ergonomic sugars)** — `todo!`, record punning, `@`
   as-patterns, pipeline placeholder `_`. Four parser-only
   commits, ~1-2 days, zero risk. Lands here because the
   mid-flight overhead of sugar additions is lowest while the
   parser is still warm from m7b. Without m7d the language
   reads as "almost there" to a senior trying v1.
3.6. **m7e (error propagation polish)** — `!` postfix on
   `Option`/`Result`, `variants[T]()` builtin. ~1-2 days.
   Lands once `Option` and `Result` are firm in prelude (post
   m7b/m7c). `!` is the highest-value pending sugar after UoM
   and refinements; without it, error handling reads verbose.
   **Update 2026-04-26**: `!` postfix is extracted into its own
   mini-lane (`!`-postfix-only) ahead of m12.8; the rest of m7e
   (`variants[T]()`, main-row inference, `use Effect`) lands after
   m12 selfhost checkpoint. See "Update 2026-04-26 — post-m7b
   reordering" at the end of this section.
3.7. **m7f (LLM affordances + method refs)** — `kai effects
   --json`, `?e` effect holes, `import ?name`, method
   references as values. ~2-3 days. Lands here because all
   four dependencies (m7a, m10, m6, post-m7b state) are
   already satisfied; deferring to m11 would dilute m11's
   diagnostics-quality scope and delay the bet-B fintech demo
   (`kai effects --json` as auditable evidence) by a milestone.
4. **m8 (fibers + structured concurrency + actors)** — closes
   the concurrency surface. The language is "complete" in the
   sense that every promise of the design docs (effects +
   handlers + nurseries + actors + cancellation) compiles and
   runs.
4.5. **m8.5 (tuples decision gate)** *(landed 2026-04-27 —
   verdict REJECT)*. Closed the open `proposed-extensions.md`
   §9 decision against a parser combinator suite. Verdict and
   stress-test follow-ups (Pair[a, b], multi-arg match sugar,
   record destructuring + punning) pinned in §9.
5. **m12 (self-hosting checkpoint)** — `kaic2 stage2/compiler.kai`
   produces a byte-identical output. Stage 1 retires from the
   dev loop. This is the natural moment to evaluate stage 2's
   performance, because once stage 1 is gone, stage 2's speed
   is the development speed.
6. **m12.5 (units of measure)** — F#-style first-class units on
   numeric primitives. Isolated change to the typer/parser; codegen
   intact. Lands here because it is the first concrete fintech
   differentiator (`Money<USD>` ≠ `Money<EUR>` as a compile
   error) and it sits cleanly on a self-hosted stage 2.
   Candidate to bring forward if Money/Decimal becomes a priority
   before m12. Full design in `docs/units-of-measure.md`.
7. **m12.6 (refinements + contracts)** *(landed parser-side
   2026-04-27 — see m12.6.a–e below for what shipped, and the
   "deferred" list for what remains)*. Refinement types lite
   (`Int where >= 0`, regex predicates) + Eiffel-style `requires`
   / `ensures` on functions, decidable subset (no SMT). Lands
   right after m12.5 because the two share most of the typer
   machinery (predicate evaluator, runtime check insertion);
   sequencing them adjacent amortises the design overhead. Closes
   the audit-trail-compile-time pitch with refinements +
   contracts as the third leg next to effects in types. Full
   design in `docs/refinements-and-contracts.md`.

   Sub-lanes shipped:
   - **m12.6.a** — `TyKind | TyRefine(TypeExpr, Expr)` AST + parser
     for `BaseT where Pred`. The leading-comparator form
     (`where >= 0` desugars to `where self >= 0`) lands at parse
     time. `resolve_ty` drops the refinement so the semantic type
     is unaffected; walker arms in 11 functions over `TyKind` keep
     the refinement transparent for display, alias expansion, fiber
     detection, implicit-tparam collection, protocol dispatch, and
     `Self` substitution.
   - **m12.6.b** — `TkRequires` / `TkEnsures` keywords +
     `parse_contracts_loop` in `parse_fn_decl`. Each clause is a
     single `Expr` (terminating naturally at the next clause / `=`
     / `{`); both lists desugar to asserts that wrap the body. A
     violated precondition aborts before the body runs; runtime
     check is `kai_assert_check` from the existing assert path.
   - **m12.6.c** — `let result = body` shape, so `ensures result
     > x` and friends work end-to-end. The `mk_pat(...)` argument
     is constructed inline at the SLet call site to dodge a kaic1
     codegen quirk where a local Pattern binding inside a nested
     if/else gets emitted as a closure thunk; selfhost masks the
     bug because compiler.kai itself never invokes this branch.
   - **m12.6.d** — Constant folding over contract predicates. The
     `try_eval_pred` / `try_eval_int` evaluator covers Bool / Int
     literals, the boolean operators (`and`/`or`/`not`), Int
     comparisons, and Int arithmetic between literals. Trivially-
     true predicates are dropped at parse time (zero runtime cost).
     False-evaluating predicates still emit the assert (the runtime
     panic remains the diagnostic; promoting to compile-time error
     is a follow-up).
   - **m12.6.e** — `ensure(value) where pred` primary expression
     desugars to `{ let self = value; if pred { Some(self) } else
     { None } }`, returning `Option[T]` for explicit narrowing.
     Recognised only when `ensure(...)` is followed by `where`, so
     user-defined `ensure` functions are unaffected.

   Deferred from the original m12.6 plan. #1, #3, #4, #6
   landed; remaining open items tracked as GitHub issues:
   - **#1 `TyRefineT` semantic type** — landed 2026-04-27.
   - **#2 Static interval propagation** through refined
     arguments — v1 landed; full version (alpha-equivalent
     matching, operator rewriting, call-site substitution)
     pending in issue #83.
   - **#3 Match-arm narrowing** (`p : RefinedT`) — landed.
   - **#4 UoM composition** (`Decimal<USD> where >= 0`) —
     landed.
   - **#5 `[<refinement-pure>]` stdlib annotations** —
     enforcement + attribute parser landed; inline placement
     (lexer disambiguation) pending in issue #84.
   - **#6 Compile-time errors** for trivially-false
     predicates — landed.
   - **#7 Regex literals** (`String where matches ~r/.../`) —
     stdlib regex engine landed 2026-04-28
     (`stdlib/regexp.kai`, RE2-style); the Elixir-style sigil
     `~r/.../` + `matches` predicate-pure helper landed
     2026-05-03 (lane closing issue #85). Subsumption (static
     containment between regex literals) deferred to a
     follow-up issue.
   - **#8 Diagnostics quality** (predicate-aware panic
     messages) — v1 landed; structured violation context
     pending in issue #86.
7.5. **m12.7 (bootstrap helpers)** — `axiom name : T`. Optional,
   ~0.5 day. Useful for stubbing intrinsics and FFI declarations
   while their real definitions land. Lands here because m12
   self-host is the first time selfhost-shaped scaffolding
   becomes load-bearing.
8. **m5 (basic Perceus in the typed IR)** — reuse analysis +
   drop insertion. Lands now because it directly improves the
   self-compile speed and validates the IR design under load.
9. **Full Perceus** (§2 — reuse-in-place, drop specialisation,
   unboxing, opt-in regions) — the heavy memory work, scheduled
   after the basic pass has stabilised.
9.5. **m6.2 (qualified module calls)** — namespacing on top of
   m6.1's concat semantics. `import list` makes `list.map` a
   qualified call distinct from any unqualified `map`. Required
   before m14 can rename `list_map` → `list.map`; without it
   the rename is a syntax error. Independent of effects/fibers.
   Lands directly before m14 so the migration is the immediate
   payoff.
10. **m11 (diagnostics quality)**, **m13 (property/bench)**,
   **m14 (stdlib expansion)**, **m15–m17 (tooling)** — these
   are mostly independent and can land in parallel once the
   above is done. m11 in particular benefits from being able
   to run end-to-end on the full feature set. m14 specifically
   blocks on m6.2 (see 9.5).

Why this order and not "Perceus first":

- m7a/m7b/m7c/m8 deliver the **language**. Without them, stage 2
  is a self-hosted kaikai-minimal with extras — not the language
  the design docs describe. Performance is a constant-factor
  improvement on top of an existing language, not the language
  itself.
- The current RC scheme (stage 0's uniform boxing + manual
  incref/decref) is **functionally complete**. Programs run
  correctly; they are just slower than they will be after m5.
- m12 is the natural quality gate. Sliding Perceus before m12
  means doing the work twice (once on a partial language, once
  after fibers/actors land).
- Items beyond m12 (m5, full Perceus, m11/m13/m14, tooling) can
  be parallelised much more freely than the m7a→m7b→m7c→m8 chain,
  which is sequential by nature (each depends on the previous).

This ordering is recorded in `docs/stage1-design.md` §Features
item 6 (basic Perceus deferred from stage 1 to stage 2 m5) and
mirrored in this section so the next contributor can find the
critical path without re-deriving it from the milestone list.

### Update 2026-04-26 — m12.8 landed

`m12.8 — Single-dispatch protocols` shipped. `protocol`, `impl`, and
`#derive(...)` syntax compiles end-to-end on both backends; selfhost
and `selfhost-llvm` are still green and the new `test-protocols` suite
covers Show/Eq/Ord/Hash/Serialize positives plus orphan + duplicate-impl
negatives. Stdlib protocols live in `stdlib/protocols.kai` (loaded via
`--prelude`) — folding them into the default prelude requires teaching
stage 1's compiler to parse the new keywords, which is the
**compiler-cleanup** lane immediately after this one.

The vigente order (below) now reads:

```
m12.8 ✅ → compiler-cleanup → m12 → m7e (rest) → m7f → m5.x → m8.5
  → m12.5 → m12.6 → m12.7 → full Perceus → m6.2 → m11/m13/m14/m15-17
```

See `docs/lane-experience-m12.8.md` for the lane retrospective and the
v1 limitations the cleanup lane will need to factor in (return-type
dispatch via `from_string`, scope-aware renaming, vtable emission for
genuinely-late-binding sites).

### Update 2026-04-26 — post-m7b reordering

After m7c, m7d, and the m7b #15 scope-limited follow-up landed in
main, the path forward was re-evaluated. The original ordering
above is preserved for historical context; the **vigente order
from this point** is:

```
!-postfix → m12.8 → compiler-cleanup → m12 → m7e (rest) → m7f
  → m5.x → m8.5 → m12.5 → m12.6 → m12.7 → full Perceus
  → m6.2 → m11/m13/m14/m15-17
```

Two changes vs the original ordering:

1. **`!` postfix split out of m7e** as a 0.5-1d mini-lane and moved
   ahead of m12.8. Rationale: it is a pure desugar with zero
   bootstrap risk; landing it now removes ~108 lines of
   `match Some(x) -> ... None -> return None` cascades from the
   stage 2 compiler, which every subsequent agent benefits from.
2. **m12.8 (single-dispatch protocols) moved ahead of m12** instead
   of after m12.7. Rationale: the Tier 3 "LLM authorability"
   argument has present-tense traction — the compiler is being
   built using LLM agents (Claude), and each agent that adds a new
   compiler type today writes ~30 lines of `dump_*` / `eq_*` /
   `hash_*` boilerplate. With `#derive(Show, Eq, Hash)` shipped
   first, that drops to one line. Demos are already rewritten
   (`d1e909c`) using `#derive` and `impl Show`; landing m12.8 makes
   them compile instead of acting as aspirational syntax.

Implementation guard: m12.8 protocols ship as O(1) lookup table
without polymorphism (single-dispatch only, no constraint solver).
Stage 1 must compile them, so the dispatch table mirrors the
existing evidence-passing infrastructure for effects.

A **compiler cleanup pass** lands immediately after m12.8 stabilises:
convert ~150 lines of hand-written `dump_X` / `eq_X` / `hash_X`
functions to `impl Show for X` / `impl Eq for X` / `impl Hash for X`.
This pass is the immediate dividend of m12.8; it cannot run before.

The **m12 self-hosting checkpoint** then validates the cleaned-up
compiler. Doing the cleanup first means the checkpoint baseline
is the smaller, deduplicated compiler — which is what selfhost will
re-verify on every subsequent change.

After m12 the rest of m7e and m7f run in either order, since both
their dependencies (firm `Option`/`Result`, `kai effects --json` on
top of m7c emit) are satisfied.

Total estimated days for the reordering block (`!` postfix + m12.8
+ cleanup + m12): **3.5 - 6.5 days**, of which `!` postfix and
cleanup are nearly mechanical. m12.8 itself dominates the variance.

### Update 2026-04-26 — m12.5 (units of measure) landed

Implemented in stage 2 ahead of the post-m7b ordering above (which
had m12.5 sitting after m8.5). Lane brought forward as part of the
m12.5 spike — independent of effects/fibers and contained in the
typer + parser + AST extensions; codegen emits identical C/LLVM IR
with or without unit annotations. Roughly +1100 lines added to
`stage2/compiler.kai`. Fixtures land in `examples/units/`. Full
landing notes in `docs/units-of-measure.md` (Status: Landed at the
top); lane experience report in `docs/lane-experience-m12.5.md`.

### Update 2026-05-03 — hex and binary integer literals landed

PR #160 (closes #156) added `0x` and `0b` lexer branches to the
integer-literal token. Same `Int` type, same `EInt` AST node — no
new node, no new type, no new operator, no parser change. Range
is signed 64-bit. No underscore digit separators in the hex/bin
branches, no octal `0o`, no hex floating-point literals. Full
spec in `docs/syntax-sugars.md` §7; lexer code in
`stage2/compiler.kai` (`lex_hex_or_bin_int`); fixtures in
`examples/literals/`.

The companion landings on the same day — regex sigil `~r/.../`
(PR #159, closes #85) and n-tuple parser sugar `(a, b)` /
`(a, b, c)` / `(a, b, c, d)` (PR #155, closes #154) — already
update their respective docs in place. UoM annotation in record
field positions (issue #158, PR #163) is still in flight and
will be documented when it merges.

### Milestone naming: Core language vs Full language

The project adopts two named milestones to disambiguate from the
informal "MVP" labels used in earlier planning snapshots.

**Core language** — closes when ALL of the following landed:

- Stages 0/1/2 self-host (m1-m6), all with selfhost-llvm fixed-point
  green.
- Effects + handlers + parametric effects + per-instance dispatch
  (m7a/b/c).
- Ergonomic sugars: trailing lambdas, `@cap`, `var`, `a[i]`, aliases,
  `todo!`, record punning, `@` as-patterns, pipeline `_`, `++`, `!`
  postfix on `Option`/`Result` (m7b/d/e§13).
- Fibers, structured concurrency, actors with supervision (m8).
- Typed holes (m10).
- Perceus basic memory management (m5 rounds 1-3) plus capture incref
  (m5.x #2). Linear-consumption runtime is deferred follow-up
  (`m5x-1-flip`); current Perceus pass is functionally correct under
  loose runtime, optimisation only.
- Units of measure (m12.5).
- Single-dispatch protocols + 5 stdlib protocols + `#derive(Show, Eq,
  Hash)` for records and sum types (m12.8 + m12.8.x), plus
  `#derive(Ord)` (m12.8.z).
- Protocol feature validation: a fixture that exercises Show / Eq /
  Hash / Ord derives on records and sum types whose shapes mirror
  the compiler's internal data structures
  (`examples/protocols/m12_8_compiler_shapes.kai`). This is the
  honest substitute for the original "compiler cleanup" plan in
  `687f1f3`, which presupposed hand-written structural Show / Eq /
  Hash functions in `stage2/compiler.kai` that an audit
  (`docs/lane-experience-m12.8-cleanup.md`) showed do not exist —
  the compiler's `dump_*` family is depth-indented AST printers,
  not structural Show; its `*_eq` helpers are intentionally partial
  (e.g. `row_label_eq` ignores args by design); `hash_*` does not
  appear at all. A second structural blocker is that the selfhost
  pipeline cannot import `stdlib/protocols.kai` without rebaselining
  the byte-identical fixed-point, so `#derive(Show)` inside
  `compiler.kai` itself would fail with `unknown protocol Show`.
  Validation moved out to the dedicated fixture instead, and to the
  in-flight external demos (`/tmp/kaikai-portfolio-demo`).
- m12 self-host checkpoint: byte-identical fixed-point of
  `kaic2 stage2/compiler.kai`.
- Both backends (`--emit=c`, `--emit=llvm`) at parity for every
  shipped feature.

State as of 2026-04-26: m12.8 protocols + #derive(records) landed
(`1cf1183`); m12.8-cleanup round 1 in flight; m12.8.x and round 2
queued; m12 checkpoint pending.

**Update 2026-04-27 — Core CLOSED.** Every Core item above is in
place. m12.8.x merged (Phase 1+2+3+4: sum-type derives; Bugs 1-5 +
Gap 1; Bug 6 effects-prelude gap with the main-row exception
preserved per pinned decision; followup `49b4bd7` after the merge
restored that exception). m12.8.z merged (`#derive(Ord)` for records
and sum types; followup `f8a2881` aligned the lane with Phase 4's
row enforcement). Bug 8 (`unitless` leak, `f0acda2`) and the
multi-prelude support (`6be885f`) shipped on the same wave. The
protocol-validation fixture
`examples/protocols/m12_8_compiler_shapes.kai` exercises Show / Eq /
Hash / Ord derives on records and sum types whose shapes mirror
`compiler.kai`'s `Pos` / `Span` / `TokKind` / `Token` carriers; it
passes both backends. Selfhost C + LLVM fixed-point both green;
`test-llvm-coverage` 60 pass / 0 DIFF / 14 skip; the m12 self-host
checkpoint is implicit in those gates.

The original "compiler cleanup" plan in `687f1f3` is retired. The
audit in `docs/lane-experience-m12.8-cleanup.md` showed the
estimated dividend (150 LOC of hand-written `dump_*` / `eq_*` /
`hash_*` to convert) was speculative — the compiler does not contain
structural Show / Eq / Hash to convert. A second structural blocker
showed up later: the selfhost pipeline does not load
`stdlib/protocols.kai`, so `#derive(Show)` inside `compiler.kai`
fails with `unknown protocol Show`. The validation that the cleanup
was meant to provide moves to the new fixture above.

**Update 2026-04-28 — Core REOPENED for stronger demo-based
validation.** On reflection, the fixture-based validation in
`m12_8_compiler_shapes.kai` is weaker than the demo-based validation
the original plan implied. The user's preferred Core-close criterion
is "the aspirational external demos compile and produce the expected
output", concretely:

- `portfolio.kai` (57 lines, post-Phase-4 target): records + sum types
  + `#derive(Show)` + UoM + per-stream effect rows + pipes;
- `usd_to_eur.kai` (58 lines, post-Phase-4 target): `Stdin` and
  `Stdout` as separate per-stream effects + UoM literal/algebra +
  apply-pipe + `read_line`-based parsing.

Two structural items must land before either demo compiles:

1. **Stdout / Stderr atomic split** (defer-on-Phase-4 from
   `docs/m12.8-followup.md` — retired 2026-05-02; see git history).
   Today `Console` is the combined effect
   that owns both `print` (stdout) and `eprint` (stderr). Both demos
   require declaring `/ Stdout` (and `usd_to_eur.kai` also `/ Stdin`
   alongside) as atomic effects. Migration: split `Console` into
   `Stdout` + `Stderr`, keep `Console = Stdout + Stderr` as an
   alias for over-declaration.
2. **Parametric `impl Show for Real[u: Measure]`** (Gap 1 partial in
   `docs/m12.8-followup.md` — retired 2026-05-02; see git history).
   Today `unit_name(x: Real<u>)` is a
   reflection intrinsic that gives the suffix string; the parametric
   impl that lets `#{balance(txs)}` of `Real<USD>` render as
   `"845 USD"` requires per-unit specialisation in the protocol-impl
   resolver.

After both land, the two demos move from `/tmp/kaikai-portfolio-demo/`
into `examples/portfolio/` and `examples/usd_to_eur/` (or similar)
with `.out.expected` golden files and harness wiring. The
`m12_8_compiler_shapes.kai` fixture stays as a smoke test for the
derive surface.

Banking-style demos (`/tmp/henua-example/`) are NOT in Core scope —
they require `ahu` (OTP-analog) and `henua` framework layers that
land post-Core (~5-6 weeks total). Banking is validation of "Core +
framework", not Core alone.

Estimated time to Core CLOSED under this revised criterion: 4-6 days.

**Update 2026-04-27 (final) — Core CLOSED (revised criterion).**
Both structural items above are in. The Stdout / Stderr atomic split
landed in m12.8 Phase 4b (`8b134bb`, `54d5364`, `ee49db2`), with
`Console = Stdout + Stderr + Stdin` and `Io = Console + Env + File`
shipping as effect aliases plus alias-aware op resolution. The
parametric `impl[u: Measure] Show for Real<u>` landed in m12.8 Phase 2
(`81e0306`) — the v1 takes a call-site rewrite shortcut around the
still-identity m4c monomorphiser, but the user-visible behaviour
matches the original ask (`#{x : Real<USD>}` renders as `"100 USD"`).
Both demos are in the repo:

- `examples/portfolio/portfolio.kai` + `portfolio.out.expected`
- `examples/usd_to_eur/usd_to_eur.kai` + `usd_to_eur.in` + `usd_to_eur.out.expected`

Wired into `make test` via the new `stage2/Makefile :: test-demos-core`
target. The diff is exact (post-Phase-2 output, not the README's
aspirational format — the `.out.expected` files capture what the
compiler actually produces, e.g. `EUR * 1 / USD` for the canonical
form of `EUR/USD` from `unit_expr_display`). Demos remain editable
only via the goldens; the .kai sources are immutable per the review
note ("when the compiler reaches the demo, the demo wins").

`m12_8_compiler_shapes.kai` stays as the smoke test for the derive
surface. `test-demos-core` is the demo-based gate the user asked for.
Both runs the C and LLVM backends; selfhost C + LLVM remain at fixed
point.

**Update 2026-04-27 (later, same day) — Core REOPENED.**
Within hours of running `make -C demos verify` over the broader probe
set in `demos/`, four typer/resolver/codegen bugs surfaced:

1. **Handler-clause alias resolution gap** (`10a1e7b`):
   `synth_handle` reconstructed `EHandle` with the original (un-typed)
   clauses, discarding the typed body returned by `check_clauses_types`.
   `Console.print` inside `raise(resume) -> { ... }` emitted
   `kai_apply(kai_field(kai_Console, "print"), ...)` against an
   undeclared symbol.
2. **Protocol-op shadows local parameter** (`257b247`): `rename_proto_calls`
   rewrote every `EVar(name)` matching a registered op without lexical
   scope tracking. `list_sort_by(cmp)` had its `cmp` parameter rewritten
   to the dispatcher; runtime panicked with `__protocol_dispatch_Ord_cmp`.
3. **Lambda rows closed by default** (`b528655`): `synth_lambda` built
   lambdas with `Row { tail: None }`, so `while(pure_pred,
   effectful_body)` failed type-check — the first lambda's empty closed
   row pinned `while`'s row var `e` to empty, then the second lambda's
   `[Stdout]` row could not unify.
4. **Closure capture of var-bound aliases** (`4c61184`): `fv_expr` /
   `collect_expr` collected handler aliases (`var top = 0` desugars
   to `with State[Int](0) as top`) as captured values; the C emit
   read `kai_top` against an undeclared identifier.

Linus and Eric agents reviewed the codebase and converged: the gate
("two demos compile + selfhost + 61-fixture suite") was extrinsic
(observable output) without any intrinsic invariant (typer/resolver
properties). All four bugs are instances of the same systemic
failure: a 22000-line single-file compiler with no shared AST visitor;
each pass duplicates the structural recursion manually and each must
independently remember every invariant other passes already enforce
(thread typed sub-expressions back, track lexical scope, give
lambdas open rows). The two cherry-picked demos exercised neither
the handler-clause path nor `list_sort_by`-style higher-order
helpers nor row-polymorphic `while` callers nor lambda-with-alias
capture.

**Revised closure criterion** — three levels:

1. **Automated, mechanical** (existing, unchanged): `make selfhost`
   + `make -C stage2 selfhost-llvm` byte-identical fixed point;
   `make test` clean (incl. `test-demos-core`).
2. **Audit + invariant verifier**: every `_ -> k` wildcard in every
   AST walker pass must be either (a) justified ("this kind cannot
   contain sub-expressions"), or (b) made explicit. Three rep
   invariants must be verifiable by walkers: (i) every reconstructed
   `EHandle` uses typed clauses, not the originals; (ii) every
   `EVar(name)` rewritten by `rename_proto_calls` has `name` outside
   every enclosing binder; (iii) every `TyFnT` produced by
   `synth_lambda` for an inferred row has an open tail.
3. **Demo gate**: `make -C demos verify` produces no new FAIL relative
   to the milestone start; remaining FAIL must each be blocked by a
   documented roadmap item, not by a compiler bug. Plus: at least one
   aspirational program *not* in the milestone plan must compile, to
   force the designer to look outside the contemplated paths.

The "two demos compile" gate is retired. The new gate runs at every
milestone close, not just at Core.

Estimated time to Core RE-CLOSE under the revised criterion: 2-4
days, dominated by the audit (~1 day) + invariant walkers (~1-2
days) + addressing the four lurking suspects Linus identified by
inspection (`desugar_interp_decls` no scope tracking,
`pcs_rewrite_kind` `ELitUnit` fallthrough, `rename_proto_calls_kind`
`ELitUnit` fallthrough, `synth_lambda` doesn't clear
`clause_resume`). See `docs/m12.8-followup.md` post-Core section
for the live tracking (file retired 2026-05-02; see git history).

**Update 2026-04-27 (post-REOPEN final) — Core RE-CLOSED.** All
three levels of the revised gate are met:

- **Level 1** (automated, mechanical): `make selfhost` + `make -C
  stage2 selfhost-llvm` byte-identical fixed point; `make test`
  clean (incl. `test-demos-core` and the new `test-aspirational`).
- **Level 2** (audit + invariant verifier): every `_ -> k`
  wildcard in every AST walker pass justified or made explicit
  (`7bd5a68`); rep invariant (b) — protocol-dispatcher references
  outside enclosing binders — runs on every compile via
  `validate_typer_invariants` (`0afc2a9`); invariants (a) and (c)
  documented as construction-site properties, not post-finalise
  walkers. The four lurking suspects Linus called out are fixed
  (suspects 2/3/4) or analyzed as false-positive (suspect 1).
- **Level 3** (demo gate + unplanned program): `make
  demos-no-regression` enforces a baseline of 18 OK + PASS demos
  in `demos/`; `make -C stage2 test-aspirational` runs
  `examples/aspirational/event_logger` — a custom Logger effect
  + handler + `each` with effectful callback — that exercises
  features the milestone-planned demos did not (custom effect
  declaration, effectful HOF callbacks, sum-type-with-payload
  `#derive(Show)` interpolation).

The `map_expr_kind` shared visitor refactor Linus floated is
explicitly **not** part of the gate — CLAUDE.md "no premature
abstraction" rules it out. The combination of the wildcard audit +
the rep invariant walker covers the bug class the abstraction
would prevent without imposing a new compiler-wide pattern.

Recap of all four bugs found post-the-original-CLOSED (in the
order they surfaced): handler-clause alias rewrite (`10a1e7b`),
scope-blind protocol rename (`257b247`), closed lambda rows
(`b528655`), closure-captured handler aliases (`4c61184`). All
fixed; all defended by either the audit, the runtime invariant
walker, or a regression fixture.

What is **not** in Core but is part of Full:

- ~~Remaining m7e items~~: all three landed 2026-04-27 —
  `variants[T]()` (Full lane 1.3, `docs/proposed-extensions.md`
  §11), `use Effect` (Full lane 1.4, §25), and `main()` row
  inference (Full lane 1.5, §24, commit `7e36027`).
- ~~m7f LLM affordances~~ (landed 2026-04-27 as Full lane 2.3 —
  see `docs/proposed-extensions.md` §5 `--effects-json`, §6 `?e`
  effect holes, §7 `import ?name`, §19 `.field` method-ref
  placeholder; the general `obj.method` ref is v2).
- Refinement types and Eiffel-style contracts (m12.6).
- `axiom` declarations (m12.7).

(String interpolation auto-Show via `#{x}` was originally listed
here. It actually shipped earlier as m12.8.x phase 2 / Bug 4 fix
(`4b137d4`): `desugar_interp_decls` lifts every `#{e}` body into
`__proto_show(e)` ahead of inference, so the post-inference rewrite
routes each call to the user's `impl Show`. Fixture
`examples/protocols/m12_8_y_interp_show.kai`. No further work.)

**Full language** — Core + the items above. Does not include
post-MVP bets (LSP server, package manager, WASM target, full
Perceus optimisation, m11 diagnostics polish, m13 property/bench,
m14 stdlib expansion, m15-17 tooling).

The "MVP B" / "MVP D" labels in earlier snapshots map to:

- "MVP B" → **Core language** (this section).
- "MVP D" → **Full language** (Core + remaining items above).

## Full prerequisites (carried from Core post-mortem)

These are not features but structural debt items that must land
before specific Full lanes start. Identified during the
post-Core REOPEN by `linus` and `eric` agents (independently);
deferred from Core because they had no feature upside on the
Core lane itself.

- **`map_expr_kind` shared AST visitor refactor — LANDED
  2026-04-27.** The 10 walker passes catalogued in `7bd5a68`
  (`rewrite_alias`, `desugar_op_calls`, `desugar_index`,
  `specialise`, `desugar_var`, `desugar_interp`, `finalise_typed`,
  `pcs_rewrite`, `rename_proto_calls`, `resolve_protocol_calls`)
  all delegate their structural recursion to `map_expr_kind` and
  its sibling helpers (`map_stmt_exprs`, `map_arm_exprs`,
  `map_field_exprs`, `map_elem_exprs`, `map_clause_exprs`,
  `map_opt_return_exprs`, `map_return_exprs`, `map_opt_expr`).
  Each walker keeps explicit cases only for kinds it actually
  transforms or that need scope-aware bookkeeping; everything
  else falls through to the visitor. Adding a new `ExprKind`
  variant (m7e `variants[T]()`, m12.6 refinement carriers, future
  actor primitives) now breaks the compiler at the visitor's
  match site instead of silently degrading ten walkers.
  Net: −1031 / +236 lines in `stage2/compiler.kai`, 12 commits
  (1 abstraction + 10 walker migrations + this doc + cleanup).
  Selfhost C + LLVM byte-identical fixed point at every step;
  `make demos-no-regression` (baseline 18) clean;
  `make -C stage2 test-aspirational` clean. Walker invariant (b)
  in `validate_typer_invariants` still passes.

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
