# kaikai roadmap

Pinned 2026-05-02 (post v0.30.0). Names follow the Rapa Nui
convention already in use across the project (the language
`kaikai` itself, the framework `ahu`, the web framework
`manutara`). Each milestone is a real Rapa Nui site; the
sequence Tongariki → Anga Roa → Orongo → Anakena tracks the
arc from public face → daily life → ceremonial culmination →
horizon beyond.

## Status snapshot

- **HEAD**: `0.31.0` (R9 closed + R10/R11 diagnostic + lane handoff
  authorization)
- **Current target**: kaikai-Tongariki Wave 3 — m8.x disclaimer
  sweep + residual items (issue #59). The runtime side of m8.x —
  cooperative scheduler, cancel delivery, blocking mailbox
  primitives, Link / Monitor / trap-exit — landed in `0.4.0`–
  `0.21.0`; the open work is documentation alignment plus two
  smaller residual items (full TyBranded region brand, per-op ROW
  generics on Spawn). Waves 1, 2, and the MVP cierre shipped at
  `0.24.0`–`0.27.0`.
- **Cross-cutting principles** (per CLAUDE.md):
  - Tier 1 #1 (memory safety + effects in types) — defendible
  - Tier 1 #2 (mandatory TCO) — defendible without footnote as of 0.23.0
  - Tier 1 #3 (fast compilation) — defendible
- **Inner-loop perf**: ~2.0× C reference under -O2 (post Real
  unboxing in 0.27.0). Target trajectory: post-Tongariki ~2.0× →
  Anga Roa ~1.5–1.7× → Orongo near-C with multi-thread + Tier 3b
  unboxed messages.
- **Selfhost**: byte-identical at fixed point under both C and LLVM
  backends.

## Milestones — kaikai language

Each milestone has scope, definition-of-done, estimated cost,
and one optimization thread (so perf debt does not pile up to
the end).

### Tongariki — MVP

The 15-moai platform: the public face. Ship the language with
the minimum testing and dev workflow that lets early adopters
write real code. Delivered in waves; the MVP cierre (`0.27.0`)
closed the original "ship a usable language" promise. The m8.x
cooperative scheduler runtime that backs the actor and structured-
concurrency surfaces shipped in `0.4.0` (R2 lane); **Wave 3**
(issue #59) is the documentation sweep that brings stdlib and
roadmap text in line with that runtime, plus the residual
typer-side items (full `TyBranded` region brand, per-op ROW
generics on Spawn).

#### Wave 1 (`0.24.0`, shipped 2026-05-01) — `kai fmt` + TCO stage 1

- `kai fmt` — gofmt-style deterministic formatter, no options.
- TCO stage 1 mirror (issue #42) and precise per-call-site
  dropmask cons-cell case (issue #43). Housekeeping from 0.23.0.

#### Wave 2 (`0.25.0`, shipped 2026-05-02) — `bench` v1

- `bench` v1 — bench blocks reporting mean ns/iter (no stats),
  `kai bench` runner. Issue #40.

#### Cierre (`0.26.0`–`0.27.0`, shipped 2026-05-02) — `check` v1 + Real unboxing

- `check` v1 — property test blocks with intrinsic-table
  generators (no shrinking), `kai check` runner. Issue #44.
- Drop specialisation — per-type drop chains inlined at emit
  time instead of going through runtime dispatch.
  **Closed 2026-05-01 as doc-only PR (negative result).**
  End-to-end implementation measured −1.7% at `-O2` and a
  +5.4% regression at `-O0` on `kaic2` self-compile — both
  well below the lane's ≥10% success threshold. Phase 2
  unboxing had already absorbed the addressable overhead.
  Re-evaluation is paired with the Anga Roa
  **reuse-in-place** lane, which may shift the alloc mix
  enough to unlock per-tag dispatch on
  KAI_RECORD / KAI_VARIANT. Retro pinned in
  `docs/lane-experience-drop-specialisation.md`.
- **Real unboxing (Phase 2 follow-up)** — pulled forward
  from `docs/unboxing-phase2-followup.md` after drop
  specialisation closed as negative result. Brings
  Real-heavy programs from the prior ~50–100× C-ref gap into
  the same band as Int/Bool/Char (~5–10× post Phase 2). Same
  unbox_pass machinery + new C type emission for `MUnboxed Real`.
  Replaced drop specialisation as the Tongariki optimization
  thread.

#### Wave 3 — m8.x disclaimer sweep + residual items (issue #59)

The cooperative scheduler runtime landed in `0.4.0` (R2 lane,
2026-04-28): `swapcontext`-based fiber substrate, intrusive ready
queue, blocking mailbox primitives (Phase 4 — `Actor.receive()` on
empty mailbox parks the caller; `Bounded(c, BlockSender)` parks the
sender on full), Cancel delivery at every effect-op lookup
(Phase 3 — `kai_check_cancel_yield_point` + trampoline `cancel_pad`),
Link runtime (Phase 5), region-brand v1 (Phase 6 shallow check). The
Tier 2 items (Monitor, trap-exit, LLVM `in_dispatch_node` mirror,
per-op TYPE generics on Spawn) followed in `0.5.0`–`0.21.0`. End-to-
end fixtures cover every cooperative semantic: see
`examples/effects/m8x_2_yield_interleave.kai`,
`m8x_3_cancel_at_yield.kai`, `m8x_4_recv_blocking.kai`,
`m8x_4_request_reply.kai`, `m8x_4_block_sender.kai`,
`m8_monitor.kai`, `m8_trap_exit.kai`, `m8_spawn_per_op_generics.kai`,
`m8_fiber_stack_overflow.kai`, plus the multi-actor `demos/ping_pong/`
demo (`demos/baseline.txt = 24` no-regression gate).

What was open after `0.21.0` was a documentation gap: the headers in
`stdlib/actor.kai`, `stdlib/spawn.kai`, and a pair of paragraphs in
`stage0/runtime.h` still described the pre-`0.4.0` inline-eager
runtime. That gap drives Wave 3.

**Scope**:

- **Disclaimer sweep**. Bring `stdlib/actor.kai`, `stdlib/spawn.kai`,
  and the leftover paragraphs in `stage0/runtime.h` (above
  `KaiMboxNode` and the `KAI_OVERFLOW_*` defines) in line with the
  cooperative scheduler that has been in `main` since `0.4.0`.
  Same PR adds `examples/effects/m8x_4_request_reply.kai` for
  explicit two-actor request/reply round-trip coverage.
- **Residual item — full `TyBranded` region brand.** The shallow
  `ty_expr_contains_fiber` / `ty_expr_contains_pid` walk catches
  Fiber and Pid handles only when they are surface-level in the
  return type. Sum-type-wrapped escapes (e.g.
  `type Boxed = Wrapper(Fiber[Int])` returned from a non-stdlib
  helper) and brand-mismatch detection between sibling nurseries
  need the `TyBranded(Ty, BrandId)` propagation pinned in
  `docs/structured-concurrency.md` §*Type system*. Tracked in
  `docs/fibers-honesty-targets.md` §*Residual m8.x items*.
- **Residual item — per-op ROW generics on Spawn.** The four
  Fiber-shaped Spawn ops carry per-op `[T]` (TYPE generics) since
  Tier 2; the spawned thunk's row variable still flows through the
  `thunk: Nothing` (TyAny) erasure rather than via a per-op
  `[T, e]` row binding. Reduces the wrappers in `stdlib/spawn.kai`
  to one-line aliases. Tracked in
  `docs/fibers-honesty-targets.md` §*Residual m8.x items*.

Out of scope for Wave 3: multi-thread scheduler (Orongo), asm-level
context switch (Orongo), preemption / fairness guarantees (post-MVP
per `docs/structured-concurrency.md` §*Non-goals*), distributed
actors (Orongo+).

**Definition of Done** (Tongariki overall, with Wave 3 closing the
remaining gap):

1. `kai test`, `kai check`, `kai bench`, `kai fmt` all functional
   and exercised by fixtures under `examples/stdlib/` and a
   pre-flight self-test on `stage2/`. *(Closed at `0.27.0`.)*
2. Inner-loop benchmark (`examples/perceus/unbox_bench.kai`) at
   ~2.0× C reference under -O2. *(Closed at `0.27.0` — Real
   unboxing.)*
3. All ≥24 demos pass; selfhost byte-identical (C + LLVM).
   *(Holds as of `0.30.0`.)*
4. CI green; release notes for each chunk.
   *(Tier 1 gate green via `.github/workflows/tier1.yml`;
   tier1-asan daily gate added in `0.30.0`.)*
5. CLAUDE.md Tier 1 closure remains defendible (no new
   footnotes). *(Holds.)*
6. **Wave 3 (m8.x)**: stdlib disclaimers no longer describe the
   pre-`0.4.0` inline-eager runtime; the two-actor request/reply
   fixture (`m8x_4_request_reply.kai`) is wired and passes;
   `lnds/ahu/docs/design.md` §*External dependencies on kaikai*
   gaps 1 and 2 closeable (coordinated PR follows on the ahu side);
   `docs/fibers-honesty-targets.md` Tier 1 / Tier 2 reflect the
   closed gap and the residual items have GitHub issues.

**Estimated cost**: Waves 1+2+cierre shipped in ~2 weeks of agent
work distributed across ~5 lanes. Wave 3 disclaimer sweep is one
small lane (~1 day); the two residual typer items are independent
lanes (~3–5d each) that can land in parallel with Anga Roa.

### Anga Roa — pre-1.0

"Hanga Roa" — the village where life happens. Polish the
developer experience to the level where teams (not just
hobbyists) can build with kaikai.

**Scope**:

- m11 diagnostics quality pass — Elm/Rust-grade error
  messages: "expected `Int`, found `String` at line N" with
  suggested fix, exhaustiveness counterexamples, scope-aware
  hole reporting. References `docs/typed-holes.md` for the
  JSON contract; m11 elevates the human-text side.
- `kai lsp` — Language Server Protocol implementation: type
  on hover, completions, go-to-definition, diagnostics push,
  symbol renaming. Reuses the typed-hole + diagnostic JSON
  surface.
- `kai repl` — interactive eval loop. Persists scope across
  expressions; honours effect handlers; integrates with
  `kai test` for in-REPL test execution.
- `bench` v1.x — median + MAD outlier detection,
  configurable iteration count.
- `check` v1.x — shrinking. Per-type halving for `Int`,
  delete-element + per-element for `[T]`, structural for
  records / sums.
- Reuse-in-place (Perceus Tier 3a) — constructors reuse
  consumed cells when alias analysis can prove the old value
  is dead at the construction site. Big win on linked-list
  rewrites (`map`, `filter`, `fold`-into-list). Brings inner
  loop to ~1.5–1.7× C reference.
  *(Optimization thread for this milestone.)*

**Definition of Done**:

1. Editor with `kai lsp` running shows type on hover, completes
   record fields, surfaces diagnostics inline.
2. `kai repl` round-trips a non-trivial session (define fn,
   call it, observe effect).
3. Property test runner reports the minimal counterexample
   (post-shrink) on failure.
4. Inner-loop benchmark at ~1.5–1.7× C reference.
5. Diagnostic quality: a representative ill-typed program
   produces a message at the bar set by the m11 acceptance
   fixtures (TBD as part of m11 design).

**Estimated cost**: ~4–6 weeks. m11 + lsp are the heaviest
items; reuse-in-place is the optimization sub-lane.

### Orongo — 1.0.0

The ceremonial village on Rano Kau, where the birdman cult
sealed the year's transitions. Mark the language as 1.0.0:
multi-thread, complete tooling, no honest-target footnotes.

**Scope**:

- Multi-thread scheduler — work-stealing across N OS threads;
  cross-thread RC requires atomics; changes the memory model.
  Single-thread cooperative is correct and parallelisable
  later, so this gates 1.0.0.
- asm-level context switch — `ucontext` is deprecated on
  macOS and ~1–2 µs per switch; production wants ~50–100 ns
  (boost.context shape).
- Cross-fiber unboxed messages (Tier 3b) — `send` / mailbox
  copies stay raw `int64_t` for primitive payloads, rather
  than always going through boxed `KaiValue *`. Coordinates
  with the multi-thread scheduler.
  *(Optimization thread for this milestone.)*
- Type-erased layouts (Tier 3b) — polymorphic raw layouts so
  generic containers can hold unboxed primitives without
  monomorphising for every primitive type.
- Package manager — `kai new`, `kai add`. Lockfile, registry
  abstraction, dependency resolution.
- FFI binding generator — given a C header, emit kaikai
  `extern "C" fn` declarations and `Ffi`-effected wrappers.
- Region-brand full `TyBranded` machinery — propagation
  through every binding form, sum-type-payload escape
  detection, brand-mismatch detection between sibling
  nurseries. Closes m8x §6.
- Opt-in regions (Tier 3a residual) — arena allocation for
  parser scratch and similar narrow workloads.
- `check` v2 — protocol-based generators (`impl Arbitrary
  for T`). Replaces the v1 intrinsic-table approach. Requires
  protocols to be load-bearing for stdlib (m12.8+).

**Definition of Done**:

1. Multi-threaded fiber scheduler running production
   workloads; context switch under 200 ns.
2. `kai new my_app && cd my_app && kai add some_lib && kai
   build` round-trips end to end.
3. FFI binding generator produces working bindings for at
   least one non-trivial C library (libc subset, sqlite, or
   similar).
4. Region-brand `TyBranded` propagation: no more "shallow
   check" caveats; sibling-nursery brand mismatch flagged at
   compile time.
5. Tier 1 #1, #2, #3 fully defendible without footnotes;
   `docs/perceus-honesty-targets.md` and
   `docs/fibers-honesty-targets.md` Tier 3 sections marked
   closed.

**Estimated cost**: ~2–3 months. Multi-thread scheduler is
the long pole; the rest can parallelise.

### Anakena — post-1.0 horizon

The historical landing beach of Hotu Matu'a — the
horizon-facing milestone. Reach beyond the founding
platforms.

**Scope**:

- Linux arm64 — second native target.
- macOS x86_64 — second native target on Apple platforms.
- Windows — third platform.
- WASM — runtime target with appropriate size + JIT-friendly
  emit constraints.
- LLVM mirrors for Phase 2 + TCO — close the gap where
  `--emit=llvm` binaries don't yet receive the speedups that
  `--emit=c` enjoys. Currently deferred per
  `docs/unboxing-phase2-followup.md` and issue #42.
  *(Optimization thread for this milestone.)*
- Stage 1 mirrors for Phase 2 + TCO — bootstrap chain
  parity. Currently bootstrap-only, so user code is
  unaffected, but cleanliness matters at this point.
- Per-target perf tuning — aarch64-specific TCO encoding,
  WASM size optimization, etc.
- Profiling tooling integrated with `kai bench`.

**Definition of Done**:

1. CI matrix runs all 5 platforms (the founding two plus
   Linux arm64, macOS x86_64, Windows, WASM).
2. `--emit=llvm` binaries reach inner-loop perf parity with
   `--emit=c` binaries.
3. Profiling tooling exposes per-fn alloc / time / RC
   breakdown.

**Estimated cost**: variable / ongoing — each target is its
own sub-lane.

## Meta-roadmap: the 5-layer architecture

kaikai is the foundation of a layered ecosystem. Each layer
is its own project, with its own repository, its own
roadmap, and its own `Tongariki / Anga Roa / Orongo /
Anakena` series. Names repeat across projects; context
disambiguates.

```
kaikai (the language)
   ↓
ahu (OTP-style framework: supervision, gen-server, BEAM-style)
   ↓
kohau (database / persistence layer)
   ↓
henua (DDD building blocks: aggregates, repositories, domain events)
   ↓
manutara (Phoenix-LiveView-style web framework)
```

### Disambiguation by project

- `kaikai-Tongariki` = the kaikai language MVP (this document,
  the top section).
- `ahu-Tongariki` = the ahu framework MVP. Separate repo,
  separate `docs/roadmap.md` once that project starts.
- `manutara-Tongariki` = the manutara web framework MVP.

### Sequencing

| Project   | Can start when                                                                       |
|-----------|--------------------------------------------------------------------------------------|
| kaikai    | (in progress; foundation of everything else)                                         |
| ahu       | effects + actors + structured concurrency in main; **already true today**.           |
|           | Recommended to wait for `kaikai-Tongariki` so `kai fmt` / `kai test` / `check`       |
|           | are available for ahu development from day one.                                      |
| kohau     | post `ahu-Tongariki`                                                                 |
| henua     | post `kohau-Tongariki`                                                               |
| manutara  | post `henua-Tongariki` — manutara needs ahu + kohau + henua at least at MVP.         |

Each downstream project does not wait for 1.0.0 of its
dependency. It waits for that dependency's Tongariki (MVP).
This unlocks parallelism: once `kaikai-Tongariki` ships,
`kaikai-Anga Roa` and `ahu-design` can run in parallel.

### What this doc is NOT

- Not the roadmap for ahu, kohau, henua, or manutara.
  Those projects own their own `docs/roadmap.md` once they
  start. This doc only mentions them to make the
  layering explicit.
- Not a calendar. Estimates assume each milestone has the
  team's primary focus; parallel work shifts the cost.
- Not a contract. Scope shifts item-by-item per the
  honesty-target documents
  (`docs/perceus-honesty-targets.md`,
  `docs/fibers-honesty-targets.md`).
- Not exhaustive. Items not listed (e.g. final syntax
  consolidation, stage retirement) live in
  `docs/proposed-extensions.md` or surface as new entries
  when they become load-bearing.
