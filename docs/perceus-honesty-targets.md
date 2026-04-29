# Perceus — honesty targets

Decision pinned 2026-04-29 (post R1 Phase 3 atomic flip landed v0.2.0;
match-scrutinee plug v0.9.2 + R3 amend; per-fiber in_dispatch fix
2026-04-29). Three tiers of "Perceus done", what each one means
concretely, and which followups belong in which tier. Cost numbers
are real-day estimates with the runtime-engineering velocity
calibration (~5–10× faster than spec).

This is a **scope decision**, not a roadmap. The roadmap proper lives
in `docs/m5x-followup.md`; this file says *which followups are
required for which honesty claim*.

Sister doc: `docs/fibers-honesty-targets.md` (same shape, applied to
the m8.x scheduler).

## Where we are today (2026-04-29)

R1 atomic flip landed v0.2.0 (2026-04-28):

- 13 primitives in `stage0/runtime.h` consume their args linearly
  (`add`, `sub`, `mul`, `div`, `idiv`, `mod`, `neg`, `lt`, `gt`,
  `le`, `ge`, `eq_v`, `ne_v`, `boolnot`).
- `__perceus_dup` / `__perceus_drop` wrappers, small-int + char
  cache, singleton constants saturated to `INT32_MAX`.
- `pcs_is_non_last` switched to "dup all ≥ 2 non-lambda uses"
  (conservative variant) — m5.x Step A.
- `pcs_prepend_unused_drops` wraps multi-use / LUBlocked params
  with entry + exit drops — m5.x Step B.
- Stage 0 eager-dup retrofit on every local-binding read in value
  position — m5.x Step D.

Numbers on `kaic2` self-compile (KAI_TRACE_RC):

| metric        | pre-m5  | m5 #7   | Phase 1 (inert) | Phase 3 (flip) | + Tier 2 partial (2026-04-29 evening) |
|---------------|--------:|--------:|----------------:|---------------:|--------------------------------------:|
| alloc_total   | 130.7 M | 29.5 M  | 33.0 M          | 69.7 M         | (similar)                             |
| free_total    | 3.5 M   | 37      | 39              | 22.8 M         | (higher)                              |
| leaked        | 127.2 M | 29.5 M  | 33.0 M          | 46.9 M         | **23.8 M** (−49%)                     |
| live_peak     | 127.2 M | 29.5 M  | 33.0 M          | 46.9 M         | (similar)                             |
| max RSS       | 6.25 GB | n/a     | n/a             | 3.02 GB        | (untouched, Tier 2 hasn't moved RSS)  |
| wall time     | 2.15 s  | n/a     | n/a             | 5.74 s         | (untouched, expected to recover with kai_field/pat_test balance) |

The flip cut RSS in half and started calling `free` (~23 M times,
vs 37 calls pre-flip). Four Tier 2 partial-landings on
2026-04-29 (commits `8bd6431`, `73a12d4`, `7ab3d64`, and
`b3b1e2f`) cut the residual leak roughly in half: **46.9 M → 23.8 M
(−49%)**. The remaining ~24 M are the named sources below
(perceus_pass multi-read, kai_field/pat_test). The stage 0 emit-
side audit shipped with `b3b1e2f` extends `7ab3d64`'s single-use
optimisation past parameters to let-bindings and match-arm binds:
`stage1.c` `kai_internal_dup` count drops 1181 → 1090 (−7.7%) on
top of the param-only baseline; selfhost stays byte-identical.
Wall time has not moved — the dup machinery still fires for the
multi-use majority; recovering wall regression is paired with the
kai_field balance work that drops redundant increfs in the
match-test phase.

## What does NOT work today

Five named leak sources (all in `docs/m5x-followup.md` §4b–§5):

- **Match scrutinee net-zero workaround** — `emit_match_expr`
  brackets the body in `kai_incref(_scr)` … `kai_decref(_scr)`
  to preserve callsite refcount. Net zero, no leak reduction.
  The original 9fe6f6d shape decref'd whatever the scrutinee
  expression returned — fine when transferred, UAF when read.
  R3 in `docs/known-regressions.md` documents why we backed off.
- **`kai_truthy` non-consuming** — intentional. The LLVM
  short-circuit phi returns `lhs` in the early-exit branch, so
  consuming the truthy probe's argument would alias-free a
  value still referenced downstream. Pinned in
  `m5x-followup.md` Step C.
- **`kai_field` increfs without paired decref** — `pat_test`
  reads call `kai_field` repeatedly on the same scrutinee; each
  call increfs but no path decrefs the returned cells when the
  test fails its arm. Result: every match arm leaks one ref per
  field tested.
- **`kai_prelude_*` C helpers leak their params** — 9fe6f6d
  closed 12 of them (print/eprint, int/real string conversion,
  array & list ops). The remaining hand-written prelude
  helpers in `runtime.h` still borrow rather than consume.
- **Stage 0 eager-dup retrofit** — `emit_ident_value` in
  `stage0/emit.c` still emits `kai_internal_dup(kai_<name>)` on
  every multi-use, captured, or unresolved local read. The
  single-use, non-captured fast path landed in two steps
  (`7ab3d64` for fn params, `b3b1e2f` for let-bindings and match-
  arm binds), keyed by binding identity (`Node *`) so disjoint
  scopes don't collapse. Lambda body lets are still on the
  brute-force path — running a per-lambda-body counter is the
  follow-up. Each remaining wrap leaks one ref per read inside
  `kaic1`'s emitted code.

Plus one architectural debt:

- **`perceus_pass` does not dup multi-read let-bound vars across
  expression trees** — root cause of R3 (`interp.kai` panic).
  Documented as §4b in `m5x-followup.md`. Once fixed, the
  match-scrutinee net-zero workaround can drop and recover its
  ~0.7 M allocs of savings.

## Tier 1 — *Show HN honest* (~0 days)

Empty. The R1 flip already did the Show-HN work. A 30-minute live
demo on unfamiliar code does not reach the leak ceiling — RSS is
3 GB only on a self-compile of a 25 k-line compiler, not on any
demo someone would actually try in a tutorial.

The two correctness bugs that *would* embarrass us in a 5-minute
browse — R2 (m8x_2 SIGSEGV) and R3 (interp panic) — are already
closed (commits `3553e9f` and `235638f`). There is no Perceus
debt that surfaces below the Tier 2 boundary.

## Tier 2 — *Production-honest 1.0* (~5–8 days)

The set that lets the project drop "RC is partial" caveats and
claim "Tier 1 #2 (runtime-efficient) holds without footnotes".

| Item | Cost | Why |
|---|---:|---|
| **`perceus_pass` multi-read let dup** | ~1d | Root-cause fix for R3; closes §4b in `m5x-followup.md`; recovers ~0.7 M allocs of match savings |
| **Match-scrutinee real plug** | ~0.5d | Drop the net-zero workaround once §4b lands; recover the original 9fe6f6d savings |
| **`kai_field` / `pat_test` balance** | ~1–2d | Decref the increfed cells when the test fails its arm; biggest of the named leaks (per-arm × per-field) |
| **Remaining `kai_prelude_*` helpers** | ~1–2d | Audit every hand-written `kai_prelude_*` in `runtime.h`; flip the borrowing helpers to callee-consume, mirror the 9fe6f6d 12-helper pattern |
| **Stage 0 eager-dup retrofit cleanup** | ~0.5–1d | Single-use, non-captured fast path landed in `7ab3d64` (params) + `b3b1e2f` (lets + match arms). What's left: per-lambda-body counter to extend the same fast path to lambda-local lets, and a Perceus-style decref insertion at scope exit so the multi-use majority stops leaking. |

After this set, `leaked` should drop from 46.9 M to **< 5 M**
(threshold: noise floor of constant-pool reuse). RSS stays
≤ 3 GB but stable across runs; wall time recovers some of the
+2.7× regression once the redundant dups disappear.

## Tier 3 — *Full Perceus* (post-MVP)

The Koka feature set the m5 lane explicitly named "future
milestone". Items don't belong in 1.0; listed so they don't
accidentally get pulled into a 1.0-scoped lane.

| Item | Cost | Why post-MVP |
|---|---:|---|
| **Reuse-in-place** | ~1–2w | Constructor reuses consumed cell instead of `free` + `alloc`; needs alias analysis the type system can prove. Big win on linked-list rewrites; not needed for correctness. |
| **Drop specialisation** | ~1w | Decref chains generated per-type and inlined, instead of going through runtime dispatch. Performance, not correctness. |
| **Unboxing** | ~2–3w | Phase 1 unboxing (small-int + char cache) landed; full unboxing puts `Int` / `Bool` / `Char` / `Real` in native registers inside each fiber, heap-boxed only on the message boundary. Architectural shift; coordinates with multi-threaded scheduler tier 3 of fibers. |
| **Opt-in regions** | ~1–2w | Arena allocation for parser scratch / lexer state where RC overhead demonstrably costs more than a single arena reset. Power-user feature. |

CLAUDE.md should keep "Full Perceus is post-m12" pinned alongside
the multi-threaded scheduler decision.

## Sequencing recommendation

Right after m4c body subst (`r4-m4c-body-subst` lane in flight
as of this writing) closes, and **after** the fibers Tier 1
(R4 + stack guard pages, ~1 day):

1. **Tier 2 part 1 — perceus_pass + match-scrutinee plug**
   (~1.5 days). Closes §4b and recovers the deferred match
   savings. One commit each, easy to review.
2. **Tier 2 part 2 — kai_field/pat_test balance** (~1–2 days).
   Biggest leak source, design-heavy because the increfed cell
   may be needed by sibling tests on the same arm.
3. **Tier 2 part 3 — prelude helpers + stage 0 eager-dup
   audit** (~2–4 days). Mostly mechanical; the audit shape is
   the same as 9fe6f6d's 12-helper sweep.

After Tier 2, the kaikai CLAUDE.md claim "Tier 1 #2
(runtime-efficient) holds" is verifiable: `leaked` < 5 M on
self-compile, no footnote.

Tier 3 stays explicitly post-MVP. Reuse-in-place and unboxing
are the right path eventually but they require type-system
support (alias analysis, unboxed layout) that doesn't exist yet
and shouldn't be designed pre-1.0.

## What this document is NOT

- Not a calendar. Cost estimates assume nothing else competes
  for attention; `kai_field`/`pat_test` balance has REOPEN-ed
  before (lane-experience-m5x-1-2.md) so add 1–2d buffer.
- Not a list of all m5x-followup items — only those that
  affect honesty claims. Internal scaffolding work that doesn't
  surface to user code lives in `m5x-followup.md` proper.
- Not an excuse to defer Tier 2. The +2.7× wall regression and
  46.9 M residual leaks are the cost of the flip without the
  follow-throughs; paying this debt is what makes the v0.2.0
  trade reverse from "more honest, slower" to "more honest, no
  slower".
