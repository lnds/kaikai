# Changelog

All notable changes to kaikai are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html);
prior to 1.0.0 minor versions may break backwards compatibility (see CLAUDE.md
"Backward compatibility — not promised until post-MVP").

## [Unreleased]

### Added

- Versioning infrastructure: `VERSION` file at repo root, this `CHANGELOG.md`,
  retroactive git tags `v0.1.0` / `v0.1.1` / `v0.1.2` / `v0.1.3` / `v0.2.0`
  / `v0.2.1` / `v0.2.2` / `v0.3.0` / `v0.4.0` and the current `v0.4.1`. The
  compiler's own `kaic2 --version` flag still reports the legacy `kaic2
  stage 2 (self-hosted)` string; the `bin/kai --version` wrapper reads
  VERSION dynamically.

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

[Unreleased]: https://github.com/lnds/kaikai/compare/v0.4.1...HEAD
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
