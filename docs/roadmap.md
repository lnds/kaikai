# kaikai roadmap

Pinned 2026-05-02 (post v0.30.0). Last refreshed 2026-06-29 (HEAD
0.94.0, post the 0.79 → 0.94 arc: the in-process libLLVM backend
became the default (#851) and the llvm-text backend was removed
(#850); user-effect dispatch moved to capability passing with named
instances and hybrid dispatch (#820); the compile-time cache chain
closed its last load-bearing item, Phase B (#455); stage 2 began its
per-phase modularization (#677); and lazy `Stream[t, e]` landed in
stdlib (#801)). The prior pin tracked the LSP v1 → v2 → v3 wave that
closed #447 at 0.79.0. Names follow the Rapa Nui convention already in use across
the project (the language `kaikai` itself, the framework `ahu`,
the web framework `manutara`). Each milestone is a real Rapa Nui
site; the sequence Tongariki → Hanga Roa → Orongo → Anakena tracks
the arc from public face → daily life → ceremonial culmination →
horizon beyond.

**Version-to-edition map** (kept explicit so there is no ambiguity
when discussing Orongo readiness):

- **Tongariki** — pre-1.0 MVP edition, closed 2026-05-02.
- **Hanga Roa** — current pre-1.0 edition. HEAD `0.79.0` sits inside
  it. Spans the `0.30.0` → `<0.100.0` arc.
- **Orongo** — ships as **0.100.x** under this edition name (the 1.0
  label is postponed indefinitely). The edition
  boundary is the public stability commitment: surface code written
  against Orongo is preserved across patch / minor bumps within the
  edition.
- **Anakena** — post-Orongo horizon.

## Status snapshot

- **HEAD**: `0.94.0`. The 0.79 → 0.94 arc (2026-05-20 → 2026-06-29)
  was dominated by seven waves, all inside the Hanga Roa edition:
  - **Native backend default** — the in-process libLLVM backend
    (`--backend=native`) became the default (PR #851) and the
    llvm-text backend was removed (#850). It now implements mandatory
    TCO (#706), carries the Perceus RC discipline (cons/variant leaks
    #860/#868/#872 + the `kai_op_eq` UAF #858 closed), static-links
    libLLVM to drop external clang (#497), runs opt passes in-process
    (#498), caches bitcode (#499), and runs the dev-loop subcommands
    `test`/`bench`/`watch` C-free (#950). Backend-parity audit #622.
  - **User-effect dispatch overhaul (#820)** — by-name dynamic
    dispatch retired for user effects in favour of capability passing
    with first-class named instances (`with Eff as a`) and hybrid
    dispatch; a bounded lookup is retained for fiber-local builtins +
    `Ffi`. Evidence frames thread through both backends; the op-name
    evidence-collision corruption (#789) is fixed. Builtin effects
    migrated from compiler AST into stdlib source (#866).
  - **Compile-time perf + Phase B** — Phase A.1 core post-typecheck
    cache (#825) + A.2 (#461) + Phase B user-file incremental cache
    (#455) closed DoD #6's last load-bearing item. A quadratic sweep
    (#911/#914/#915/#916/#937) plus the emitter `Array[Byte]`
    accumulator (#901/#910) cut self-compile RSS from ~9 to ~5 GB and
    recovered throughput.
  - **Stage 2 modularization (#677)** — per-phase source split (#580)
    + `.kaii` interface persistence (#760); separate compilation
    (cross-module monomorph + per-module LLVM) is in progress (#963).
  - **Stdlib surface** — lazy `Stream[t, e]` + pipe combinators (#801)
    and chunked file reading (#771); civil date/calendar v1 (#767);
    protocol-bounded free-fn generics + generalised aggregates
    (#877/#891); `StringBuilder` (#902) and AVL-backed `Set` (#936);
    codepoint-correct String/Char fixes (#744/#926/#935); `#[doc]`
    attribute migration + coverage audit (#681/#774); opaque `Instant`
    (#867); `KAI_MAX_HEAP` ceiling (#878).
  - **Concurrency** — nursery auto-join + cancel-on-fail completes the
    structured-concurrency semantics (#959); `Spawn.cancel` now reaches
    parked fibers (#679/#682); `Actor.receive_timeout` (#638) and
    `spawn_actor_policy` (#763) landed.
  - **Tooling + surface ergonomics** — `kai lint` subcommand, phase 1
    (#966); `kai fmt` completed to the full language surface with a
    self-hosting ratchet (#781/#786); naked cell read + `var x := init`
    with `@` as Ref-deref (#956); point-free sections on method calls
    and chained fields (#958); block lambda pattern binder
    `{ (a, b) -> ... }` (#970).
- **Current target**: **kaikai-Hanga Roa** — de-facto since
  ~2026-05-03 with the protocols + ergonomics precondition chain.
  Tongariki MVP closed 2026-05-02 via PR #73 (issue #59 — m8.x
  disclaimer sweep). The chain that opened Hanga Roa de facto:
  - **Hanga Roa precondition work (landed early)**: #246 / #245
    (operator overloading via protocols + Complex stdlib),
    #266 (positional record construction), #257 (Ref[T] in
    Mutable effect), #260 / #261 (extern "C" fn syntax + name
    override), #275 (`@` / `:=` Ref sugar), #267 phase 1
    (Complex `i` literal), #180 (`protocol P[A]` single-dispatch
    parametrised), #267 phase 2 (heterogeneous Complex
    arithmetic), #174 (polymorphic impl bounded constraints),
    #175 (stdlib polymorphic impls Show/Eq/Ord/Hash for
    [T]/Option/Result), #258 (Default protocol + 6 impls).
    These are prerequisites for the remaining Hanga Roa scope
    (LSP, m11 diagnostics quality, bench v1.x, check
    shrinking, reuse-in-place, `check` v2 Arbitrary).
  - **Distribution**: macOS arm64 tarball + GH Actions release
    pipeline shipped at `0.39.0` (PR #278); private brew tap
    available.
- **Cross-cutting principles** (per CLAUDE.md):
  - Tier 1 #1 (memory safety + effects in types) — defendible
  - Tier 1 #2 (mandatory TCO) — defendible without footnote as of 0.23.0
  - Tier 1 #3 (fast compilation) — defendible
- **Inner-loop perf**: compute-bound code at C parity — fib(35) =
  1.0× C, Euler #4 = 1.05× C (post Phase 3 unboxing #383). Structural
  traversal: red-black tree at **2.64× C wall / 1.17× C RSS /
  garbage-free** (`docs/benchmarks/rb_tree_2026-06-02.md`). The
  default `--backend=native` reaches C parity on scalar arithmetic
  (#853/#857/#859); its cons/list and non-tail-call residuals
  (#858/#860/#861) closed through 2026-06-19. The remaining native-vs-C
  gap is heap-bound traversal tuning (`list_fold`/`rbtree`); numbers in
  `docs/native-codegen-perf-plan.md`. Orongo target: near-C on
  compute-bound with multi-thread + Tier 3b unboxed messages.
- **Selfhost**: per-compiler determinism — `kaic2b` and `kaic2c`
  (stage 2 compiled by itself, twice) produce byte-identical output.
  The native backend is now the default; the C backend remains the
  selfhost oracle. Stage 1's output is not required to match (see
  `docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md`).

## Milestones — kaikai language

Each milestone has scope, definition-of-done, estimated cost,
and one optimization thread (so perf debt does not pile up to
the end).

### Tongariki — MVP — **CLOSED 2026-05-02**

The 15-moai platform: the public face. Ship the language with
the minimum testing and dev workflow that lets early adopters
write real code. **Closed 2026-05-02** with PR #73 (issue #59 —
Wave 3 m8.x disclaimer sweep). Today's PR #283 (2026-05-05) was
a residual cleanup of secondary disclaimers in demos and
`stdlib/effects.kai`; it shipped after Wave 3 was already closed
and is not part of the gate.

Delivered in waves; the MVP cierre (`0.27.0`) closed the original
"ship a usable language" promise. The m8.x cooperative scheduler
runtime that backs the actor and structured-concurrency surfaces
shipped in `0.4.0` (R2 lane); **Wave 3** closed the documentation
sweep plus the residual typer-side items.

#### Wave 1 (`0.24.0`, shipped 2026-05-01) — `kai fmt` + TCO stage 1

- `kai fmt` — gofmt-style deterministic formatter, no options.
  Formats the full language surface — generics, effects, handlers,
  protocols, impls, units, refinements — and round-trips all of
  `stdlib/` + `stage2/compiler/` idempotently (issue #781 split the
  formatter into A-grade modules and closed the last real gaps: real
  literals now carry their raw span, typed single-lambda params keep
  their parens; one documented trailing-comment edge remains).
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
  Re-evaluation is paired with the Hanga Roa
  **reuse-in-place** lane, which may shift the alloc mix
  enough to unlock per-tag dispatch on
  KAI_RECORD / KAI_VARIANT. Retro pinned in
  `docs/lane-experience-drop-specialisation.md`.
- **Real unboxing (Phase 2 follow-up)** — pulled forward
  from the Unboxing Phase 2 inventory (now issues #87–#91)
  after drop specialisation closed as negative result. Brings
  Real-heavy programs from the prior ~50–100× C-ref gap into
  the same band as Int/Bool/Char (~5–10× post Phase 2). Same
  unbox_pass machinery + new C type emission for `MUnboxed Real`.
  Replaced drop specialisation as the Tongariki optimization
  thread.

#### Wave 3 — m8.x disclaimer sweep + residual items (issue #59) — **CLOSED 2026-05-02 (PR #73)**

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

**Definition of Done** (Tongariki overall, all six items closed):

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
   *(Closed 2026-05-02 via PR #73; secondary residuals swept by
   PR #283 on 2026-05-05.)*

**Estimated cost**: Waves 1+2+cierre shipped in ~2 weeks of agent
work distributed across ~5 lanes. Wave 3 disclaimer sweep is one
small lane (~1 day); the two residual typer items are independent
lanes (~3–5d each) that can land in parallel with Hanga Roa.

### Hanga Roa — pre-1.0 — **STARTED ~2026-05-03 (de facto)**

"Hanga Roa" — the village where life happens. Polish the
developer experience to the level where teams (not just
hobbyists) can build with kaikai.

**Started ~2026-05-03** with the protocols + ergonomics
precondition chain (see *Status snapshot*); the remaining scope
below tracks against issues #86 (m12.6 diagnostics quality —
title was "m11 diagnostics" pre-rename), #210 (variant + record
reuse-in-place — closed 2026-05-06), and #264 (canonical
grammar.md, closed 2026-05-06). LSP is tracked under #447; v1
(hover) shipped v0.75.0, v2 (goto-def + publishDiagnostics +
documentSymbol) shipped v0.76.0, v3 (completion + signatureHelp +
hole warning diagnostics) shipped v0.77.0 → v0.79.0
(2026-05-09 → 2026-05-20). The previous citations of #92 and
#120 were ghost references (#92 is `R6 — TCO precise
per-call-site dropmask`, closed; #120 is `Perceus: opt-in
regions for parser/lexer scratch buffers`, open) and have been
removed. **REPL is removed permanently** — see #406 and
`docs/decisions/repl-removal-2026-05-09.md`. Not in any edition, not on the roadmap. `kai run` + `kai watch` cover the
ad-hoc evaluation workflow.

**Scope — precondition work (landed early, 2026-05-03 → 2026-05-05)**:

- Operator overloading via protocols + Complex stdlib (#246, #245).
- Positional record construction `T { v1, v2 }` (#266).
- `Ref[T]` in `Mutable` effect (#257).
- `extern "C" fn` syntax + name override (#260, #261).
- `@` / `:=` Ref sugar (#275).
- Complex literal `i` suffix — phase 1 (#267).
- `protocol P[A]` single-dispatch parametrised + heterogeneous
  arithmetic — phase 2 (#180, #267 phase 2).
- Polymorphic impl bounded constraints (#174).
- Stdlib polymorphic impls Show/Eq/Ord/Hash for
  [T]/Option/Result (#175).
- Default protocol + 6 impls (#258).

These unblock the rest of the scope below: m11 needs the
diagnostic surface stabilised, LSP needs the protocol resolver,
and `check` v2 (`Arbitrary`) needs `protocol P[A]` plus the
polymorphic-impl machinery.

**Scope — closure status (refreshed 2026-06-29, HEAD v0.94.0)**:

The original Hanga Roa scope was six items: m11 diagnostics, LSP,
bench v1.x, check shrinking, reuse-in-place, plus the compile-time
perf chain pinned in DoD #6. **All six have shipped** — Phase B
(#455), the last load-bearing item, closed 2026-06-12. The edition
then absorbed a large architectural wave that was not in the original
six (native backend default #851, user-effect dispatch #820, stage 2
modularization #677, lazy streams #801); see the §Status snapshot
HEAD bullet for the full 0.79 → 0.94 arc.

- ~~**m11 diagnostics quality pass**~~ ✅ **shipped 2026-05-03 → 2026-05-15**.
  Elm/Rust-grade error messages with stable JSON alongside the
  human-readable text. Three sub-lanes closed:
  - m11 v1 (#445, `e239d15`): type mismatch + non-exhaustive +
    unbound name templates.
  - m11 v1.x (#479, `3273f87`): wrong arity + missing effect
    templates. Closes the m11 v1 series.
  - Collectable structured diagnostics (#487, PR #491,
    `5ffe26c`): T1-T5 emit through the structured collector,
    consumed by `kai build --diags-json` and by the LSP's
    `textDocument/publishDiagnostics`.
- ~~**`kai lsp`**~~ ✅ **shipped v0.75.0 → v0.79.0
  (2026-05-09 → 2026-05-20, issue #447)** — Language Server
  Protocol implementation: hover, goto-definition,
  publishDiagnostics (compile errors + hole warnings),
  documentSymbol, completion, signatureHelp. Reuses the
  typed-hole + diagnostic JSON surface; consumes
  `--library-mode` from issue #454. Symbol rename and
  cross-file resolution slip to a v4 lane post-Hanga Roa.
  See `docs/lsp.md` for the v3 capability matrix.
- ~~**`bench` v1.x**~~ ✅ **shipped** (#437, `53a1cbd`):
  median + MAD outlier detection + configurable iterations +
  warmup. `kai bench` consumes the new surface.
- ~~**`check` v1.x — shrinking**~~ ✅ **shipped** (#438,
  `07424e6`): per-type halving for `Int`, delete-element +
  per-element for `[T]`, structural reduction for records /
  sums. Property test failures now report the minimal
  counterexample.
- ~~**Reuse-in-place (Perceus Tier 3a)**~~ ✅ **shipped** across
  three lanes:
  - #118 (`0c97cb8`): known-unique constructors reuse consumed
    cells.
  - #209 (`9baff4e`): resolved `pcs_rewrite_expr` dup
    interaction so reuse-in-place actually fires.
  - #210 (`e8cf955`): typer-aware shape predicate enables
    variant + record reuse-in-place (PR #289).
- ~~**Compile-time perf chain (DoD #6)**~~ ✅ **complete** — see
  §DoD #6 below. Phase A.0 + A.1 (#825) + A.2 (#461) + KAB2 binary
  format + Phase B user-file incremental cache (#455, closed
  2026-06-12) all shipped.

**Definition of Done** (closure status refreshed 2026-05-20):

1. ✅ **MET.** Editor with `kai lsp` running shows type on
   hover, surfaces diagnostics inline, lists outline symbols,
   triggers completion (top-level), and shows signature help.
   `kai lsp` v3 shipped v0.75.0 → v0.79.0 (#447); see
   `docs/lsp.md`. Record-field completion is a deferred v4
   item (not load-bearing for Hanga Roa close).
2. ✅ **MET.** Property test runner reports minimal
   counterexample post-shrink (#438, `07424e6`).
3. **Performance, bifurcated by workload class.** The split
   reflects the architecture: kaikai represents locals unboxed
   and storage edges boxed; a single performance target cannot
   honestly cover both unless they share a representation.

   3a. ✅ **MET.** Compute-bound code ≤ 1.5–2× C reference
       under -O2 — actual: **fib(35) = 1.0× C, Euler #4 =
       1.05× C** post Phase 3 unboxing (#383), see
       `docs/benchmarks/compute_2026-05-09.md`. Both numbers
       smash the gate.

   3b. **Structural data traversal** (records, variants,
       recursive tree algorithms with mutation-as-rebuild):
       ≤ 5–10× C reference under -O2 in-MVP, with a roadmap to
       ≤ 2× post-MVP via Phase 3 (field unboxing + reuse-in-place
       for variants). Acceptance benchmark: red-black tree
       (`examples/perceus/rb_tree_bench.kai`).

   The locals-unboxed/storage-edges-boxed split (Phase 2) and the
   call-boundary split (Phase 3, #383) both landed; the
   reuse-in-place cascade (2026-06) followed. Current measured
   state: the red-black tree benchmark runs at **2.64× C wall /
   1.17× C RSS / garbage-free** — see `docs/benchmarks/rb_tree_2026-06-02.md`
   for the apples-to-apples Koka/C/C++ comparison and
   `docs/perceus-honesty-targets.md` for the honesty-tier breakdown.
   The single remaining large lever is value-immediate `Int`
   (post-MVP).
5. ✅ **MET.** Diagnostic quality at the m11 bar — type
   mismatch (#445), non-exhaustive match (#445), unbound name
   with Levenshtein did-you-mean (#445), wrong arity (#479),
   missing effect (#479). Both human-readable text and stable
   JSON (`kai build --diags-json`, via #487).

6. **Compile-time performance — DoD #6 (added 2026-05-11)**:
   - **Tiny program cold compile** (empty fn, no user imports):
     ≤ 300 ms wall, ≤ 100 MB RSS. Phase A.0 + KAB2 landed
     2026-05-14 → 2026-05-16 (#452 + #592).
   - **Incremental recompile** (touch + 0-byte change → rebuild):
     ≤ 150 ms wall. Phase B (#455) shipped 2026-06-12.
   - **Self-compile** (`make -C stage2 kaic2`): ≤ 5 s wall, ≤ 800 MB
     RSS. Original baseline 6.8 s / 950 MB; re-measure pending.

   Empirical motivation: `docs/benchmarks/cross_lang_2026-05-10.md`
   measured kaikai at 1.5 s cold (40× slower than `cc -O2`,
   5× slower than `elixirc`). Decomposition revealed ~1.3 s is
   stdlib re-parsing on every `kai build` invocation. Required
   lanes (prerequisite chain):

   - ✅ #453 — runtime `core_read_bytes` (LSP precursor) — **shipped**.
   - ✅ #454 — compiler library mode (typed AST retention + span index
     for queries, selfhost waiver). **Shipped** as the
     `--library-mode` flag + `compile_to_module` in-process API; see
     `docs/library-mode.md`.
   - ✅ #452 — Phase A.0 stdlib precompiled cache. **Shipped**
     (`e6f02f9`, `688e666`, `676f4d1`).
   - ✅ #592 — KAB2 binary on-disk format. **Shipped**
     (`8b33888`, `3954c21`). Default cache flipped to
     `KAI_PRELUDE_CACHE=1`.
   - ✅ #825 — Phase A.1 core post-typecheck cache. **Shipped.**
   - ✅ #461 — Phase A.2 (post-perceus cache + emit-only-user).
     **Shipped.**
   - ✅ #455 — Phase B user-file incremental cache. **Shipped
     2026-06-12.** Mtime + hash skipping for user modules; the
     last load-bearing item closing DoD #6.

   DoD #6 is complete. #447 (LSP) consumes the same query surface
   from #454 and is a thin layer over it (shipped).

**Estimated cost**: original 4–6 weeks. The precondition chain
absorbed ~2 weeks. The six scope items closed in roughly two weeks
of agent work (m11 v1+v1.x+collectable, LSP v1→v3, bench v1.x, check
shrinking, reuse-in-place); Phase B (#455) closed 2026-06-12. With
all six DoD items met, Hanga Roa's original scope is closed; the
0.79 → 0.94 architectural wave (native default, #820 dispatch, stage 2
modularization, streams) is additional edition-window work, not a
blocker. Remaining notes:

- **DoD #3b** RB-tree perf is **explicitly Orongo-trajectory**, not
  Hanga Roa (decided 2026-05-09; see the §DoD #3b note). Its three
  levers all shipped: #383 (Int unboxing), #384 (variant-reuse
  borrowed-binds, closed 2026-06-03), #593 (primitive-slot extract
  raw, closed 2026-06-03). Current measured state: 2.64× C wall.

### Orongo — 0.100.x

The ceremonial village on Rano Kau, where the birdman cult
sealed the year's transitions. Ships as 0.100.0; the 1.0 label is
postponed indefinitely.

The scope below supersedes the original draft (which gated 1.0 on a
multi-thread scheduler and a package registry); the authoritative
remaining-gates list lives in `docs/editions.md` and this section
mirrors it. The two Orongo pillars the edition was re-scoped around
(2026-07-08) have both **shipped**:

- **Kind engine** (#1108, PR #1133) — user abelian kinds, per-kind
  habitant isolation, use-site resolution, the `unify_<theory>`
  dispatch seam for future theories (`Structural`, `Module`).
- **Borrow** (#1129/#1130/#1138) — `^` surface + interface ABI,
  non-consuming indirect calls (closure RC eliminated), relaxed
  read-path inference, modular-safe.

**Remaining scope (the real Orongo gates):**

- **The surface pin + `EDITION` flip** — the deliberate act. Two
  surface decisions are explicitly deferred to this moment, to be
  taken with measurements in hand: the list-literal default
  (`[1,2,3]`: cons today; the flip — literal AND type together,
  cons becoming nominal `List[T]`, no sigil — per the 2026-07-09
  design consult) and the final `kai info syntax` freeze.
- **`kai migrate` rule set** — machinery shipped (#1047); the rule
  set grows with the surface pin (the list flip, if taken, is its
  largest rule).
- **Separate/modular compilation for user programs** — L1–L3
  shipped; the Phase 3 remainder of #963 closes the gate.
- **Near-C performance on structural workloads** — the measured
  ladder, in order: pipe fusion (#1140/#1145, shipped: fused
  chains, range generator heads), `Vec[T]` (#1142/#1147, shipped:
  flat unboxed storage, uniqueness in-place, raw element paths —
  case-6 memory at Rust parity), sized-exact variant
  representation (#1136, in flight — the rb-tree wall closer),
  `Vec` surface stage 3 (#1150, shipped: O(1) slices with in-place re-slice, literal minting, fused pipeline collect).
- **The authorability benchmark** — the Tier 3 acceptance measured
  head-to-head (kaikai vs Rust vs Go, success rate per compile
  round; harness repo exists, task set in authoring). Orongo ships
  with the number published, not asserted.

**Definition of Done:**

1. `EDITION` reads `orongo`; every surface decision in
   `kai info syntax` is pinned; `kai migrate` rewrites the full
   Hanga-Roa → Orongo delta with a byte-identity gate on the
   migrated compiler.
2. The perf ladder holds on `main`: case-6 within the
   Koka-class envelope on wall with Rust-class memory, rb-tree
   within the Eje-2 target on both backends, all guarded by
   tier-wired bench fixtures (no unreproducible claims).
3. Modular compilation compiles, links and runs a multi-package
   user program with per-package rebuilds.
4. The authorability benchmark's function tier runs green
   end-to-end and its result ships in the release notes.
5. Honesty docs current: no `v1 status` sidebar older than the
   release; `docs/perceus-honesty-targets.md` and
   `docs/fibers-honesty-targets.md` reflect shipped reality.

**Explicitly moved past 1.0** (tracked for Anakena, matching
`docs/editions.md`): the multi-thread scheduler (work-stealing,
atomics-free by fiber isolation — the architecture already paid
for it), asm-level context switch, cross-fiber unboxed messages,
type-erased layouts, the package registry and FFI binding
generator, `check` v2, and the full region machinery (#1123,
parked: `Vec` took the confined-bulk case; regions keep
trees-in-arena, lifetime batching and the generational escape).

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
- Native codegen perf parity — the in-process libLLVM backend is
  now the **default** (PR #851), so this gap is what users ship by
  default. Scalar `+ - * / %` is at C parity (lanes #853/#857/#859,
  see `docs/native-codegen-perf-plan.md`); the cons/list RC leak
  (#860), the `kai_op_eq` UAF (#858), and the non-tail raw-call re-box
  (#861) all closed (to 2026-06-19). Per-target tuning of the heap-
  bound traversal residuals continues here.
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
2. `--backend=native` binaries reach inner-loop perf parity with
   `--backend=c` binaries (scalar arithmetic at parity; the heap +
   non-tail-call residuals #858/#860/#861 closed — heap-bound
   traversal tuning ongoing).
3. Profiling tooling exposes per-fn alloc / time / RC
   breakdown.

**Estimated cost**: variable / ongoing — each target is its
own sub-lane.

## Meta-roadmap: the 5-layer architecture

kaikai is the foundation of a layered ecosystem. Each layer
is its own project, with its own repository, its own
roadmap, and its own `Tongariki / Hanga Roa / Orongo /
Anakena` series. Names repeat across projects; context
disambiguates.

```
kaikai (the language)
   ↓
ahu (concurrency + fault-tolerance framework: streams, cells, restart helpers)
   ↓
kohau (database / persistence layer)
   ↓
henua (DDD building blocks: aggregates, repositories, domain events)
   ↓
   ├──▶ manutara (Phoenix-LiveView-style web framework)
   └──▶ hopu (background jobs / queue / scheduler — Oban-style)
```

`manutara` and `hopu` are sibling consumers of the lower stack:
manutara handles the synchronous web face, hopu handles the
asynchronous background-job face. Neither depends on the other.

### Disambiguation by project

- `kaikai-Tongariki` = the kaikai language MVP (this document,
  the top section).
- `ahu-Tongariki` = the ahu framework MVP. Separate repo,
  separate `docs/roadmap.md` once that project starts.
- `manutara-Tongariki` = the manutara web framework MVP.
- `hopu-Tongariki` = the hopu jobs framework MVP.

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
| hopu      | post `kohau-Tongariki` — hopu needs ahu (workers) + kohau (persistent queue).        |
|           | henua is optional; jobs without DDD aggregates are fine.                             |

Each downstream project does not wait for 1.0.0 of its
dependency. It waits for that dependency's Tongariki (MVP).
This unlocks parallelism: once `kaikai-Tongariki` ships,
`kaikai-Hanga Roa` and `ahu-design` can run in parallel.

### What this doc is NOT

- Not the roadmap for ahu, kohau, henua, manutara, or hopu.
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
