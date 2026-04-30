# Changelog

All notable changes to kaikai are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html);
prior to 1.0.0 minor versions may break backwards compatibility (see CLAUDE.md
"Backward compatibility — not promised until post-MVP").

## [Unreleased]

### Added

- **Unboxing Phase 2 (Tier 2.5) — locals + arithmetic stay raw
  inside the C backend.** A new `unbox_pass` between
  monomorphisation and `perceus_pass` analyses every function
  body and decides, per `Expr` node, whether the value lives as
  a raw C scalar (`int64_t` / `int` / `uint32_t`) or as a boxed
  `KaiValue *`. Local `let` bindings whose RHS is a numeric
  `Int` arithmetic chain collapse to `int64_t kair_<name>` C
  storage; comparisons, short-circuiting `and` / `or`, the
  `EIf` condition slot, and literal-arm `match` scrutinees all
  fall out of the same `MUnboxed` reasoning.
  - The Phase 1 small-int + char cache stays — it pays off at
    every `MUnboxed → MBoxed` boundary.
  - Function parameters and return values stay boxed (Phase 2
    non-goal #1). Closure captures stay boxed (a candidate raw
    binding referenced from an inner ELambda is demoted to
    `MBoxed` at the `let` site).
  - Perceus coordination: raw locals do NOT enter the Perceus
    tracking scope (`pat_bindings_skip_raw`), so the
    `__perceus_dup` / `__perceus_drop` insertion never targets
    them. Documented invariant in the `unbox_pass` comment.
  - Coverage fixtures under `examples/perceus/`:
    `unbox_phase2_arith.kai` (M3 — let + arith),
    `unbox_phase2_cond.kai` (M4 — comparisons + booleans + EIf),
    `unbox_phase2_match.kai` (M5 — switch fast path),
    `unbox_bench.kai` + `.c.ref` (wall-clock comparison vs C).
    Wired into `stage2/Makefile :: test-unbox-phase2`.
  - Local timing on the bench fixture: kaikai ~5 ms vs C
    reference ~2 ms under `-O2` (~2.5×, inside the Tier 2.5
    target ~5–10× band). Pre-Phase-2: ~50–100×.
  - LLVM backend mirror and stage 1 mirror are deferred and
    documented in `docs/unboxing-phase2-followup.md`. Both
    omissions are non-regressions: `make -C stage2 selfhost-llvm`
    stays byte-identical at the fixed-point level, and stage 1
    is a bootstrap-only path so its perf is irrelevant to user
    code.

## [0.20.1] — 2026-04-30 (R5 fix — RLIMIT_STACK bump for tail-recursive demos on Linux)

### Fixed

- **R5 — `demos/euler4` segfault on Linux is gone.** The runtime
  installs an `__attribute__((constructor))` that bumps
  `RLIMIT_STACK` to 256 MiB (or the hard cap, whichever is
  smaller) before `main` runs, so the kernel's automatic
  stack-grow path keeps servicing page faults past the 8 MiB
  glibc default. Root cause was H4 (none of the issue's three
  ranked hypotheses) — Perceus drops emitted *after* a self-tail
  call inhibit C-level TCO under both gcc and clang on Linux
  (verified: `kai_search` emits `call kai_search`, not `jmp`),
  and `search`'s ~1 M-deep recursion blew the OS stack. macOS
  shipped a more permissive default and never tripped it. The
  proper compiler-side fix — emitter-recognised self-tail-call
  rewriting into a `goto` loop — stays pinned as a follow-up;
  the runtime bump unblocks CI today. `BASELINE=23` is dropped
  from `.github/workflows/tier1.yml`; CI now runs against the
  same `demos/baseline.txt = 24` as local mac. Closes #34.

## [0.20.0] — 2026-04-30 (Fibers Tier 2 close — Monitor + spawn_actor + LLVM in_dispatch + Spawn cleanup + Pid region-brand)

### Added

- **`Pid[Msg]` region-brand check now symmetrises with `Fiber[T]`.**
  `check_no_fiber_escape` rejects any non-stdlib helper that
  returns a `Pid[Msg]` at any nesting depth, with a Pid-shaped
  diagnostic. The producer allow-list grew to include
  `alloc_for_policy` and `spawn_actor` from `stdlib/actor.kai`,
  the two legitimate Pid surface helpers. The full
  `TyBranded(Ty, BrandId)` machinery — propagation through every
  binding form, sum-type-payload escape, brand-mismatch detection
  between sibling nurseries — is still pending and pinned in
  `docs/m8x-followup.md` §6. Coverage:
  `examples/effects/m8x_6_pid_escapes.kai`.

- **`Monitor.monitor(pid)` + `Monitor.demonitor(ref)` are now
  end-to-end primitives (Tier 2 supervision).** The Monitor
  effect was type-surface-only before; the runtime now
  registers `(observer, target_pid)` entries on the *target*
  fiber's `monitor_head` chain, and the trampoline's
  termination tail walks it and pushes the original
  `target_pid` value into each observer's mailbox — without
  touching `cancel_requested` (monitors are fault-isolated per
  `docs/actors.md` §*Fault propagation*). v1 simplification:
  `MonitorRef` collapses to `Pid[Nothing]` and the
  `MonitorDown(ref, cause)` payload becomes a bare pid push;
  reason distinction (Normal/Crashed) is reachable today
  through Link+trap_exit on the same target. Coverage:
  `examples/effects/m8_monitor.kai`.

- **`spawn_actor(body) : Pid[Msg]` lifts on `fiber_spawn` +
  `with_mailbox`** so a parent fiber receives the spawned
  fiber's pid synchronously and can immediately pass it to
  `Monitor.monitor(pid)`, `Link.link(pid)`, or `Spawn.send(pid,
  msg)`. Two new runtime helpers in `stage0/runtime.h`:
  `kai_mailbox_alloc_unowned()` (no owner stamp) and
  `kai_mailbox_assign_owner(pid, fiber)` (set
  `pid->as.mb->owner_fiber = fiber->as.fib` AND
  `fiber->as.fib->mailbox = pid->as.mb`). Safe under the
  cooperative scheduler because `Spawn.spawn` does not yield,
  so the parent's assign call is observed before the spawned
  trampoline runs. Closes the spawn_actor + Monitor line item
  from `docs/fibers-honesty-targets.md` Tier 2.

### Fixed

- **LLVM op-dispatch now mirrors the C emit's `in_dispatch_node`
  guard (m8 bug #12 LLVM mirror).** `llvm_emit_op_dispatch` saves
  the current fiber's `in_dispatch_node`, sets it to the looked-up
  evidence node before the indirect call, and restores afterwards;
  without this, a self-delegating handler under `--emit=llvm`
  would re-resolve `Eff.op(...)` back into its own clause and
  infinite-loop until stack overflow — same shape as the C-side
  bug closed in commit `4a77d49`. Three new runtime helpers
  (`kaix_evidence_lookup_node`, `kaix_evidence_node_handler`,
  `kaix_in_dispatch_enter` / `kaix_in_dispatch_leave`) keep the
  KaiFiber struct out of the IR. Coverage:
  `examples/effects/m8_12_self_delegating_handler.kai` now runs
  under both backends, with a structural grep on the IR
  confirming enter/leave pairing. Closes the LLVM
  `in_dispatch_node` line item from
  `docs/fibers-honesty-targets.md` Tier 2.

### Changed

- **Spawn ops now carry per-op `[T]` (m7b #2a retrofitted to
  `builtin_spawn_decl`).** `Spawn.spawn(thunk) : Fiber[T]`,
  `Spawn.await(f) : T`, `Spawn.select(fs) : T`, `Spawn.cancel(f) :
  Unit` — `await` / `select` no longer return `Nothing` (TyAny)
  and the typed `Fiber[T]` flow is observable end-to-end. The
  `spawn` op keeps `thunk: Nothing` until per-op ROW generics
  land (`spawn[T, e](f: () -> T / e)`); without them the wrappers
  in `stdlib/spawn.kai` would lose the row propagation they
  provide. Coverage: `examples/effects/m8_spawn_per_op_generics.kai`.
  Closes the Spawn-API line item from
  `docs/fibers-honesty-targets.md` Tier 2 (partial — TYPE generics
  done; ROW generics tracked in `docs/m8x-followup.md` §7).

## [0.19.0] — 2026-04-30 (m13 bit ops chunk — twelve compiler intrinsics on `Int`)

### Added

- **m13 bit ops chunk — twelve compiler intrinsics on `Int`.**
  `bit_and` / `bit_or` / `bit_xor` / `bit_not` / `bit_shl` /
  `bit_shr` / `bit_ushr` / `bit_count` / `bit_test` / `bit_set`
  / `bit_clear` / `bit_toggle` are registered in the typer's
  intrinsic table (next to `unit_name` / `__strip_unit`) and
  lowered inline by `emit_call_value` in stage 2 to the matching
  C operator (`<<`, `>>`, `&`, `|`, `^`, `~`,
  `__builtin_popcountll`). No runtime helper, no stage 1 mirror —
  the emitted C never contains the intrinsic name. `bit_shr` is
  arithmetic (sign-preserving); `bit_ushr` casts through
  `uint64_t` for logical zero-fill; `bit_test` returns `Bool`;
  the rest stay in `Int`.
- `stdlib/math/bits.kai` — header-only documentation surface for
  the intrinsics. Declares no bodies; the typer recognises the
  names directly.
- `examples/stdlib/bits_basic.kai` — fixture exercising all 12
  ops. The `test-stdlib` target now greps the emitted C and
  asserts each operator was lowered inline (and that no
  `kai_bit_*` runtime call leaked through).

## [0.18.0] — 2026-04-29 (Perceus Tier 2 close — multi-read dup + match-scrutinee + kai_field balance)

### Added

- **`kai_field_borrow` runtime helper** (and LLVM symmetric
  `kaix_field_borrow`) — like `kai_field` but does not incref
  the returned value. Used in `emit_pat_test_record_fields`
  (stages 0 + 1 + 2) so a record-pattern arm test no longer
  leaks one ref per field tested. `emit_pat_binds` keeps using
  the incref-ing `kai_field` so matched arms still get owned
  bindings.

### Changed

- **`perceus_pass` now wraps multi-read EVar reads inside string
  interpolation bodies in `__perceus_dup`** (stage 1 + stage 2;
  closes `docs/m5x-followup.md` §4b and the architectural debt
  pinned in `docs/perceus-honesty-targets.md`). The collector
  already counted reads buried in `#{...}` correctly, but the
  rewriter could not transform an opaque source span. The new
  `pcs_rewrite_estr_span` re-tokenises and re-parses each interp
  body, threads the parsed Expr through `pcs_rewrite_expr`, then
  surgically rewrites the source by replacing wrapped ident
  tokens with `__perceus_dup(NAME)`. The result rounds back
  through the parser at emit time as a regular ECall.
- **`emit_match_expr` (stages 0 + 1 + 2) now consumes the
  scrutinee linearly** at match exit (`kai_decref(_scr)`). The
  R3 amend's net-zero `kai_incref(_scr)` … `kai_decref(_scr)`
  bracket in stage 2 drops after §4b made multi-read bare reads
  always arrive pre-wrapped in `__perceus_dup`. Stages 0 and 1
  pick up the exit decref so the kaic1 / kaic2 binaries
  themselves stop leaking match scrutinees during their own
  runtime.

### Numbers

`kaic2` self-compile under `KAI_TRACE_RC` (compared to the
post-Tier 2 partials baseline pinned in
`docs/perceus-honesty-targets.md`):

| metric        | partials baseline | + §4b + match + field_borrow |
|---------------|------------------:|-----------------------------:|
| alloc_total   |             33.5M |                        34.3M |
| free_total    |              8.2M |                        20.8M |
| leaked        |             25.4M |                        13.4M |
| live_peak     |             25.4M |                        13.4M |

`leaked` cut by 47% (25.4M → 13.4M) on top of the partials
landed earlier in the day. The `kai_field_borrow` swap is a
correctness improvement (a failing record-pattern arm no longer
leaks per-field) — its measurable impact on `kaic2`'s self-
compile is small because stage 2 emits exactly one record-
pattern test in its own source. Selfhost (stage 1 + stage 2 +
LLVM) byte-identical, `make tier0` and `make tier1` clean,
demos baseline 24 holds, R3 closed-loop fixture (`examples/
effects/interp_recursive_walk.kai`) green.

## [0.17.0] — 2026-04-29 (Fibers Tier 2 — trap-exit + DX polish)

### Added

- **Fibers Tier 2 — trap-exit semantics.** `Spawn.set_trap_exit(Bool)`
  toggles the current fiber's `trap_exit` flag. With `trap_exit=1`, a
  linked peer's termination no longer cancels the fiber: instead the
  Link propagation walk pushes a `String` (`"Normal"` if the peer's
  state is `KAI_FIBER_DONE`, `"Crashed"` if `KAI_FIBER_CANCELLED`) into
  the fiber's most-recently-allocated mailbox. Without trap_exit (the
  default) the previous uniform cancel-cascade is unchanged. This
  closes the BEAM-style `process_flag(trap_exit, true)` distinction
  the design has long called out as post-MVP, and lets supervisor
  patterns observe child terminations without being killed.
  - `KaiFiber` gains two slots: `int trap_exit` (the flag) and
    `struct KaiMailbox *mailbox` (back-pointer stamped by
    `kai_mailbox_alloc[_bounded]`, cleared by `kai_mailbox_free`),
    both used only by `kai_link_propagate_terminate`.
  - `kai_link_propagate_terminate` now takes a `KaiExitReason`
    argument (`KAI_EXIT_NORMAL` / `KAI_EXIT_CRASHED`); the trampoline
    selects it from `self->state` at termination.
  - Builtin `Spawn` effect grew a sixth op `set_trap_exit(on: Bool)
    : Unit` (`stage2/compiler.kai` `builtin_spawn_decl`); the default
    handler `kai_default_spawn_set_trap_exit` writes through to the
    current fiber's flag.
  - Stdlib wrapper `fiber_set_trap_exit(on: Bool) : Unit / Spawn` in
    `stdlib/spawn.kai`.
  - Coverage: `examples/effects/m8_trap_exit.kai` (end-to-end through
    the Spawn effect — supervisor pattern observing both Normal and
    Crashed children) wired into `make -C stage2 test-effects`;
    `stage2/tests/link_runtime_test.c` extended with three new
    scenarios (Normal/Crashed delivery, fallback when peer has no
    mailbox, plain peer still gets `cancel_requested`).
  - Spec: `docs/actors.md` §*Trap-exit semantics*. Status update in
    `docs/m8x-followup.md` §5 and `docs/fibers-honesty-targets.md`
    Tier 2.
- `examples/quickstart/` — five short, runnable programs that
  introduce one concept each: hello, sum types + match, recursive
  AST, custom effect + handler, cooperative fibers. README gains
  a Quickstart section above Build pointing at them. ~150 lines
  total, every file ships with its header comment, run command,
  and expected output.
- Coverage probe ratchet: `tools/coverage-baseline.txt` 17 → 0
  after marking design / historical / non-feature / post-MVP-deferred
  headings with `<!-- coverage: skip -->` and naming the existing
  fixtures that already cover Spawning actors / Mailbox policies /
  Interaction with Cancel.

### Changed

- `bin/kai` now passes `--path "$ROOT/stdlib"` on every `kaic2`
  invocation, so `import spawn` (and any other stdlib module
  beyond the default prelude chain) resolves out of the box. Fixes
  `demos/concurrent/main.kai` running under the driver and unblocks
  the quickstart's fibers example.

## [0.16.0] — 2026-04-29 (Fibers Tier 1 #2 — stack guard pages)

Closes the second and final Fibers Tier 1 item (`docs/fibers-honesty-targets.md`).
After this lane, *Fibers Show HN without fine print* is shipped: the two
bugs a curious visitor would hit in five minutes — `let _ =
fiber_spawn(...)` deadlock (R4, fixed in PR #25) and silent stack
overflow — both now produce well-defined behaviour.

#### Added

- Spawned fibers' private stacks are now `mmap(MAP_PRIVATE | MAP_ANON)`
  with one extra page reserved at the low end and flipped to
  `PROT_NONE` via `mprotect`. Stack grows down on x86_64 / arm64, so
  the guard sits at the overflow target — touching it raises
  SIGSEGV/SIGBUS instead of corrupting whatever malloc happened to
  place underneath the previous heap-allocated stack.
- A SIGSEGV/SIGBUS handler installed on first `Spawn.spawn` runs on
  a `sigaltstack` (so the overflowed stack is not used to format
  the message), detects faults inside the active fiber's guard
  range, and prints `kai: fiber stack overflow at <ptr>` to stderr
  before re-raising with the default disposition. Faults outside
  any guard fall through to default — NULL derefs and other
  unrelated SIGSEGV sources keep their natural behaviour.
- `KAI_FIBER_STACK_SIZE` is now rounded up to a page-size multiple
  via `sysconf(_SC_PAGESIZE)`. macOS arm64 has 16 KiB pages; sub-page
  values silently broke the env knob otherwise.
- Coverage fixture: `examples/effects/m8_fiber_stack_overflow.kai`
  recurses non-tail-deeply (`deep(50000)`) inside a spawned fiber.
  Wired into `test-effects` to assert non-zero exit + the diagnostic
  appearing on stderr.

#### Changed

- `docs/fibers-impl.md` *Stack model* describes the new layout +
  handler. Pending-followup line *Stack guard pages / overflow
  detection* removed.
- `docs/fibers-honesty-targets.md` Tier 1 closed; both rows struck
  through with shipped dates.

#### Notes for the integrator

- Tier 1 final line: `tier1 OK — full make test + demos baseline`.
- `make -C stage2 selfhost-llvm` byte-identical.
- `demos/ping_pong/` (R4 regression coverage) still green.
- POSIX-only path: `<sys/mman.h>` and `<signal.h>` are the only new
  includes pulled into stage 0; macOS and Linux covered, Windows
  remains post-MVP per CLAUDE.md.

## [0.12.0] — 2026-04-29 (Perceus Tier 2 partials + testing-tier guardrails)

**Minor: residual leak after R1 flip drops 46.9 M → 23.8 M (−49%)
on `kaic2` self-compile.** Four Tier 2 partial-landings paid down
the brute-force eager-dup cost the m5.x flip introduced. None
moves wall time on its own; recovering the +2.7× wall regression
is paired with the still-pending `kai_field`/`pat_test` balance
work.

### Added

- Three-tier testing discipline: `make tier0` (selfhost +
  demos-no-regression, every commit), `make tier1` (`tier0` +
  full `make test`, every PR), `make daily` (`tier1` + stress
  fixtures + coverage probe + RC budget, on `main` HEAD only).
  Pin spec: `docs/testing-tiers.md`. The PR description must
  include the trailing line of `make tier1` output verbatim;
  without it, the merge does not happen.
- Coverage probe (`tools/coverage-probe.sh`) with a baseline file
  (`tools/coverage-baseline.txt`) ratchet — new gaps fail the
  probe; closed gaps bump the baseline down.
- `LocalUseTable` in `stage0/emit.c`: binding-identity (`Node *`)
  keyed use counter that powers the single-use, non-captured
  fast path in `emit_ident_value` for fn parameters, let-bindings
  (simple + destructuring), and match-arm pattern binds.
- `count_local_uses` + `count_local_uses_in_string`: scope-aware
  walker over the fn body that mirrors the emit-time scope shape
  (push/pop balanced, lambdas push their params with `decl=NULL`
  so they shadow without polluting outer counts), runs once
  before emit, and threads through `#{...}` interpolations the
  same way `collect_free_vars_in_string` does.
- `ls_resolve` helper on the local scope returning the innermost
  matching declarator `Node *`.

### Changed

- `kai_internal_dup` count in `stage1/build/stage1.c` drops
  1381 → 1090 across the two stage 0 deeper-Perceus commits
  (`7ab3d64` cut to 1181 by handling fn params; `b3b1e2f` cut
  another 91 by extending to lets and match-arm binds). Selfhost
  stays byte-identical.
- 8 hand-written `kai_prelude_*` helpers in `stage0/runtime.h`
  flip from borrow to callee-consume (`8bd6431`); the iterative
  refactor of `map`/`filter`/`reduce`/`each` consumes its `xs`
  argument (`73a12d4`).
- `docs/perceus-honesty-targets.md`: numbers refreshed to reflect
  the Tier 2 partials and the deeper-Perceus stage 0 audit.

### Versioning

- Tags v0.1.0 → v0.7.2 retroactive; `bin/kai --version` reads
  `VERSION` dynamically (carried over from the [Unreleased]
  section the v0.11.0 release left untouched).

## [0.13.0] — 2026-04-29 (Fibers Tier 1 — close R4 fiber-discard)

**Minor: scheduler-side fiber RC discipline closes R4.** Discarding a
spawned fiber via `let _ = fiber_spawn(…)` no longer deadlocks the
scheduler. The wrapper `KaiValue` now starts at RC=2 — one ref for
the caller (the user-visible `Fiber[T]` handle) and one for the
scheduler — and the trampoline's DONE/CANCELLED tail
`kai_decref`s the scheduler ref before dispatching the next ready
fiber. Because the decref can bring RC to 0 while we are still
running on the fiber's private stack, `kai_free_value`'s `KAI_FIBER`
branch detects `v->as.fib == kai_current_fiber()` and defers the
struct + stack free into a single-slot `kai_pending_free`; the next
fiber drains the slot at trampoline entry and post-`swapcontext` in
`kai_sched_yield` / `kai_sched_park`.

This is the first item of the **Fibers Tier 1** scope pinned in
`docs/fibers-honesty-targets.md` — the two bugs a curious visitor
trips over in five minutes. Stack guard pages remain open as the
second Tier 1 item, in a separate lane.

### Added

- `KaiValue *value` back-pointer on `KaiFiber` (`stage0/runtime.h`),
  set by `kai_fiber_value(f)` so the trampoline tail can decref the
  scheduler-side ref via `self->value`.
- `kai_pending_free` single-slot global + `kai_drain_pending_free()`
  helper that reaps the deferred struct + stack at the next entry
  point following a context switch.
- `examples/effects/m8_fiber_discard_yields.kai` regression fixture —
  combines `let _ = fiber_spawn(…)` with `fiber_yield()` between
  sends to exercise the discard path through a context switch and a
  resume that drains `kai_pending_free`.

### Changed

- `kai_default_spawn_spawn` allocates the wrapper before enqueue and
  takes its own incref before placing the fiber on the ready queue;
  the old code returned `kai_fiber_value(f)` inside `kai_cont_resume`,
  which left the scheduler holding a raw `KaiFiber *` while the
  wrapper's RC was still 1.
- `kai_fiber_trampoline` drains `kai_pending_free` at entry and runs
  `kai_decref(self->value)` after walking awaiters / link chains and
  before the dequeue + setcontext to the next ready fiber.
- `kai_sched_yield` / `kai_sched_park` drain `kai_pending_free`
  immediately after `swapcontext` returns.
- `kai_free_value`'s `KAI_FIBER` branch defers the struct + stack
  free when `v->as.fib == kai_current_fiber()` (the trampoline tail's
  decref-on-self), and falls back to the immediate-free path when
  the wrapper is dropped from a different fiber.
- `examples/effects/m8_fiber_discard.kai` is no longer aspirational
  — promoted from the `make stress-fixtures` expected-fail block
  into `make test-effects`.
- `demos/ping_pong/main.kai` simplified: the third worker is now
  `let _ = fiber_spawn(…)` so the demo exercises both the discard
  path and the await + result-flow path in a single run. Long
  comment about the R4 workaround removed; round-robin output
  unchanged.

### Fixed

- **R4 — discarding a `Fiber[T]` value (`let _ = fiber_spawn(…)`)
  deadlocks the scheduler.** Documented in
  `docs/known-regressions.md` since 2026-04-29 evening; status
  flipped to FIXED today.

### Internal

- `docs/known-regressions.md` §R4 carries the FIXED writeup with
  root-cause and the verification artifacts (both fixtures + the
  ping_pong demo edit).
- `docs/fibers-honesty-targets.md` Tier 1 row crossed out for R4;
  Tier 2 carry-over row mirrored.

## [0.14.0] — 2026-04-29 (tooling: bin/kai polish + editor syntax)

**Patch-shaped minor: developer-experience polish, no compiler
changes.** This release brings `bin/kai` to a state a first-time
visitor can navigate without reading the source, and ships the
first round of editor support. No effect on the compiler, runtime,
or selfhost path; demos baseline (24) is unchanged.

### Added

- `bin/kai <subcommand> --help`: per-subcommand usage with flags and
  examples (`build`, `run`, `test`). Each subcommand intercepts
  `-h` / `--help` and prints its own block instead of falling back
  to the global help.
- `bin/kai --version`: now prints a 3-line banner with the version,
  the demos baseline read from `demos/baseline.txt`, and a link to
  https://kaikai-lang.org. Still works regardless of the calling
  cwd (`bin/kai` resolves paths against its own location).
- File-not-found errors across `build`, `run`, `test` now read
  `kai: error: file '<path>' not found.` followed by a pointer to
  the relevant `kai <sub> --help`. Replaces the prior cryptic
  `kai: cannot read '<path>'`.
- `tools/kaikai.vim`: Vim 8 / Neovim syntax file. Covers keywords,
  built-in primitives, the Doc B effect catalog, numbers, strings
  with `#{...}` interpolation, char literals, typed holes, and
  pipe / arrow / range operators. Loads cleanly under
  `vim -u NONE` and on `stage2/compiler.kai` (~30k lines).
- `tools/kaikai-syntax.json`: TextMate-format grammar with
  matching coverage. Drop-in for VSCode / Sublime / GitHub web
  rendering. Mapped to standard TextMate scopes so any colour
  theme picks up the highlighting.
- `tools/README.md`: install instructions for both syntax files
  plus a calibrated honesty section about the heuristic limits
  (capitalisation-driven constructor detection, no semantic
  scoping; real LSP-driven highlighting waits on m17).

### Changed

- `bin/kai`'s `usage()` adds an `Environment` line for
  `KAI_NO_STDLIB` and a one-liner pointer to
  `kai <command> --help`. The descriptive line moves from "a
  functional language" to "a statically typed functional language"
  to match the project README.

### Notes

- Out of scope, deferred: shell autocompletion (`kai completion
  bash/zsh/fish`), `kai new` / `kai init` scaffolding, tree-sitter
  grammar, `kai fmt` indentation rules, full LSP integration.
  Each is a separate lane.

## [0.15.0] — 2026-04-29 (m4c #4 Phase 3 — body type / unit substitution)

**Minor: monomorphisation now substitutes types and units in
specialised bodies.** The previous release (0.11.0) emitted one
cloned `DFn` per concrete call-site tuple but left every Expr.ty in
the cloned body pointing at the source's tparam ids — so the
`try_rewrite_show_dim_real` workaround around `Show<Real<u>>` had
to wedge a special-case rewrite at the call site to produce the
unit suffix. This release walks each cloned body via `subst_decl`,
replacing every `TyVarT(id)` and `UVar(id)` whose id is a tparam
binding with the spec's concrete tuple entry. Two consequences:

1. The parametric `impl[u: Unit] Show for Real<u>` now produces one
   spec per concrete unit (`__umono__USD`, `__umono__EUR`,
   `__umono__m_d_s`, …), and each spec's body emits the unit suffix
   natively via `unit_name(x)`. The `try_rewrite_show_dim_real`
   workaround is **deleted** (function + call site).
2. Polymorphic flow-through resolves correctly. A polymorphic
   `outer[a]` calling another polymorphic `inner[a]` via its own
   tparam now retargets to `inner__mono__T` per spec, instead of
   falling back to the polymorphic original. The
   `m4c_flow_through.kai` demo + structural-grep gates pin this
   property.

The structural lie #3 from the 2026-04-28 audit
(`docs/runtime-debt-2026-04-28.md`) — *monomorphisation runs but
the substitution is identity* — is closed end-to-end by this
release.

### Added

- `MonoTuple = MT(String, [Ty], [UnitExprT])`. The unit tuple
  distinguishes specialisations of `impl[u: Unit] Show for Real<u>`
  bound to different concrete units; without it, every call site
  collapsed onto one spec. `mangle_name` encodes the unit tuple
  after the type tuple via `__umono__<unit_mangle>` (composite
  units like `EUR/USD` mangle to `__umono__EUR_d_USD` after the
  unit-ident sanitiser).
- `ResolvedCS = RCS(String, Int, Int, [Ty], [UnitExprT])` and
  `CallSite = CS(String, Int, Int, [Int], [Int])` gain a parallel
  `[Int]` / `[UnitExprT]` slot for unit-tparam fresh ids and their
  resolved values. `mk_fresh_unit_subst` returns the fresh ids;
  `st_instantiate_report` propagates them to the CS push.
- `subst_ty` / `subst_unit` / `subst_expr` / `subst_decl` walkers
  in `stage2/compiler.kai`. Exhaustive over every Ty and ExprKind
  variant — adding a new variant fails `map_expr_kind`'s match
  rather than silently leaking through. `subst_ty` recurses through
  `TyDimT` (substituting both base and unit), `TyFnT`'s row labels,
  and `TyRefineT`'s base.
- `build_subst_map` derives the per-spec `SubstMap` from the source
  decl's tparams via `tparam_id_split`, so the body subst keys
  align with the typer's bound ids.
- `recover_protoimpl_insts`-style inline synthesis: `resolve_call_inst`
  falls back to `synthesize_inst_for_decl` when the typer's CS
  table has no record for `__pimpl_*` calls (which the
  post-typing `resolve_protocol_calls` rewrite produces). The
  inline synth uses the call's actual arg `.ty` per call site, so
  multiple impl calls coalescing onto one (line, col) under string
  interpolation each produce their own (tys, units) tuple.
- `collect_impl_tuples_in_decls` runs at the start of
  `monomorphise` to add inline-synthesised tuples to the worklist
  before pre-mono rewrite. `generate_specs_iter` iterates to
  fixpoint, picking up transitive tuples introduced by per-spec
  body subst.
- `examples/effects/m4c_flow_through.kai` — polymorphic `outer[a]`
  calling polymorphic `inner[a]` via `outer`'s tparam. Two
  structural-grep gates in `make -C stage2 test-m4c` confirm
  `outer__mono__Int` calls `inner__mono__Int` (not the polymorphic
  `inner`) and likewise for `String`.
- m4c Phase 3 invariant: `count_tyvar_in_*` /
  `find_first_leak_in_*` walkers count only TyVarT/UVar ids in the
  spec's `SubstMap` domain. `emit_spec` records a per-spec leak
  record in `MLeakRecord`; `compile_source` calls
  `report_mono_leaks` after `monomorphise` — non-zero count fails
  compilation. Out-of-domain ids (free typer tyvars from clause
  answer types, etc.) are skipped because the codegen erases them.
- `infer_decl` post-finalise canonicalisation pass
  (`canon_uvars_expr` / `canon_tvars_expr` / `canon_resolved_css`):
  for each bound tparam id, chases the typer's substitution to find
  the chain endpoint and rewrites any UVar/TyVarT in the body's
  `Expr.ty` and the recorded CSs back to the bound id. Without
  this, the unifier's `utable_pick_min_var` always picks the bound
  id and binds it to a fresh chain, leaving the body's
  `Expr.ty` referencing fresh ids that the spec's SubstMap doesn't
  cover. Pairs with a small `advance_unit_fresh` bump in
  `infer_decl` so freshly minted uvars don't alias the bound ids
  on the first call.

### Removed

- `try_rewrite_show_dim_real` (function + sole call site in
  `try_rewrite_proto_call`). The four m12.8-phase2 fixtures
  (`m12_8_phase2_show_real_unit`,
  `m12_8_phase2_show_real_bare_and_unit`,
  `m12_8_phase2_show_real_compound`, `m12_8_y_show_real_unit`)
  updated their expected output: their impl bodies omit the unit
  suffix on purpose, so the post-Phase-3 output is the bare
  numeric. The `portfolio` and `usd_to_eur` demos keep their
  unit-suffixed output because the stdlib `protocols.kai` impl
  body explicitly invokes `unit_name(x)`, which Phase 3 now
  resolves correctly.

### Deferred (unchanged from 0.11.0)

- **Generic prune**: dropping the polymorphic original once every
  reference is redirected. `ResolvedCS` is keyed on call-site
  positions, not on bare-name references, so pruning would break
  function-as-value patterns (`let f = my_poly_fn; f(42)`,
  `map(xs, my_poly_fn)`). Adding the missing index plus a separate
  walker that rewrites bare references is the followup that
  unlocks pruning.

### Documentation

- `docs/m4c-real-specialisation.md` — Phase 3 section (this
  release) added under "What landed", with the SubstMap design,
  per-unit specialisation rationale, the post-finalise
  canonicalisation pass, the inline-synth recovery for
  `__pimpl_*` calls, and gate evidence. Phase 4 (generic prune)
  stays in "Deferred".

## [0.11.0] — 2026-04-29 (m4c #4 Phase 2 full — call-site rewrite)

**Minor: monomorphisation now retargets call sites.** The previous
release (0.10.0) emitted specialised copies of every polymorphic
DFn but left every `ECall(EVar(name), args)` pointing at the
polymorphic original — the specialisations were dead code modulo
linker DCE. This release adds the AST walker that rewrites
`ECall(EVar(name), args)` to `ECall(EVar(mangle_name(name, tys)),
args)` whenever the call site's `(name, line, col)` matches a
`ResolvedCS` with concrete tys that produced a specialisation.
After the patch, both the C and LLVM backends route every
concrete-typed call to its specialised body; the polymorphic
original stays in the decl list to support function-as-value
references that `ResolvedCS` does not track. The
`try_rewrite_show_dim_real` workaround around `Show<Real<u>>`
remains in place — removing it depends on body-type substitution
(deferred — the impl-body's `unit_name(x)` still reads the
impl-level uvar even after the body is cloned).

### Added

- `rewrite_callsites_decls` / `rewrite_callsites_decl` /
  `rewrite_callsites_expr` / `rewrite_callsites_kind` /
  `find_mono_tys` in `stage2/compiler.kai`. Mirrors the shape of
  `rename_proto_calls_*` and `resolve_protocol_calls_*`; the only
  rewrite case is `ECall(EVar(name), args)` against the recorded
  `ResolvedCS` table. All other ExprKind variants delegate to
  `map_expr_kind` so adding a new variant fails compilation
  rather than silently leaking through.
- `monomorphise` rewrites `tp.decls` *before* cloning the
  specialised copies, so each spec inherits the rewritten body via
  structural sharing — calls inside a polymorphic body that
  resolved to a concrete `g(...)` retarget to `g__mono__T` in
  every clone, without an extra pass.
- `make -C stage2 test-m4c` adds three new structural-grep gates
  on the C and LLVM emission of `m4c_run_with.kai` and
  `m4c_handler_in_body.kai` that fail if the call sites still
  point at the polymorphic name.

### Fixed

- Specialised copies are now actually invoked. Pre-fix the
  `kai_run_with__mono__Int__Int` / `kai_run_with__mono__String__Int`
  symbols were emitted but every call still went to
  `kai_run_with`; post-fix the specialisations are reached at
  every concrete call site.

### Deferred (unchanged from 0.10.0)

- **Body type substitution**: required to remove the
  `try_rewrite_show_dim_real` workaround around `Show<Real<u>>`.
  The impl body's `unit_name(x)` reads the impl-level uvar after
  identity monomorphisation; substituting Expr.ty across the
  cloned body lets the parametric impl produce the unit suffix
  natively. Today the body is byte-identical across
  specialisations (only the top-level symbol differs).
- **Generic prune**: dropping the polymorphic original once every
  reference is redirected. Today pruning would break
  function-as-value patterns the `ResolvedCS` table doesn't index
  (`let f = my_poly_fn; f(42)` records no CS for the bare-name
  reference).

### Documentation

- `docs/m4c-real-specialisation.md` — Phase 2 — call-site rewrite
  moved from "Deferred" to "What landed in this lane", with the
  keying decision (`(name, line, col)` lookup against
  `tp.insts`), gate evidence, and measurements.

## [0.10.0] — 2026-04-29 (m4c #4 — clause-info plumbing + minimal real specialisation)

**Minor: monomorphisation unblocked.** The `monomorphise` pipeline
pass shipped as identity since stage 2 m4c #1 (early 2026) because
duplicating a polymorphic body that contained an `EHandle` would
mint colliding C symbols for the embedded clauses and the linker
would reject the binary. This release threads the **enclosing fn
name** through the clause-info plumbing so each clause symbol
carries a post-monomorph prefix (`_kai_<enc>__clause_<l>_<c>_<op>`),
and flips `monomorphise` from identity to a minimal real
specialisation pass that emits one cloned `DFn` per distinct
call-site type tuple. The collision-avoidance is exercised
end-to-end by the new
`examples/effects/m4c_handler_in_body.kai` regression demo (a
polymorphic body with an embedded `EHandle` called at two distinct
(a, b) tuples produces three distinct clause C symbols and the
binary links and runs on both backends). One of the three
"structural lies" called out in the 2026-04-28 audit
(`docs/runtime-debt-2026-04-28.md`) — that monomorphisation didn't
actually monomorphise — is closed in scope; full call-site rewrite
(redirect every `EVar` to its specialised name) and body type
substitution (which removes the `try_rewrite_show_dim_real`
shortcut around `Show<Real<u>>`) stay deferred for a follow-up
lane. Both follow-ups are pinned in
`docs/m4c-real-specialisation.md` with concrete shape + cost.

### Added

- `ClauseInfo` carries an 8th `String` field with the post-monomorph
  enclosing fn name. The collect walker (`lc_record_clauses`,
  `collect_decl`, `collect_expr`'s `ELambda` arm) populates it from
  `LamCollect.cur_enc_fn`, swapping the value at scope boundaries.
- `clause_fn_name(enc_fn, line, col, op)` mints the prefixed C
  symbol; both the install path
  (`emit_clause_assignments`/`llvm_emit_handle_clause_assigns`) and
  the body emit path (`emit_clause_sig`/`emit_clause_body`/
  `llvm_emit_clause_body`) feed it the same `enc_fn` so install and
  body symbols match.
- C emit family: `cls: [ClauseInfo]` threaded alongside `lams` and
  read by `emit_clause_assignments` via `lookup_clause_enc_fn` to
  find the matching enc_fn at install.
- LLVM emit: `LlvmEmit` gains a `cls` field populated once at the
  top of `emit_program_llvm`; `llvm_emit_handle` reads `e.cls` for
  the same lookup.
- `monomorphise` flips from identity to real: walks polymorphic
  DFns (non-empty tparams), gathers distinct concrete (name, [Ty])
  tuples from `tp.insts`, and emits one cloned DFn per tuple with
  `mangle_name` applied and `tparams = []`.
- New regression demos
  `examples/effects/m4c_run_with.kai` and
  `examples/effects/m4c_handler_in_body.kai`. The first exercises a
  polymorphic helper called at two tuples under a single handler
  (call-site handler, no clauses inside the polymorphic body); the
  second exercises the actual collision case (handler installed
  inside the polymorphic body, two specialisations).
- New `make -C stage2 test-m4c` target wires both demos into the
  umbrella `test` target; gates assert both backends emit + link +
  run, the C output carries the expected mangled symbols, and the
  collision-prevented clause symbols are present.

### Fixed

- Monomorphisation no longer ships as identity. Polymorphic decls
  with concrete call-site type tuples now produce specialised C
  copies. The change is selfhost-byte-identical because
  `compiler.kai` itself has no `EHandle` blocks (the prefixed
  naming is a no-op for self-compilation) and no polymorphic decls
  with concrete instantiations recorded against them at the same
  source position twice.

### Deferred

- **Call-site rewrite**: today the specialisations are emitted
  alongside the polymorphic original and the original is what gets
  called. The C linker's DCE may strip the unused symbols at link
  time but they ARE in the source. Real call-site rewrite
  (`ECall(EVar(name)) → ECall(EVar(mangled))`) needs a deep AST
  walker shaped like `rename_proto_calls_*` (~300 LOC of
  mostly-mechanical recursion). Pinned for design review per the
  lane's design-decision policy.
- **Body type substitution**: required to remove the
  `try_rewrite_show_dim_real` workaround around `Show<Real<u>>`.
  The impl body's `unit_name(x)` reads the impl-level uvar after
  identity monomorphisation; substituting Expr.ty across the
  cloned body lets the parametric impl produce the unit suffix
  natively. Coupled with the call-site rewrite — both must land
  together, so deferred together.
- **Generic prune**: dropping the polymorphic original once every
  reference is redirected. Today pruning would break
  function-as-value patterns the `ResolvedCS` table doesn't index.

### Documentation

- `docs/m4c-real-specialisation.md` — new design / lane doc.
  Covers what landed in Phase 1 (plumbing) and Phase 2 minimal
  (specialisation generation), what is deferred (call-site
  rewrite, body type substitution, generic prune), the gate
  evidence, and the measurements.
- `docs/m5x-followup.md` §3 marked LANDED with the m4c #4
  reference.
- `docs/known-regressions.md` — pinned R-interp (pre-existing
  stage-2 codegen panic on `examples/minimal/interp.kai`) and
  R-m8x2 (effect-runtime stack overflow under default ulimit on
  `examples/effects/m8x_2_yield_interleave.kai`). Both pre-existing
  on `main` HEAD, out of m4c lane scope.

## [0.9.2] — 2026-04-29 (var sugar — didactic errors + nested handler closure capture)

**Patch: var sugar correctness.** Three changes that close a
long-standing m7b #15 sub-followup. Nested `var` bindings now
work end-to-end across a lambda boundary, the runtime no longer
aliases the State slot pointer, and bare reads of a cap binder
report a Tier-3-style diagnostic at the call site instead of
crashing inside `cc`.

### Fixed

- **State runtime aliasing** (`stage2/compiler.kai`,
  `var_canonical_clauses`): the canonical `with State[T] as
  name` get clause now returns `__perceus_dup(state)` so
  `self->state` keeps its refcount when the caller consumes
  the value. Pre-fix `var n = 0; while @n < lim { n := @n + 1
  }` crashed with `kai: type mismatch in +` once the slot was
  reused after a single get/consume cycle.
- **Closure capture of `kai_alias_<a>_id`** for per-instance
  dispatch (`fv_expr`, `emit_closure_caps`, `emit_lam_cap_reads`,
  `synth_lambda`, `rewrite_alias_kind`): tagged op calls
  (`Eff.op@<alias>`) now stay tagged across lambda boundaries.
  The collector adds an `__alias_id__<alias>` sentinel to each
  lambda's free-var set, the closure literal packs the matching
  `KaiHandlerId` as a `kai_int`, and the body prologue unpacks
  it back into the local the dispatch expects. Nested
  lambdas now also propagate their free vars to the outer
  closure's capture set so the inner `kai_closure(...)` literal
  sees every value it references.
- **Spiral demo unblocked**: rewritten with the correct cap
  syntax (`@name`, `name := <expr>`) and now compiles + runs
  cleanly. 4×4 clockwise spiral output stable. Demo baseline
  22 → 23.

### Added

- **Didactic error for bare cap reads** (`validate_var_uses_decls`
  in `stage2/compiler.kai`): a new pre-desugar pass that walks
  every fn / test body with a stack of in-scope cap binders
  and reports each bare `EVar(name)` whose `name` is a live
  cap binder. Reports a span-aware diagnostic with help line:

  ```
  error: `top` is a State capability bound by `var top = ...` —
         bare reads are not allowed
    --> file.kai:12:11
       |
    12 |   while { top <= bottom and left <= right } {
       |           ^
    = help: use `@top` to read the cell, `top := <expr>` to write it;
            cap binders are not ordinary locals
  ```

  Legitimate uses (`name.get()`, `name.set(v)`) and shadowing
  (let / var / lambda params / match patterns) are recognised
  and pass through. Pre-fix the bare read leaked to the C
  compiler as `kai_<name>` undeclared, with no .kai location
  in the diagnostic.

### Validation

- selfhost (C + LLVM) byte-identical fixed point.
- `make test` clean.
- `make demos-no-regression` 23 (baseline raised 22 → 23).

## [0.9.1] — 2026-04-29 (resolver same-module preference + banker's rounding)

**Patch: same-module preference + new rounding mode.** Two
quick-win lanes:

### Fixed

- **Resolver same-module preference** (`stage2/compiler.kai`):
  when several modules export the same name with the same arity
  AND no root-file shadow exists, `efn_resolve` now falls back
  to the first match instead of returning ambiguous. The C and
  LLVM backends rotate the EFn table via `fns_prefer_module(fns,
  mo)` before walking each DFn body, so a recursive call inside
  `stdlib/loop.repeat` resolves to `loop.repeat` even when
  `stdlib/core/list.repeat` is also in scope.
- `stdlib/core/list.kai`: `repeat` and `list_repeat` both
  delegate to a new internal `list_repeat_loop`. This keeps the
  recursive call off the global namespace so the same-module
  preference is not even exercised on stdlib's recursion path.

### Added

- `stdlib/math/real.kai`: `real_round_half_even` (banker's
  rounding, IEEE 754 default). Previously listed as deferred in
  v0.6.0. Ten cases added to `examples/stdlib/math_real_basic`.

### Validation

- selfhost (C + LLVM) byte-identical fixed point.
- `make test` clean.
- `make demos-no-regression` 22 (baseline 22 unchanged — `spiral`
  still fails, but on a different bug in the `var` desugar; see
  `docs/known-regressions.md`).

## [0.9.0] — 2026-04-29 (`^` value-level operator + canonical unit pretty-print)

**Minor: new operator surface and prettier UoM render.** Resolves
the asymmetry where `^` parsed only inside unit expressions
(`Real<m^2>`) but not at the value level: now `r ^ n` works on
`Int`, `Real`, and dimensioned `Real<u>`. The unit pretty-printer
also drops the spurious `* 1` sandwich it produced for `m / s` —
the same change improves `unit_name`, `impl Show for Real<u>`,
typer diagnostics, and any AST/IR dump that surfaces unit text.

### Added

- **`^` operator at expression level** (`stage2/compiler.kai`):
  - Parser: new `parse_pow` between unary and postfix; right-
    associative (`2 ^ 3 ^ 2 = 512`), higher precedence than
    `*`/`/`. Negative literal exponents lex via `EUnop("-",
    EInt(n))` so `r ^ -1` parses correctly.
  - Typer: `synth_dim_pow` extends `synth_binop` with UoM-aware
    rules. When the base has a non-trivial unit, the exponent
    must be a compile-time literal Int (positive or negative);
    `Real<u> ^ N` lifts to `Real<u^N>`. Bare numeric bases accept
    any Int exponent. Non-literal exponent + dimensioned base is
    rejected with a targeted diagnostic.
  - Runtime: `kai_pow_int(a, b)` in `stage0/runtime.h` and the
    `kaix_pow_int` LLVM bridge in `stage0/runtime_llvm.c`. Int
    bases clamp negative exponents to 0; Real bases compute
    `1.0 / base^|n|` so the unit lift at the type level matches
    a meaningful runtime value.
  - Codegen: `binop_cname` and `llvm_binop_helper` route `^`
    through the runtime helper without protocol dispatch (the
    operator does not depend on `Numeric` being in scope).
- `examples/protocols/pow_operator.kai` (+ golden) and
  `pow_operator_neg.kai` (+ `.err.expected`): cover bare numerics,
  unit lift, right-associativity, negative literal exponent, and
  the non-literal-exponent rejection.
- `demos/free_fall/`: physics demo (`h(t) = h0 - (1/2) g t^2`)
  showcasing `^` and UoM together. Demo baseline 21 → 22.

### Changed

- **Pretty-print of canonical unit expressions** (`unit_expr_display`,
  `stage2/compiler.kai`): scientific style. Multiplication is a
  single space (`kg m`), division `/` is unspaced (`m/s`,
  `kg m/s^2`). `UMul(a, UInv(b))` collapses to `a/b` directly so
  `m / s` no longer surfaces as `m * 1 / s`. Trivial `UOne`
  factors drop out. Refresh of three goldens
  (`examples/usd_to_eur/`, `examples/protocols/m12_8_phase2_show_real_compound`,
  and any future fixtures using compound units).

### Validation

- selfhost (C + LLVM) byte-identical fixed point.
- `make test` clean (test-protocols includes both new fixtures).
- `make demos-no-regression` 22 (baseline 22, raised from 21).

## [0.8.1] — 2026-04-29 (resolver — local DFn shadows same-arity protocol op)

**Patch: resolver-local-shadow.** Companion to v0.7.2's resolver-arity
fix. The pre-resolve dispatcher rewrite now also drops every protocol
op entry whose `(name, arity)` is provided by a top-level `DFn` in
the compilation unit. Same-arity collisions like `fn show(stack:
[Int])` vs `protocol Show { show(x: Self) }` resolve to the local
fn instead of routing to `__proto_show` and failing with the
misleading `error: no impl of \`Show\` for type \`List\``. The
protocol stays reachable through a qualified call (`Show.show(x)`).

### Fixed

- `stage2/compiler.kai`: `lower_protocols` collects local DFn arities
  from the post-strip decl stream and filters `op_arities` through
  `filter_shadowed_ops` before the rewrite walks user fns or impl
  bodies. The arity-1 `pub fn min(xs)` / arity-2 `Ord.min(a, b)`
  split is unchanged — different arities never get filtered.

### Added

- `examples/protocols/resolver_local_shadow.kai` (+ golden):
  `fn show(stack: [Int])` shadows `Show.show(x: Self)`; both call
  forms (`show(xs)` and bare `let f = show`) resolve to the local
  fn.

### Validation

- selfhost (C + LLVM) byte-identical fixed point.
- `make test` clean (test-protocols includes the new fixture).
- `make demos-no-regression` 21 (baseline 21).

## [0.8.0] — 2026-04-29 (stdlib extras — int↔real builtins, math/real rounding, path module)

**Minor: new user-visible stdlib surface.** Triple-combo lane that
lands three mechanically-related stdlib extras:

### Added

- **Phase A — runtime builtins** (`20b7f8e`): `int_to_real(n: Int) :
  Real` (lossless cast) and `real_to_int(x: Real) : Int` (truncating
  toward zero). Wired through every layer: `stage0/runtime.h`,
  `stage0/runtime_llvm.c`, `stage0/check.c`, `stage0/emit.c`,
  `stage1/stage2 compiler.kai` (prelude_names + EP table), stage2
  typer (TyEntry) + LLVM declarations + LP table.

  `real_to_int` matches C's `(int64_t) r` semantics: NaN, Inf, and
  out-of-Int64-range values collapse to 0 — kaikai has no IEEE 754
  predicate surface for the user yet, so the safer default beats UB.

- **Phase B — `stdlib/math/real.kai` rounding** (`78667a0`): four
  rounding ops, all pure kaikai over Phase A:
  - `real_trunc` — toward zero.
  - `real_floor` — toward -inf.
  - `real_ceil` — toward +inf.
  - `real_round` — half-away-from-zero (schoolbook).

  These were marked "deferred — need int_to_real / real_to_int
  runtime builtins" in v0.6.0; Phase A unblocks them. Banker's
  rounding (round-half-to-even) stays deferred.

- **Phase C — `stdlib/path.kai`** (`9fa5f3e`): new module with
  seven public fns over POSIX paths (`/` separator only):
  - `path_join` / `path_dirname` / `path_basename`
  - `path_ext` / `path_strip_ext`
  - `path_split` (returns `Pair[dirname, basename]`)
  - `path_is_absolute`

  Pure kaikai over the existing string builtins — no FFI, no
  syscalls. Edge cases verified: leading-dot files (.bashrc has no
  extension), dots in dirname (`a.b/foo` has no extension), multiple
  consecutive slashes (`a//` joined with `//c` yields `a/c`),
  absolute paths preserved through join with root `/`. Wired into
  `bin/kai` and `stage2/Makefile EXTRA_PRELUDE_FLAGS`.

- New fixtures:
  - `examples/stdlib/int_real_convert_basic` — Phase A round-trip.
  - `examples/stdlib/math_real_basic` — extended with 16 rounding
    checks.
  - `examples/stdlib/path_basic` — Phase C, 37 checks across all 7 fns.

### Validation

- `make selfhost` byte-identical fixed point.
- `make -C stage2 selfhost-llvm` byte-identical.
- `make test` clean (incl. 3 new fixtures).
- `make demos-no-regression` 21 passing (baseline 21).

## [0.7.2] — 2026-04-29 (resolver: arity-aware overload resolution)

**Patch: compiler-deep resolver fix.** The pre-resolve dispatcher
rewrite (`rename_proto_calls_kind` in `stage2/compiler.kai`) is now
arity-aware: when the callee of `ECall(EVar(name), args)` matches a
protocol op name but the call's argument count does not match a
declared arity for that op, the rewrite is skipped and the typer
falls back to the regular EFn lookup. Same-named module functions
with a different arity now coexist with protocol ops. This unblocks
adding `min(a, b)` / `max(a, b)` to the `Ord` protocol alongside the
existing `pub fn min(xs: [Int])` / `pub fn max(xs: [Int])` in
`stdlib/core/list.kai`. Before the fix the rewrite was unconditional
and `max(xs)` mis-dispatched to `__proto_max` (arity 2), surfacing as
the misleading `error: no impl of \`Ord\` for type \`List\``.

### Added

- **`stage2/compiler.kai`**:
  - `type OpAr = OpAr(String, Int)` records each protocol op's
    declared arity.
  - `op_arities_from_ops`, `op_ar_has_name`, `op_ar_match` helpers.
  - `rename_proto_calls_kind` adds an explicit `ECall(EVar(name), args)`
    branch: when `name` is a protocol op and the call's arity does
    not match, the callee is preserved verbatim so normal name
    resolution can find a same-named module function.
  - `lower_protocols` projects `reg.ops` into `[OpAr]` once and
    threads it through the rewriter (replacing `reg.op_names`).
- **`stdlib/protocols.kai`**: `protocol Ord` gains `min(a, b)` and
  `max(a, b)`; impls extended for `Int` / `Real` / `String` / `Char`.
- **`examples/protocols/resolver_arity_overload.kai`** + golden:
  declares a protocol op + module function sharing a name with
  different arities; both call sites compile and run.

### Validation

- `make -C stage2 selfhost`: byte-identical fixed point in 1 round.
- `make -C stage2 selfhost-llvm`: byte-identical fixed point.
- `make -C stage2 test`: clean.
- `make demos-no-regression`: 20 passing (baseline 20).

## [0.7.1] — 2026-04-29 (--effect-holes-json — typed effect-hole resolutions)

**Patch: developer-facing CLI flag.** Adds `--effect-holes-json` to
the compiler driver. Emits a JSON array describing every typed effect
hole (`?` or `?name` in row tail position) the program contains plus
how the inferencer resolved each one. Closes the `?e` reporter half
of Phase L (LLM authorability — the L1 cross-call effect propagation
half is still pending).

### Added

- **`stage2/compiler.kai`**:
  - `ResolvedRowHole(name, line, col, fn_name, resolved_row,
    body_row)` recorded per `?` / `?name` occurrence.
  - `TypedDecl` + `TypedProgram` extended with `row_holes` field.
  - `infer_decl` extracts row-hole resolutions after the per-decl
    substitution is final.
  - Placeholder parser (`split_row_hole_name`,
    `find_last_underscore`, `substring_range`, `parse_decimal`)
    recovers source name + position from the parser-minted
    `__rowhole_<name>_<line>_<col>` placeholders.
  - `dump_effect_holes_json` + `json_row_hole_report` emit the JSON.
  - Driver: new `MEffectHolesJson` mode + `--effect-holes-json`
    flag + help line. Existing `--effects-json` help line was
    missing too — added alongside.
- **Schema** (stable, per element):
  `{file, fn, line, col, name, labels, open, tail_var, body_row}`.
  - `name`: source-written name (`"e"` for `?e`) or `null` for
    bare `?`.
  - `labels`: effect labels actually unified into the row var. In
    practice usually empty: `check_body_row` treats `ROpen` rows as
    unconstrained and never unifies the tail.
  - `body_row`: the *useful* field for an LLM consumer — labels
    actually performed by the fn body, in declaration order. This
    is what would replace `?e` to produce a closed signature.
  - `open` + `tail_var`: explicit signal that the row var is still
    free and what its compiler-internal id is.
- **`examples/sugars/effect_holes_json_basic.kai`** + golden:
  exercises three shapes (`?e` with Stdout, `?eff` with
  Stdout+Stderr, `?p` with no effects); picked up by `test-sugars`.

### Validation

- `make test` clean.
- `make demos-no-regression`: 20 passing (baseline 20).

## [0.7.0] — 2026-04-29 (math/numeric — single-dispatch `Numeric` protocol)

**Minor: new stdlib protocol.** Single-dispatch `protocol Numeric` in
`stdlib/math/numeric.kai` collapses the dual-surface (`int_abs(5)` vs
`real_abs(3.5)` vs `dec_abs(d)`) into one polymorphic call: `abs(x)`,
`sign(x)`, `pow_int(x, n)`, `clamp(x, lo, hi)`. Lives under `stdlib/math/`
because it is domain-specific (numeric types only) — `stdlib/protocols.kai`
is reserved for protocols universal to any type (Show, Eq, Ord, Hash,
Serialize).

### Added

- **`stdlib/math/numeric.kai`** (new): protocol declaration + impls
  for `Int` and `Real` (4 ops × 2 types = 8 method bodies).
- **`stdlib/decimal.kai`** appended `impl Numeric for Decimal` (4
  ops; `pow_int` uses recursive `dec_mul`, `clamp` wraps `dec_cmp`).
- **`stdlib/math/real.kai`** (renamed from `float.kai`): keeps only
  `real_pi`/`real_e`/`real_tau` constants and prefixed `real_min`/
  `real_max`. abs/sign/pow_int/clamp moved to the Numeric impl.
- **`stdlib/math/int.kai`**: `int_abs` / `int_pow` removed (now
  `abs(5)` / `pow_int(5, 3)`). `gcd` / `lcm` / `factorial` / `fib` /
  `is_prime` stay (Int-specific). `int_min` / `int_max` stay
  prefixed for the same reason `real_min`/`real_max` do.
- **`bin/kai` + `stage2/Makefile`**: `numeric.kai` loads before
  `int`/`real` so the impls see the protocol declaration.
- **`examples/stdlib/math_int_basic`** + **`math_real_basic`**
  (renamed from `math_float_basic`): updated to call `abs`/`sign`/
  `pow_int`/`clamp` polymorphic.

### Changed

- `stdlib/math/float.kai` → `stdlib/math/real.kai` (file rename;
  the type is `Real`, not `Float`).
- `examples/stdlib/math_float_basic.*` → `math_real_basic.*` (rename).

### Not in this release

- **`min` / `max` NOT added to `Ord`**: collides with
  `pub fn min(xs)` / `pub fn max(xs)` in `stdlib/core/list.kai`
  (different arity, same surface name). Resolver prioritises
  protocol dispatch even when arity does not match → typer error
  at the `list.max(xs)` call site. Two paths unblock: (a) rename
  list.min/max to list.minimum/maximum (Haskell convention);
  (b) fix resolver to fall back to module fns when no protocol
  impl matches. Until then `int_min` / `real_min` etc. stay
  prefixed.
- **`Numeric` NOT implemented for `Money`**: `pow_int` has no
  meaning for Money (Money^Int undefined). abs/sign/clamp would
  work but are partial (clamp requires same currency). Money keeps
  its `money_*` prefixed surface; if a `Currency` / `Tagged`
  protocol with a smaller method set emerges, Money picks those up.

### Validation

- Selfhost C + LLVM byte-identical.
- `make test` clean (incl. new Numeric for Decimal exercises in
  `math_*_basic`).
- `make demos-no-regression`: 20 passing (baseline 20).

## [0.6.0] — 2026-04-28 (stdlib/math/float — Real math helpers)

**Minor: new stdlib module.** Pure-kaikai `Real` math helpers mirroring
`stdlib/math/int.kai`'s shape. No new runtime builtins; v1 ships what's
expressible from `+`, `-`, `*`, `/`, comparisons.

### Added

- **`stdlib/math/float.kai`** (`addf6a3`, PR #14):
  - 3 constants: `real_pi`, `real_e`, `real_tau` (18 significant digits).
  - 6 functions: `real_abs`, `real_sign`, `real_min`, `real_max`,
    `real_clamp`, `real_pow_int`.
- **`bin/kai`** loads the new module in its prelude chain alongside
  `stdlib/math/int.kai`.
- **`stage2/Makefile`**: `EXTRA_PRELUDE_FLAGS` includes
  `--prelude ../stdlib/math/float.kai` so `test-stdlib` picks it up.
- **`examples/stdlib/math_float_basic`** + golden: 25 OK lines with
  ε = 1e-4 closeness check.

### Deferred

Functions needing new runtime intrinsics or libm via FFI:
- `real_floor` / `real_ceil` / `real_round` / `real_trunc` — need
  `int_to_real` + `real_to_int` runtime builtins.
- `real_sqrt` / `real_sin` / `real_cos` / `real_log` / `real_exp` —
  need libm via FFI (would also feed `Crypto` / `Net` effects).

### Validation

- Selfhost C + LLVM byte-identical.
- `make -C stage2 test-stdlib`: math_float_basic OK + 19 other
  fixtures still green.
- demos-no-regression: 20 passing (baseline 20).

## [0.5.1] — 2026-04-28 (m14 v1.E — char module rename to bare names)

**Patch: char surface added; option / result / string deferred.** Renames
`stdlib/core/char.kai` definitions from the `ch_*` prefix to bare names
(`is_digit`, `is_lower`, `is_upper`, `is_alpha`, `is_alnum`, `is_space`,
`to_lower`, `to_upper`). Adds 8 legacy alias trampolines so existing call
sites that spell `ch_is_digit(c)` keep compiling unchanged.

### Added

- **m14 v1.E — char module rename** (`065cf50`, PR #13): `char.is_digit('7')`
  qualified surface plus bare-name resolution (`is_digit('7')` resolves
  to the char module via m6.2 v2 minting). Legacy `ch_*` aliases preserved
  at the bottom of the file.

### Deferred (with cause)

- **m14 v1.B / v1.C / v1.D — option / result / string renames**: attempted
  in the same lane and reverted. Cross-module bare-name collisions on
  `map`, `and_then`, `unwrap_or`, `repeat` cause the resolver to pick the
  wrong module without type-directed dispatch:
  - `examples/stdlib/base64_basic.kai` compiles `option.map`-style calls
    as `result.map` post-rename → type errors.
  - `stdlib/encoding/base64.kai` uses bare `repeat(s, n)` for `String`;
    resolver picks `list.repeat` → type errors.

  Char has no such collisions (no other module exports `is_digit`,
  `is_alpha`, etc.) so it migrates safely. The remaining v1.B-E modules
  wait for **type-directed dispatch in the typer** — when `map(x, f)` can
  be resolved to the right module from the type of `x`, the renames
  become safe. Tracked in `docs/stage2-design.md` §m14 follow-ups.
- **Cleanup of legacy aliases** (29 in list, 8 in char, more once
  v1.B-E lands): the agent that landed m6.2 v2 noted the aliases as
  "redundant", but a foreground experiment showed 130+ legacy bare-name
  call sites across 18 files (`list_is_empty`, `opt_map`, `string_concat`,
  etc.) still reach the aliases — the resolver does not auto-rewrite
  `list_is_empty(xs)` into `list.is_empty(xs)`. The aliases stay until
  every caller migrates to the qualified or bare-name surface.

### Validation

- `make selfhost` byte-identical fixed point: OK.
- `make -C stage2 selfhost-llvm` byte-identical fixed point: OK.
- `make test` full suite green.
- `make demos-no-regression`: 20 passing (baseline 20).

## [0.5.0] — 2026-04-28 (m6.2 v2 — universal prefixed C-symbol minting)

**Minor: linker namespace changed for module-owned decls.** Every public
fn defined in a `--prelude`-loaded file or a transitively imported module
now mints to `kai_<module>__<name>` instead of `kai_<name>`. Decls
authored in the root file (the user program) keep `kai_<name>` flat. The
shape of the AST `DFn` and the codegen `EFn` table both grew an
`Option[String]` `module_origin` field; the parser produces `None` and
the prelude / import loaders stamp the surface module name when concat-
ing decls into the global stream.

### Added

- **m6.2 v2 — universal prefixed minting** (this release):
  - Decl-side: every imported / pre-loaded `pub fn` mints
    `kai_<module>__<name>` in both the C backend (`emit_fn_signature`,
    `emit_thunk_forward`, `emit_fn_forward`, `emit_fn_thunk`,
    `emit_fn_body`) and the LLVM backend (`llvm_emit_fn`,
    `llvm_emit_thunk`).
  - Use-side: `EModCall(mod, fname)` codegen routes the module surface
    name straight into the symbol it emits (no round-trip through
    `efn_resolve` that would lose the qualified context). Bare-name
    `EVar(name)` resolution goes through `efn_resolve` which prefers
    a root-file definition over module-owned ones (Rust / Python /
    OCaml's locals-shadow-imports rule applied at the top-level
    scope).
  - Ambiguity diagnostic: `find_ambig_efns` walks `[EFn]` for names
    exported by ≥2 modules with no root-file shadow; `report_ambig_efns`
    emits a stderr warning at the start of each codegen run pointing
    at the ambiguous pair (e.g. `option.map` vs `result.map` once
    those renames land in m14 v1.B-E). Lazy: warns even if no caller
    uses the bare form, harmless when the user qualifies every site.
  - Regression fixture `examples/modules-qualified/two_modules_same_name`
    exercises two modules exporting `pub fn wrap`; pre-v2 the C
    output collided as duplicate `kai_wrap`; post-v2 they mint to
    distinct `kai_box_a__wrap` / `kai_box_b__wrap`. Wired into
    `make test-modules-qualified`.
- Demo goldens: `demos/euler1/main.out.expected` + `demos/quicksort/
  main.out.expected`. Pre-v2 both demos failed at link time because
  the demo's local `fn sum` (resp. `fn sort`) collided with
  `pub fn sum` (resp. `pub fn sort`) in `stdlib/core/list.kai` — both
  minted `kai_sum` / `kai_sort`. v2 mints the prelude entries as
  `kai_list__sum` / `kai_list__sort` so the demo locals shadow them
  silently. `demos/baseline.txt` bumped from 18 → 20.

### Changed

- `DFn(...)` carries a 10th field `module_origin: Option[String]`.
  All ~30 pattern / construction sites in `stage2/compiler.kai`
  thread it through unchanged. Stage 1 + stage 0 are not touched —
  stage 2 selfhost (`make selfhost`, `make selfhost-llvm`) does not
  load preludes, so every decl seen by stage 1 has `module_origin =
  None` and the bootstrap chain stays byte-identical.
- `EFn(name, arity)` → `EFn(name, arity, module_origin)`. New
  `efn_resolve` returns the full entry; `efn_find_arity` keeps the
  pre-v2 `Option[Int]` shape.

### Unblocks

m14 v1.B-E renames (the per-module bare-name migration deferred from
m14 v1 because two modules exporting the same name would collide on
the C symbol). With v2 in place, `pub fn opt_map` → `pub fn map` in
`stdlib/core/option.kai` (and the equivalents in result.kai / string.kai
/ char.kai) become safe — each lands on its own prefixed C symbol.
The rename itself is a separate lane.

### Validation

- `make selfhost` byte-identical fixed point: OK.
- `make selfhost-llvm` byte-identical fixed point: OK.
- `make test` full suite green (lex / parse / typer / runtime / LLVM
  + qualified-call positive + negative + the new
  `two_modules_same_name` fixture).
- `make demos-no-regression`: 20 passing, baseline 20 (two demos
  flipped from FAIL to OK without source edits).

### Deferred

- The lazy ambiguity walker that only warns when a bare-name call
  actually uses an ambiguous name (rather than warning unconditionally
  on every compile). Filtering would cost a body walk on every
  invocation; the unconditional warning is cheap and silent in the
  common case where stdlib modules pick distinct bare names.

## [0.4.1] — 2026-04-28 (Stage 1 codegen shadow-bug fix — symmetry with stage 2)

**Patch: insurance fix.** Mirrors the m14 v1.x codegen shadow-bug fix from
`stage2/compiler.kai` to `stage1/compiler.kai`. Stage 2 source does not
currently exercise the local-shadow-global pattern, so the bootstrap chain
was not breaking — but the asymmetry with stage 2 was real, and any future
stage 2 code that did `let drop = ...; drop(x)` (with `pub fn drop`
reachable through the prelude) would have silently emitted the wrong call.

### Fixed

- **`stage1/compiler.kai` codegen shadow bug** (`1cb7b63`, PR #10):
  - New `emit_named_call_lookup` / `emit_pipe_named_lookup` helpers
    mirror the stage 2 design.
  - `emit_call_expr`'s `EVar` branch and the two `EVar` branches in
    `emit_pipe_expr` now route through the new helpers, which
    short-circuit to `kai_apply(kai_<callee>, ...)` over the local C
    variable when the callee name is in `locals` instead of resolving
    to a same-named top-level (variant / prelude / user fn).
  - `emit_ident_value` already had the `locals` guard from the m12.6
    kaic1 pattern-capture fix; the call paths did not — same bug shape
    as stage 2 pre-v1.x.

Closes the symmetry gap noted under "Items deferred from m14 v1" #3 in
`docs/lane-experience-m14-v1.md`.

## [0.4.0] — 2026-04-28 (R2 — m8.x cooperative fiber scheduler)

**Minor: runtime semantics changed.** m8 v1's inline-eager scheduler is
replaced with a real cooperative runtime built on POSIX `ucontext`.
After this release, the BEAM-style structured-concurrency claim from
`docs/structured-concurrency.md` and `docs/actors.md` is no longer
type-check-only — fibers actually suspend, `await` actually parks,
cancellation actually propagates, and mailbox blocking actually parks
senders/receivers.

R2 closes the second of the three structural-debt audit findings from
2026-04-28 (Linus + Eric). After this release: R1 (v0.2.0 — Perceus
runtime flip) + R2 (this) done. R3 (effect-row strictness) remains
queued.

### Added

- **R2 Phase 2 — cooperative scheduler core** (`5de03d2`): `KaiFiber.ctx`
  via POSIX `ucontext` (`swapcontext` / `makecontext`), run-queue
  primitives, 5 Spawn handler rewrites (spawn / await / yield / select
  / cancel), `docs/fibers-impl.md` substrate design doc.
  Demo: `examples/effects/m8x_2_yield_interleave.kai` — two yielding
  fibers genuinely interleave (`A0 B0 A1 B1 A2 B2`).
- **R2 Phase 3 — Cancel delivery at yield points** (`c8aa1a7`):
  `setjmp` landing pad in trampoline, `kai_check_cancel_yield_point`
  hook in all `kai_evidence_lookup*`. Demo:
  `examples/effects/m8x_3_cancel_at_yield.kai` — worker cancelled
  mid-loop, body unwinds to `KAI_FIBER_CANCELLED`.
- **R2 Phase 4 — blocking mailbox primitives** (`4cb56c9`):
  per-mailbox sender/receiver waiter queues. `Actor.receive` parks
  on empty mailbox, `Bounded(_, BlockSender)` parks senders on full.
  Demos: `m8x_4_recv_blocking.kai` + `m8x_4_block_sender.kai`.
- **R2 Phase 5 — Link runtime registry** (`55e6152`):
  `KaiMailbox.owner_fiber`, `KaiFiber.linked_head`, EvLink + default
  handler `kai_default_link_link`, trampoline propagation on
  termination. C-level smoke test in `stage2/tests/link_runtime_test.c`.
- **R2 Phase 6 — region-brand v1** (`97f3f28`): generalised
  `is_fiber_producer_helper` allow-list + recursive walker that rejects
  `Fiber[T]` escape through `TyName` / `TyList` / `TyFn` / `TyDim` /
  `TyRefine`. Demo: `m8x_6_fiber_in_result` (negative — `Fiber[T]` in
  `Result`'s second type-arg rejected).

### Substrate decision

**Path A — `ucontext` (POSIX)** over Path B (full CPS reification
through `KaiCont`). Defended in `docs/fibers-impl.md` §*Substrate*:
containment, no parser/typer/CPS/emitter changes, MVP-target alignment
(macOS arm64 + Linux x86_64/aarch64), and the runtime was already
structured for it. Per CLAUDE.md "Do not design against WASM"; the
deprecation status of `ucontext` in POSIX-2008 is acknowledged but
not a blocker for MVP.

### Critical bug fixed during Phase 2

`#define _XOPEN_SOURCE 600` MUST sit ABOVE every system include, not
just above `<ucontext.h>`. If an earlier header (`stdio.h`, `time.h`,
…) transitively pulls in `<sys/types.h>`, the feature-test macro is
frozen and `ucontext_t` compiles as the legacy 56-byte shape instead
of the full 880-byte XSI shape on darwin arm64. `swapcontext` then
writes 880 bytes into a 56-byte slot, silently corrupting adjacent
memory — in this case `kai_main_fiber.evidence_top` and the static
evidence nodes laid out next to it. Fix and rationale pinned in
`stage0/runtime.h` above the include.

### Deferred (post-MVP)

Pinned in `docs/m8x-followup.md` and `docs/fibers-impl.md`:

- **Monitor runtime (Phase 5.5+)**: not shipped. The Pid handoff for
  a clean two-fiber `Monitor.monitor` demo needs `spawn_actor`
  (m8.x #6) or message types carrying Pid; v1's actor surface
  (`with_mailbox` only) supports neither cleanly. The runtime
  registry shape mirrors Link's; the work is small once the demo
  path exists.
- **Trap-exit semantics**: collapsed to "any termination propagates
  to linked peers". BEAM-style Crashed-vs-Normal distinction
  (`process_flag(trap_exit, true)`) deferred.
- **Region-brand full machinery**: Phase 6 ships the existing
  shallow check (recursive walk through `TyName` / `TyList` /
  `TyFn` / `TyDim` / `TyRefine` looking for `Fiber`) with a
  generalised allow-list. The full `TyBranded(Ty, BrandId)` +
  brand-mint at handler-installation + propagation through every
  binding form is queued for post-Production-honest 1.0.
- **User-installed Cancel cleanup**: Phase 3's runtime-triggered
  cancel longjmps directly to the trampoline pad, skipping
  user-installed `with Cancel { raise(_) -> cleanup }` handlers.
  Direct user calls to `Cancel.raise()` still go through normal
  op dispatch, so user handlers fire there. Wiring runtime-cancel
  into the user-handler path requires more careful interaction
  with m7a #6e's discard-resume machinery.

Lane retro: see PR #8 body and `docs/fibers-impl.md`.

## [0.3.0] — 2026-04-28 (m14 v1 — stdlib qualified-call surface)

**Minor: new user-visible API surface.** `list.*`, `string.*`, `option.*`,
`result.*`, and `char.*` qualified calls are now first-class. Legacy
bare-name calls (`list_take`, `opt_map`, `ch_is_digit`, ...) keep working
via alias trampolines.

### Added

- **m14 v1 — stdlib qualified-call surface** (`ef6965e`, PR #6):
  - **v0**: `--prelude` files now register as `ModuleEntries` in the
    m6.2 module table, so qualified calls like `list.is_empty(xs)`
    resolve without an explicit `import`.
  - **v1**: per-module prefix-fallback in `me_lookup_export`
    (`option`→`opt`, `char`→`ch`, others default to module basename)
    backs the new surface against today's `pub fn list_take` /
    `pub fn opt_map` / `pub fn ch_is_digit` definitions.
  - **v1.A**: `stdlib/core/list.kai` definitions renamed to bare names
    (39 ops + internal `*_loop` helpers privatised + 29 legacy
    `pub fn list_X = X(...)` aliases for backward compat).
- New user-visible call sites (non-exhaustive):
  ```kai
  list.is_empty(xs)    list.take(xs, 3)    list.sort(xs)
  list.contains(xs, 7) list.flat_map(xs, f) list.head(xs)
  string.trim(s)       string.repeat("ab", 3)
  option.map(o, f)     option.is_some(o)
  result.is_ok(r)      result.and_then(r, k)
  char.is_digit(c)     char.to_lower(c)
  ```

### Fixed

- **C-backend codegen shadow bug** (m14 v1.x, part of `ef6965e`):
  `emit_ident_value` now threads `lcs: [String]` through every emit
  helper so locals correctly shadow same-named top-level fns. Mirrors
  the LLVM backend's `e.locals` design. The `decimal_basic` test was
  the canary: `let drop = d.scale - target` now works correctly with
  the prelude's `pub fn drop(xs, n)` defined.

### Deferred

- **`string` / `option` / `result` / `char` defs rename**: blocked on
  m6.2 v2 universal prefixed minting (`kai_<module>__<name>`).
  Cross-module bare-name collisions on `map` / `and_then` /
  `unwrap_or` / `repeat` mean two `pub fn map` defs would land on
  the same C symbol. The user-visible qualified surface is already
  complete via the prefix-fallback; the rename of internal definitions
  waits for m6.2 v2.
- `print` / `println` → `Console.print` consolidation — separate lane.
- Stage 1 codegen shadow fix — same bug exists in `stage1/compiler.kai`,
  not triggered by current stage 2 source.
- Stage 1 backport of `EModCall` — needed only when stage 2 source
  itself starts using qualified calls.

Lane experience: `docs/lane-experience-m14-v1.md`.

## [0.2.2] — 2026-04-28 (CLI polish — logo, dynamic version, RC trace fix)

### Added

- **ASCII logo** on `bin/kai` (no args) and `bin/kai help` (`54435b5`,
  PR #4): figlet "small" rendering of "kaikai" with tagline (version
  + "a functional language" + "effects | fibers | protocols") inline:
  ```
   _      _ _      _
  | |____(_) |____(_)    kaikai 0.2.2
  | / / _` | / / _` |    a functional language
  |_\_\__,_|_\_\__,_|    effects | fibers | protocols
  ```

### Changed

- `bin/kai --version` and `-V` now read the `VERSION` file dynamically
  (was hardcoded `"kai 0.2.0 (stage 2, Core language)"` and had drifted
  after the SemVer bump). Falls back to `unknown` if `VERSION` is
  missing.

### Fixed

- `bin/kai` no longer double-fires `KAI_TRACE_RC` reports. Prefix
  `KAI_TRACE_RC=` to the `"$KAIC2"` invocation in `compile_to_binary`
  so only the user's compiled program emits its RC trace, not kaic2
  itself. Closes the m5 #0 follow-up "Per-process double KAI_TRACE_RC
  report when run via bin/kai".

## [0.2.1] — 2026-04-28 (m5.x #3 LLVM emit mirror — backend parity)

### Added

- **m5.x #3 LLVM emit mirror** (`3018d2d`, PR #2): `--emit=llvm` now drops
  let-bindings whose RHS is a fresh allocation when last-use analysis
  shows the binding is unused, restoring backend parity for the m5 #3
  optimisation that the C-emitter has carried since pre-Core.
  - `llvm_emit_block` computes the per-block drop set with the same
    `block_unused_lets` walker the C-emitter uses; statements route
    through new `llvm_emit_stmts_with_drops` / `llvm_emit_stmt_with_drops`
    variants.
  - `PBind` in the drop set appends `call void @kaix_decref(...)` after
    the binding; `PWild` with a fresh-alloc RHS does the same.
  - New `kaix_decref` runtime wrapper in `stage0/runtime_llvm.c` exposes
    `kai_decref` to the LLVM backend.
  - `kaic2 --emit=llvm stage2/compiler.kai` produces 11 `kaix_decref`
    call sites — confirms the pass is active.

### Closed

- m5.x followup §4 ("LLVM emit mirror of m5 #3") — was PENDING, now LANDED.

## [0.2.0] — 2026-04-28 (R1 Phase 3 — runtime flip)

**Major: runtime semantics changed.** Phase 3 of the Perceus flip lands
the atomic A+B+C+D rewrite. The runtime now consumes args linearly;
producers incref on extraction; primitives decref after reading. This
is a behaviour change visible to any C-level extension or hand-written
runtime helper.

### Added

- **R1 Phase 3 — atomic Perceus runtime flip** (`fbb532c`, PR #1):
  - **Step A** — producer-side incref-on-extract. `emit_pat_binds`
    gains an `is_alias` flag in stage 0 + 1 + 2. True at variant args,
    cons head/tail, list-rest binding, and **top-level match-arm
    scrutinee** (subsequent arms re-read `_scr` and a guard inside an
    arm may consume the binding — const-pattern desugar emits
    `__cv_NAME == NAME()`). False for top-level let-destructure and
    record-field destructure (`kai_field` already increfs).
  - **Step B** — exit drops for multi-use / LUBlocked params.
    `pcs_prepend_unused_drops` wraps the body as
    `EBlock([entry, SLet(__pcs_ret, body), exit], EVar("__pcs_ret"))`.
    `DTest` now also routed through `perceus_decl` so test bodies get
    the dup walker (closures capturing multi-use locals inside test
    bodies were double-consuming captures across invocations).
  - **Step C** — 13 primitives flipped in `stage0/runtime.h`:
    `kai_add` / `sub` / `mul` / `div` / `idiv` / `mod` / `neg` / `lt`
    / `gt` / `le` / `ge` / `eq_v` / `ne_v` / `boolnot` decref args
    after reading every relevant field (self-aliasing-safe).
    `kai_truthy`, `kai_field`, `kai_apply`, `kai_eq` intentionally
    not flipped — see `stage0/runtime.h` comments.
  - **Step D** — stage 0 eager-dup retrofit. `emit_ident_value` wraps
    every local read in `kai_internal_dup(...)`. Brute force; stage 0
    has no perceus pass.
  - Hand-written runtime helpers (`kai_prelude_map` / `_filter` /
    `_reduce` / `_each`) updated to incref each arg before
    `kai_apply` so the consuming closure body has its own ref.

### Changed

- **Memory profile of `kaic2 self-compile**:
  - RSS: **6.25 GB → 3.02 GB (−52%)**.
  - live_peak: **−63%**.
  - alloc_total: 130.7 M (pre-m5) → 33.0 M (Phase 1 inert) → 69.7 M
    (Phase 3 — increfs and decrefs both counted).
- **Wall time of `kaic2 self-compile`**: **+2.7×** (≈2.15s → ≈5.8s).
  The dup/decref churn is real work; Phase 3 trades wall time for
  RSS. Mitigations (reuse-in-place, drop specialisation) are
  follow-up lanes.
- Tier 1 #2 ("runtime-efficient") no longer carries the asterisk
  about leaks. **However** the wall-time regression introduces a new
  asterisk that the m5.x-2 follow-up will address.

### Notes

- Phase 4 (further stage 0 optimisations) is folded into the m5.x-2
  follow-up rather than tracked separately.
- The byte-identical selfhost fixed point is preserved across the
  flip — both backends round-trip without diff.

## [0.1.3] — 2026-04-28 (m12.6.x #2 sub-steps + Interval lattice)

### Added

- m12.6.x #2 sub-step 1: Interval lattice (`0a6e0f2`) — foundation
  for refinement-type interval propagation in the typer.
- m12.6.x #2 sub-steps 2+3+4: interval pass + dump flag + fixture
  (`b6bd5f6`).
- m12.6x refine-prop design doc (`7c1c836`) — lane A scope.

## [0.1.2] — 2026-04-28 (R1 Phase 1)

### Added

- Perceus Phase 1 (m5.x-flip step 2a, `e824222`): `pcs_is_non_last`
  switched from "lexically-last → raw transfer" to "any binding with ≥ 2
  non-lam uses → dup every read". Single-use bindings still transfer
  raw. The previous rule was unsafe under C's unspecified argument
  evaluation order (clang on AArch64 evaluates argument lists
  right-to-left); the all-dup variant is order-independent.

### Notes

- Phase 1 was inert under the loose runtime — extra `__perceus_dup`
  calls bumped rc with no matching decref. Phase 1 paid its way in
  v0.2.0 once Phase 3 made every primitive consume its args.

## [0.1.1] — 2026-04-28 (m12.6.x close + regex + §29 const + stdlib Combo A)

### Added

- `stdlib/regexp.kai` (`608e694..3909c71`): RE2-style NFA regex engine,
  8 phases: design + parser (`RxAst` + recursive descent) + Thompson-style
  NFA construction + NFA simulator (set-of-states, leftmost-longest)
  + public API (`compile` / `match` / `find_all` / `replace` / `split`)
  + `regex_basic` fixture + `bin/kai` + Makefile wiring.
- `stdlib/collections/{set,queue,stack}.kai` and `stdlib/math/int.kai`
  (Combo A, `28c1b13`).
- §29 const declarations: top-level `const NAME : T = literal`
  (`75c6856`), refs inside `#{...}` interpolation, pattern-side `const`
  in match arms (`d33db66`).
- `[<refinement_pure>]` attribute (m12.6.x #5(a), `390311b`).
- Inline pure attr in match-arm narrow + alias resolution (m12.6.x #3
  v2 + #5 inline, `86dc465`).
- `impl Show for Char` + cleaner regex AST dump (`98c663f`).

### Fixed

- Unannotated `main` SIGSEGV when effects reach via helpers (`fb5bc4c`):
  post-typing walker `collect_call_labels` extends `inferred_main_row`
  with effects reached transitively via callees; the runtime now installs
  default handlers for those effects.

### Closed

- m12.6.x — refinements + contracts followup. All eight items closed:
  parser (a-e), match-arm narrow, refinement_pure attribute,
  result-binding, const-folding, `ensure(v) where pred`, **#7 regex
  literals (unblocked by the regex stdlib lane)**.

## [0.1.0] — 2026-04-27 (Core language feature-complete)

Core language closed under the revised 3-level gate (`docs/stage2-design.md`
§"Update 2026-04-27 (post-REOPEN final)").

### Gate met

- **Level 1 (mechanical)**: `make selfhost` + `make -C stage2 selfhost-llvm`
  byte-identical fixed point; `make test` clean (incl. `test-demos-core`
  and `test-aspirational`).
- **Level 2 (audit + invariant verifier)**: every `_ -> k` wildcard in
  every AST walker pass justified or explicit (`7bd5a68`); rep invariant
  (b) — protocol-dispatcher references outside enclosing binders — runs
  on every compile via `validate_typer_invariants` (`0afc2a9`).
- **Level 3 (demo gate)**: `make demos-no-regression` baseline 18 OK
  + PASS demos in `demos/`; `make -C stage2 test-aspirational` runs
  `examples/aspirational/event_logger` (custom Logger effect + handler
  + effectful HOF callbacks + sum-type-with-payload `#derive(Show)`).

### Added (across the build-up to Core)

- Stages 0 / 1 / 2 (m1-m6), all with `selfhost-llvm` fixed-point green.
- Effects + handlers + parametric effects + per-instance dispatch
  (m7a / m7b / m7c).
- Ergonomic sugars: trailing lambdas, `@cap`, `var`, `a[i]`, aliases,
  `todo!`, record punning, `@` as-patterns, pipeline `_`, `++`,
  `!` postfix on `Option` / `Result` (m7b / m7d / m7e §13).
- Fibers, structured concurrency, actors with supervision (m8 v1 type
  surface; runtime is inline-eager — real scheduler is m8.x).
- Typed holes (m10).
- Perceus basic memory management (m5 rounds 1-3) + capture incref
  (m5.x #2). Linear-consumption runtime closed in v0.2.0.
- Units of measure (m12.5).
- Single-dispatch protocols + 5 stdlib protocols + `#derive(Show, Eq,
  Hash, Ord)` for records and sum types (m12.8 + m12.8.x + m12.8.z).
- Atomic per-stream effects (`Stdout` / `Stderr` / `Stdin`) with
  `Console` and `Io` aliases (m12.8 Phase 4b).
- Parametric `impl[u: Unit] Show for Real<u>` (m12.8 Phase 2).
- m12 self-host checkpoint: byte-identical fixed-point of
  `kaic2 stage2/compiler.kai`.
- Both backends (`--emit=c`, `--emit=llvm`) at parity for every shipped
  feature.

### Notes on remaining structural debt

The runtime still carries two of the three structural deficits audited
on 2026-04-28 (Linus + Eric). After v0.2.0 the third one (RC fictional)
is closed:

1. ~~RC fictional at runtime.~~ **Closed in v0.2.0 by R1 Phase 3.**
2. **Fibers do not suspend.** m8 v1 is inline-eager — `Spawn.spawn`
   runs the thunk synchronously, `await` is identity, `select` cannot
   interleave, `Cancel` cannot be delivered (no yield points). The
   BEAM-style structured-concurrency claim is type-check-only.
   Target of the **R2** lane (m8.x real fiber scheduler).
3. **m4c monomorphisation is an identity pass.** The
   `try_rewrite_show_dim_real` shortcut in m12.8 Phase 2 confirms it;
   polymorphics with `EHandle` in the body collide on `clause_fn_name`.

[Unreleased]: https://github.com/lnds/kaikai/compare/v0.8.0...HEAD
[0.8.0]: https://github.com/lnds/kaikai/compare/v0.7.2...v0.8.0
[0.7.2]: https://github.com/lnds/kaikai/compare/v0.7.1...v0.7.2
[0.7.1]: https://github.com/lnds/kaikai/compare/v0.7.0...v0.7.1
[0.7.0]: https://github.com/lnds/kaikai/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/lnds/kaikai/compare/v0.5.1...v0.6.0
[0.5.1]: https://github.com/lnds/kaikai/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/lnds/kaikai/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/lnds/kaikai/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/lnds/kaikai/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/lnds/kaikai/compare/v0.2.2...v0.3.0
[0.2.2]: https://github.com/lnds/kaikai/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/lnds/kaikai/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/lnds/kaikai/compare/v0.1.3...v0.2.0
[0.1.3]: https://github.com/lnds/kaikai/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/lnds/kaikai/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/lnds/kaikai/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/lnds/kaikai/releases/tag/v0.1.0
