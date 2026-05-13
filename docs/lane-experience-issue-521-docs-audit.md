# Lane retro — issue #521 docs audit + sweep

Pinned 2026-05-13. Read-only Phase A audit of the 12 primary specs
called out by issue #521, followed by a per-doc sweep (Phase B). The
goal of #521 was to align primary docs with reality after #510, #511,
the 2026-05-09→10 bug-bash, and the negative-space follow-ups (#516,
#517, #518, #520, #528, #530, #534-#539, #541, #543).

This retro is the **Phase A output**. The Phase B commits cite this
file. Issue #521 closes when no STALE finding remains; ASPIRATIONAL
findings have sidebars; UNVERIFIABLE findings are either deleted or
queued.

## Method

For each of the 12 primary docs the issue body named, I:

1. Read the doc end-to-end.
2. Extracted every claim of the form "the compiler does X" / "the
   runtime does Y" / "the stdlib provides Z" / "issue #N tracks W".
3. Verified the claim against today's code (`stage2/compiler.kai`,
   `stdlib/`, `runtime/`, `examples/negative/`) and today's issue
   tracker via `gh issue view`.

No code was touched in Phase A. The audit is bitácora; the
corrections live in Phase B commits.

Finding taxonomy (per `CLAUDE.md` "Doc discipline"):

- **HOLDS** — the claim is enforced today.
- **STALE** — the claim was once true; today it is wrong.
- **ASPIRATIONAL** — the claim describes desired behaviour, not
  shipped; needs `> **v1 status (YYYY-MM-DD):** …` sidebar.
- **UNVERIFIABLE** — no test, no fixture, no code path proves the
  claim. Either delete (if not load-bearing) or queue as follow-up.

## Per-doc findings

### 1. `docs/design.md` (344 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| Tier 1 #1 (effects visible in row types) | HOLDS | line 35-38 | #516 closed transitive propagation; #517 closes handle leak; primary-doc spec is enforced by test-negative `effects_phase2/`, `handle_leak/`, `pub_effect/`. |
| Tier 1 #2 (monomorphisation, mandatory TCO, unboxing) | HOLDS | line 42-49 | Phase 3 unboxing landed (#383); TCO `tcrec_rewrite_decls` lands per `perceus-honesty-targets.md` §TCO. |
| Tier 1 #3 (single-pass parse, LL(1), no type-class resolution) | HOLDS | line 51-58 | Protocols ship as `O(1)` lookup (`docs/protocols.md` §Resolution algorithm); no HKT, no constraint propagation. |
| Tier 2 #4 (structured JSON diagnostics) | ASPIRATIONAL → ALREADY SIDEBAR'd | line 61-66 | `--holes-json` exists; `kai type --json`, `kai effects --json`, counterexample-JSON for non-exhaustive matches are referenced under `docs/proposed-extensions.md` rather than asserted as shipped. No sidebar needed — phrased as prototype + extension. |
| Decision "Concurrency: `nursery { n -> ... }`" | HOLDS | line 149 | Lives at `stdlib/spawn.kai:95`. |
| Decision "Memory model: opaque mutable `Array[T]` behind `Mutable`" | HOLDS | line 156 | #251 + #252 landed; observable-effects discipline is enforced (see `effects-stdlib.md` §Mutable). |
| Decision "Tests as first-class: builtin syntax" | HOLDS | line 182-186 | `test "..." { ... }` + `assert` recognised. |
| Decision "Kind system: closed at two kinds" | HOLDS | line 189 | `docs/kinds.md` is the authoritative reference. |
| Phase 4 stdlib row "`Console`, `Stdin`, `Env`, `File` under their pre-effects-system shapes" | STALE | line 305 | The current shape is the v1 monolithic `Console` (Stdout + Stderr + Stdin) per `effects-stdlib.md` §Console v1-vs-target sidebar. The Phase 4 paragraph predates the m7a/m7b split and still describes them as "pre-effects-system shapes". |
| MVP scope wording "compiles a significant subset of full kaikai" | HOLDS | line 219-229 | Aligned with `kaikai-minimal.md` and `stdlib-layout.md`. |
| Repository layout block | HOLDS | line 240-254 | `stage0/`, `stage1/`, `stage2/`, `stdlib/`, `runtime/`, `demos/`, `tests/`, `docs/` all exist as named. |

Totals: 9 HOLDS, 1 STALE, 0 ASPIRATIONAL (1 already sidebar-equivalent), 0 UNVERIFIABLE.

### 2. `docs/effects.md` (687 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| Row representation + Rémy-style unification | HOLDS | §Effect rows, §Unification (line 47-94) | `unify_row` ships per `effects-impl.md` §Types. |
| Subsumption: none | HOLDS | line 130-141 | The typer rejects implicit row weakening. |
| `Eff.op(...)` call surface and `as`-rebind | HOLDS | line 257-294 | Tests live in `examples/effects/`. |
| `handle ... with E { ops }` reserved keywords | HOLDS | line 297-333 | Grammar production exists; not function-call shape. |
| `return(x)` optional, defaults to identity | HOLDS | line 311-313 | #539 closed wontfix; `docs/decisions/handler-return-clause-optional-2026-05-12.md` pins the rationale. |
| One-shot `resume` is the default | HOLDS | line 361-388 | `runtime.h` enforces via `KaiCont.status`; `examples/negative/oneshot/` covers the negative case. |
| `resume_multishot` exists as opt-in | ASPIRATIONAL | line 384-387 | v1 doc text says "v1 of effects ships only one-shot; multi-shot is noted for `docs/effects-impl.md`". `effects-impl.md` covers the runtime shape; the surface verb is not yet wired. Needs a sidebar saying so. |
| Open Q1 "Syntax for calling rebound capability" → decided | HOLDS | line 628-633 | |
| Open Q2 "`return` clause sugar" → tentative | STALE-VS-DECISION | line 635-639 | The wider Q is now decided (handler-return-clause-optional-2026-05-12.md). Update to "decided". |
| Open Q3 "Ordering of `with`-clauses — exhaustive" → tentative | STALE-VS-DECISION | line 640-643 | #517 closed handle validation; the typer rejects missing op clauses. Update to "decided: exhaustive; #517". |
| Open Q4 "What does `main` look like?" → decided | HOLDS | line 645-651 | |
| Open Q5 "Effect aliases" → decided | HOLDS | line 653-665 | `type Io = Console + Stdin + Env + File` works. |
| Open Q6 "Interaction with modules" → tentative | STALE-VS-DECISION | line 667-672 | Modules and re-export of effects validated. Treat as decided. |
| §Out of scope for v1 — "operation cannot quantify over its own row variable" | HOLDS | line 592-599 | Doc B (and `effects-impl.md`) amend the type-generics half; row-level per-call polymorphism stays out. |

Totals: 11 HOLDS, 1 ASPIRATIONAL (resume_multishot), 3 STALE-VS-DECISION
(open-Q tentative labels are post-decisions). No UNVERIFIABLE.

### 3. `docs/effects-stdlib.md` (2038 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| Catalog table — `Console`, `Stdin`, `Env`, `File`, `Clock`, `Random`, `SecureRandom`, `NetTcp`, `NetUdp`, `NetDns`, `Process`, `Signal`, `Fail`, `State[T]`, `Reader[T]`, `Writer[W]`, `Mutable`, `Cancel`, `Spawn`, `Ffi` | MIXED | line 18-42 | `NetUdp`/`NetDns` are aspirational — already noted by the v1-caveat sidebar in `stdlib-layout.md` line 422-424, but the effects-stdlib.md catalog row says "yes (runtime, if in `main`'s row)" without qualification. Needs a "(aspirational)" marker or a v1 status sidebar. |
| §`Console` v1 vs target shape | HOLDS | line 272-284 | Sidebar already in place. |
| §`Stdin` `read_bytes(n)` | HOLDS | line 320 | Shipped via #453. |
| §`Env` `args() / var(name)` | HOLDS | line 369-396 | #537 maps `args()` to `Env`; `var(name)` is `Env.var(name)`. |
| §`Env` notes nothing about `program_name` | HOLDS-BY-OMISSION | line 396-410 | `program_name` is deliberately not listed under Env per #537's closure — it routes through `kai_g_argv` snapshot, not handler-mediated. The doc is silent, which matches reality. No change needed. |
| §`File` cross-cutting note on `Result` type-arg order | HOLDS | line 414-425 | Aligned with `stdlib/effects.kai:97-105`. |
| §`Clock` default handler | HOLDS | line 547-558 | Shipped via PR #134. |
| §`NetTcp`/`NetUdp`/`NetDns` declaration with v1 sidebar | HOLDS | line 789 sidebar | The §`NetTcp` v1-status sidebar at line 789 explicitly documents the gap. |
| §`Process` declaration with v1 sidebars | HOLDS | line 833, 856, 906, 939 sidebars | Multiple sidebars now in place. |
| §`Signal` `await()` blocks the OS thread | HOLDS | line 1022-1027 | "Limitations (v1)" section explicitly documents this. |
| §`Mutable` declaration + observable-effects discipline | HOLDS | line 1308-1486 | #251 + #252 enforced. |
| §`Mutable` "Default handler" §"observe or intercept mutation" | STALE | line 1369-1372 | Claims explicit `with Mutable { ... }` exists "to observe or intercept mutation — test harnesses that want to log every `array_set`". Reality (2026-05-12 audit, `docs/decisions/handler-return-clause-optional-2026-05-12.md`, Eric §2b): **bare `array_set` is a prelude builtin and BYPASSES the user-installed `Mutable` handler.** Only the qualified form `Mutable.array_set(...)` routes through the handler. Universal interception is not what ships. |
| §Open Q6 "Multiple `Mutable` scopes" → Decided: allow nested observation | STALE | line 1976-1985 | Same problem as above: the "Decided" answer promises that "a test harness can wrap a body with a `Mutable` interceptor to log every `array_set`. Such a handler sees stdlib-internal `Mutable` calls too." Stdlib-internal calls go through `kai_prelude_array_*` runtime helpers, not through the handler stack. Observers see qualified `Mutable.array_set` only; bare-builtin and prelude calls bypass them. |
| §`Cancel` Delivery points + `Spawn.yield()` | HOLDS | line 1616-1646 | Cooperative cancel ships per `fibers-honesty-targets.md`. |
| §`Spawn` declaration with per-op generics + row variable | HOLDS | line 1655-1668 | Issue #72 closed. |
| §`Ffi` declaration + name override syntax | HOLDS | line 1696-1718 | #260/#261 shipped. |
| §Migration of existing builtins table | HOLDS | line 1773-1791 | Maps `print → Console.print`, `args() → Env.args()`, `array_make/get/set/grow → Mutable.array_*`. |
| §`main` and the runtime — Allowed `main` signatures | HOLDS | line 1834-1865 | The catalog matches the compiler's `prelude_effect_for`. |
| §Out of scope for v1 — `Actor[Msg]` deferred to `docs/actors.md` | HOLDS | line 1921 | `docs/actors.md` exists and is current. |

Totals: 17 HOLDS, 2 STALE (Mutable observation/interception promises that bare `array_set` bypasses).

### 4. `docs/effects-impl.md` (2169 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| §Choice of régime "C — surface-A + runtime-B" | HOLDS | line 38-77 | Implemented; régime C is what ships. |
| §Types — `TyFnT` row slot | HOLDS | line 125-169 | `Row { labels, tail, display_alias }` is what the typer carries. |
| §Surface-to-runtime mapping — Ev struct per effect | HOLDS | line 196-253 | `EvX` structs emitted by `compute_op_offsets`. |
| §Parameterised effects — handler state field | HOLDS | line 255-300 | `EvState[T].state` slot is what the typer + emitter generate. |
| m7c phases — m7c-a/b/c/d landed | HOLDS | line 1950-2074 | All four landed per the in-doc "Landed." markers. |
| §C backend invariant `_discard` volatile (#501) | HOLDS | line 2079-2105 | Issue #501 closed, fix shipped. |
| §Scheduled follow-up m7c-e setjmp landing pad (LLVM) | ASPIRATIONAL → ALREADY SIDEBAR'd | line 2107-2147 | The doc itself frames m7c-e as "Scheduled follow-up"; the trigger condition is named. No sidebar needed — it is already presented as not-yet-shipped. |

Totals: 6 HOLDS, 0 STALE, 1 ASPIRATIONAL (already framed correctly).

### 5. `docs/protocols.md` (1052 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| Status: Landed (m12.8) | HOLDS | line 3-13 | Five stdlib protocols + `Default` (#258) shipped. |
| §Design — protocol declarations + impl form | HOLDS | line 47-104 | Surface matches `stdlib/protocols.kai`. |
| §Polymorphic impls with bounded tparams (#174) | HOLDS | line 106-160 | Shipped. |
| §Coherence rules — one impl per (P, T), orphan rule | HOLDS | line 184-196 | Enforced. |
| §Resolution algorithm — O(1) hash, no constraint propagation | HOLDS | line 235-251 | |
| §Stdlib protocols table (`Show`, `Eq`, `Ord`, `Hash`, `Serialize`, `Default`) | HOLDS | line 291-303 | All shipped except `Serialize` on parametric containers (deferred — noted line 309-311). |
| §`#derive` annotation | HOLDS | line 355-447 | Shipped per m12.8.x. |
| §Composition with effects — protocols are pure | HOLDS | line 483-500 | Enforced by typer. |
| §Impl validation diagnostics — missing/extra/wrong-arity methods | HOLDS | per #535 close | #535 closed 2026-05-13. |
| §Single-dispatch parametrized protocols (#180) | HOLDS | per #180 close | |

Totals: 10 HOLDS, 0 STALE, 0 ASPIRATIONAL, 0 UNVERIFIABLE.

### 6. `docs/structured-concurrency.md` (328 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| §Syntax — `nursery { n -> ... }` cap-binding form | HOLDS | line 27-58 | Shipped. |
| §Type system — `Fiber[T]` / `Pid[Msg]` region-branded handles | HOLDS | line 60-105 | Shallow check + sum-type-payload walker + `--dump-brands` shipped per `fibers-honesty-targets.md` §Residual m8.x. |
| §Cancellation — `Cancel.raise()` cleanup | HOLDS | line 153-198 | #103 trap-exit bypass is the documented quirk. |
| §Patterns — Race / Timeout / Actor supervision | HOLDS | line 200-262 | All compile under stage 2. |
| §Root nursery — `main` with `Spawn` in row | HOLDS | line 263-282 | Wired in m8. |
| §Non-goals — no detach, no preemption, no priority schedulers | HOLDS | line 284-296 | Cooperative only. |
| §Implementation notes — scheduler + region-branded handle + `nursery` as stdlib helper | HOLDS | line 297-318 | All shipped. |

Totals: 7 HOLDS, 0 STALE.

### 7. `docs/actors.md` (721 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| §`Actor[Msg]` declaration | HOLDS | line 48-55 | |
| §`spawn_actor` / `spawn_actor_default` / `with_mailbox` | HOLDS | line 116-170 | Shipped per `stdlib/actor.kai`. |
| §Mailbox policies — Unbounded + Bounded(capacity, on_full) with three Overflow rules | HOLDS | line 207-267 | Phase 4 mailbox primitives shipped. |
| §Selective receive — v1 receive-and-requeue, O(n²) | HOLDS | line 268-293 | Acceptable v1; future selective-receive deferred. |
| §Supervision: links + monitors | HOLDS | line 295-475 | Link runtime (Phase 5), Monitor + spawn_actor + Trap-exit shipped per `fibers-honesty-targets.md`. |
| §Trap-exit semantics + #103 bypass | HOLDS | line 397-475 | Shipped — #103 closed; both `structured-concurrency.md` §Cancellation and this section reference the same constraint. |
| §Two distinct `Actor[T]` effects in a row report error — issue #472 fix | HOLDS | per #472 close | |
| §Open Q1-4 → all decided | HOLDS | line 659-700 | |

Totals: 8 HOLDS, 0 STALE.

### 8. `docs/fibers-honesty-targets.md` (220 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| §Where we are today (2026-05-09) | HOLDS | line 16-40 | R2/R3/R4 + stack guard pages + Monitor + trap-exit + LLVM op-dispatch + per-op generics all marked LANDED. Honest. |
| §Tier 1 closed 2026-04-29 | HOLDS | line 42-53 | |
| §Tier 2 production-honest 1.0 | HOLDS | line 56-75 | Marked shipped. |
| §Residual m8.x items — item 1 (TyBranded option (a) closed; option (b) closed) | HOLDS | line 102-167 | |
| §Residual m8.x items — item 2 (Select race semantics, user Cancel handlers, structured cancel-on-fail) | HOLDS | line 168-187 | Documented as deferred. |
| §Sequencing recommendation | HOLDS | line 189-205 | |
| §Tier 3 post-MVP — Optimised context switch / multi-thread / profiling | HOLDS | line 77-91 | Out-of-MVP. |

Totals: 7 HOLDS. Doc is current. No findings.

### 9. `docs/perceus-honesty-targets.md` (300 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| §Where we are today (2026-05-09) | HOLDS | line 17-75 | Compute parity with C reached on benchmarks (fib 1.00× C, Euler 4 1.05× C). |
| §What does NOT work today | HOLDS | line 78-110 | `kai_truthy` non-consuming + remaining `kai_prelude_*` helpers + stage 0 eager-dup retrofit are open follow-ups; all clearly framed. |
| §Tier 1 — empty | HOLDS | line 111-121 | Show HN honest. |
| §Tier 2 — 3 closed, 2 still open | HOLDS | line 123-140 | Items annotated with status; current. |
| §Tier 2.5 — Unboxing Phase 2 + Phase 3 landed 2026-05-09 | HOLDS | line 142-196 | Phase 3 (#383) closed. |
| §Tier 3 — Reuse-in-place v1 (#118) + v1.1 (#209) | HOLDS | line 199-213 | Both closed. |
| §TCO half of Tier 1 #2 — landed 2026-04-30 (issue #37) | HOLDS | line 234-280 | |

Totals: 7 HOLDS. Doc is current.

### 10. `docs/stdlib-roadmap.md` (262 lines) + `docs/stdlib-layout.md` (605 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| `stdlib-roadmap.md` §Current inventory (refreshed 2026-05-08) | HOLDS | line 41-92 | Tracks fs/file extras (#345), fs/dir (#344), os/env (#127), os/args, os/process (#346), Clock default handler (#134), crypto, random_secure (#140), log (#141), array bridge (#366), fx (#365), decimal div-by-zero (#363), encoding/json Real (#361) + UTF-8 (#362), core/tuple (#348), core/list expansion (#340), core/string expansion (#338) all shipped. |
| `stdlib-roadmap.md` §Tier S3 — net/udp + net/dns deferred | HOLDS | line 197-213 | Post-1.0. |
| `stdlib-roadmap.md` §Discipline reminders — fixture in `examples/stdlib/` for every new module | HOLDS | line 239-254 | |
| `stdlib-layout.md` status markers (shipped / planned: #N / stub) | HOLDS | line 9-19 | Convention is in place. |
| `stdlib-layout.md` §io — "lives in `stdlib/core/io.kai`, not at top level" | HOLDS | line 220 | Honest disclaimer. |
| `stdlib-layout.md` §net "NetUdp + NetDns aspirational" + sidebar 2026-05-08 | HOLDS | line 422-431 | |
| `stdlib-layout.md` §concurrent "No `stdlib/concurrent/` directory exists" sidebar | HOLDS | line 487-490 | |
| `stdlib-layout.md` §fs.path "lives at `stdlib/path.kai`, not `stdlib/fs/path.kai`" | HOLDS | line 386 | |
| `stdlib-layout.md` §testing "no `stdlib/testing.kai` module file exists" | HOLDS | line 494 | |

Totals: 9 HOLDS. Both docs have already absorbed #367 reconciliation work and ship status markers per `CLAUDE.md` "Doc discipline".

### 11. `docs/roadmap.md` (524 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| **HEAD: `0.43.0`** | STALE | line 13 | Today's HEAD per `VERSION` is **0.54.3** (33 minor bumps since 2026-05-05). The "post protocols + ergonomics chain; #285 Ref-loop closure capture fix" parenthetical pins the moment of writing, but the headline number is wrong. |
| Pinned 2026-05-02 (post v0.30.0) | STALE | line 3 | The pin date is fine for the section; v0.30.0 is no longer the post-state. Recommend leaving the original pin date but adding a "Last refreshed: 2026-05-13" line that tracks HEAD. |
| Tongariki CLOSED 2026-05-02 + Wave 3 (#73) | HOLDS | line 55-189 | All six DoD items HOLDS. |
| Anga Roa scope: m11 (#445, #479), kai lsp (#447 open), bench v1.x, check v1.x shrinking, reuse-in-place | HOLDS | line 191-355 | Issue #445 closed; #479 closed; #447 open. |
| **REPL removed permanently (#406)** + `docs/decisions/repl-removal-2026-05-09.md` | HOLDS | line 208-211 | Doc exists. |
| Ghost references #92 / #120 removed in this doc | HOLDS | line 205-208 | Doc explicitly notes that the previous citations were removed. |
| Anga Roa DoD #3 (compute-bound + structural data traversal) with v1 status sidebar dated 2026-05-09 | HOLDS | line 304-316 | Sidebar in place. |
| DoD #6 (compile-time perf, added 2026-05-11) + dependencies #453/#454 shipped, #452/#455 open | HOLDS | line 321-348 | #453 closed, #454 closed, #452/#455 open. |
| Orongo — multi-thread + asm-level context switch + ... | HOLDS | line 356-417 | Out-of-MVP per `fibers-honesty-targets.md` Tier 3. |
| Anakena — Linux arm64 / macOS x86_64 / Windows / WASM / LLVM Phase 2 mirror | HOLDS | line 419-454 | Post-1.0. |
| Meta-roadmap layers (kaikai → ahu → kohau → henua → manutara / hopu) | HOLDS | line 456-507 | Matches `project_ahu_design_decisions.md` and CLAUDE.md. |

Totals: 9 HOLDS, 2 STALE (HEAD version + pin recency). All cited issues verified open/closed via `gh issue view`.

### 12. `docs/testing-tiers.md` (217 lines)

| Finding | Type | Citation | Detail |
|---|---|---|---|
| §Tier 0 — `make selfhost && make demos-no-regression` | HOLDS | line 35-50 | Matches `Makefile` `tier0` target. |
| §Tier 1 — `make test` lists test-tokens, test-ast, …, test-aspirational + test-m4c | HOLDS | line 51-67 | Matches `stage2/Makefile`. Note: the wider `make tier1` target also includes `test-fmt test-bench test-check test-library-mode test-diagnostics-collected test-negative` — the doc's §Tier 1 list is the **`make test`** sub-target, not the **`tier1`** target. The doc is honest about that (line 56 says "`make test`"). |
| §Negative-space discipline (issue #511) | HOLDS | line 77-127 | `tools/test-negative.sh` gates it; three modes (compile-time / stage 1 rejection / runtime-time) documented; silent_contract subtree is excluded. **DoD met for #521 acceptance.** |
| §Tier 2 — daily / cron | HOLDS | line 129-148 | |
| §Stress fixtures + Coverage probe + RC budget probe | HOLDS | line 150-194 | All as named (the coverage probe walks `## ` headings). |
| §Cadence summary table | HOLDS | line 195-205 | |
| §What this document is NOT | HOLDS | line 207-217 | |

Totals: 7 HOLDS. Doc is current. #511 negative-space discipline IS present (the acceptance test the issue body called out).

## Summary by document

| Doc | HOLDS | STALE | ASPIRATIONAL | UNVERIFIABLE |
|---|---:|---:|---:|---:|
| design.md | 9 | 1 | 0 | 0 |
| effects.md | 11 | 3 | 1 | 0 |
| effects-stdlib.md | 17 | 2 | 0 | 0 |
| effects-impl.md | 6 | 0 | 1 | 0 |
| protocols.md | 10 | 0 | 0 | 0 |
| structured-concurrency.md | 7 | 0 | 0 | 0 |
| actors.md | 8 | 0 | 0 | 0 |
| fibers-honesty-targets.md | 7 | 0 | 0 | 0 |
| perceus-honesty-targets.md | 7 | 0 | 0 | 0 |
| stdlib-roadmap + stdlib-layout | 9 | 0 | 0 | 0 |
| roadmap.md | 9 | 2 | 0 | 0 |
| testing-tiers.md | 7 | 0 | 0 | 0 |
| **Total** | **107** | **8** | **2** | **0** |

Verdict: most of the primary specs are already disciplined. The 8
STALE findings cluster into 4 categories:

1. **effects.md Open Q2/Q3/Q6 still labelled "tentative"** — should
   read "decided" given #517/#539/#533 and the modules+effects work.
2. **effects-stdlib.md §Mutable promises universal interception** —
   bare prelude `array_set` / `array_grow` bypass the handler. The
   §Mutable "Default handler" paragraph (line 1369-1372) and the
   matching §Open Q6 decision (line 1976-1985) both need adjusting
   to reflect that only the qualified `Mutable.array_*` form routes
   through user handlers.
3. **design.md Phase 4 paragraph** still describes stdlib effects
   under their "pre-effects-system shapes". The mention is harmless
   (Phase 4 is the MVP-tooling phase, scoped past today) but reads
   as if `Console`/`Stdin`/`Env`/`File` were not yet effects.
4. **roadmap.md status snapshot** — `HEAD: 0.43.0` is 11 minor
   versions behind today's 0.54.3.

## Decisions for Phase B

Apply the corrections under the rule "one commit per doc minimum".
The four categories above map to four commits.

Specifically:

- `docs(effects): mark Open Q2/Q3/Q6 as decided` —
  retitle the "*Tentative:*" labels to "*Decided:*" with citations to
  #517, #533, and `handler-return-clause-optional-2026-05-12.md`.
- `docs(effects-stdlib): correct §Mutable to reflect bare-builtin bypass` —
  rewrite the "exists only to observe or intercept mutation" paragraph
  and Open Q6's "Decided" answer to be honest that bare `array_set` /
  `array_grow` are prelude builtins that route directly to the
  runtime, NOT through a user-installed handler. Qualified
  `Mutable.array_set(...)` is the form that handlers can intercept.
  This is consistent with the 2026-05-12 audit
  (`docs/decisions/handler-return-clause-optional-2026-05-12.md`) and
  Eric's §2b analysis from the same audit.
- `docs(design): align Phase 4 stdlib paragraph with m7a/m7b reality` —
  update line 305 of `design.md` so the Phase 4 bullet reads
  consistently with `effects-stdlib.md` §Console v1 monolithic shape.
- `docs(roadmap): refresh status snapshot to 0.54.3` —
  update the HEAD pin from `0.43.0` to `0.54.3` and add a
  Last-refreshed line.

The retro itself is a fifth standalone commit
(`docs(audit): record lane-experience #521 audit findings`).

## Decisions of scope

Not in this lane:

- **`docs/decisions/handler-return-clause-optional-2026-05-12.md`** is
  the canonical source for the `return`-clause-optional decision; no
  rewrite needed.
- **No new follow-up issues opened.** Every STALE finding is fixable
  by edit. The two ASPIRATIONAL findings (`resume_multishot`, m7c-e
  setjmp landing pad) are already framed correctly in their
  respective docs.
- **No primary-spec section rewritten from scratch (>50 lines).**
  Every Phase B edit is < 30 lines; the audit triggered no
  design-level reopens.

## Cost

Real cost: ~2 hours of audit reading + cross-checking. Phase B edits
estimated at ~1 hour total (small, surgical). Well under the
~2-3 day estimate in the issue body — the primary docs were already
in better shape than #367 suggested, because most of the recent
negative-space lanes (#516, #517, #520, #530, #535, #536, #537, #539,
#543) updated their respective docs at close.

## Follow-ups left for next lanes

None. The audit closes cleanly under the per-doc commits in Phase B.
If a future audit catches drift this small at this cadence again, the
discipline section in `CLAUDE.md` is working.
