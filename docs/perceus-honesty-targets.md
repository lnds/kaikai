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

## Where we are today (2026-04-29 evening — perceus-tier-2 lane)

Tier 2 architectural debt closed. Three items landed in one lane:
`pcs_rewrite_estr_span` (closes §4b), match-scrutinee real plug
(stages 0 + 1 + 2), and `kai_field_borrow` for pat_test
(record-pattern test paths). `leaked` cut from 25.4 M to
13.4 M (−47%) on `kaic2` self-compile. Selfhost (stage 1 +
stage 2 + LLVM) byte-identical, R3 closed-loop fixture
(`examples/effects/interp_recursive_walk.kai`) still green
without the workaround. Tier 2 mechanical follow-throughs left:
remaining `kai_prelude_*` helpers + stage 0 eager-dup retrofit
cleanup; together those should bring `leaked` under the < 5 M
threshold.

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

| metric        | pre-m5  | m5 #7   | Phase 1 (inert) | Phase 3 (flip) | + Tier 2 partial (2026-04-29) | + perceus-tier-2 (§4b + match + field) |
|---------------|--------:|--------:|----------------:|---------------:|------------------------------:|--------------------------------------:|
| alloc_total   | 130.7 M | 29.5 M  | 33.0 M          | 69.7 M         | 33.5 M                        | 34.3 M                                |
| free_total    | 3.5 M   | 37      | 39              | 22.8 M         | 8.2 M                         | 20.8 M                                |
| leaked        | 127.2 M | 29.5 M  | 33.0 M          | 46.9 M         | 25.4 M                        | **13.4 M** (−47%)                     |
| live_peak     | 127.2 M | 29.5 M  | 33.0 M          | 46.9 M         | 25.4 M                        | 13.4 M                                |
| max RSS       | 6.25 GB | n/a     | n/a             | 3.02 GB        | (similar)                     | (similar)                             |
| wall time     | 2.15 s  | n/a     | n/a             | 5.74 s         | (similar)                     | (similar)                             |

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

Two named leak sources remain (`docs/m5x-followup.md` §1 step 2c
notes + §5):

- **`kai_truthy` non-consuming** — intentional. The LLVM
  short-circuit phi returns `lhs` in the early-exit branch, so
  consuming the truthy probe's argument would alias-free a
  value still referenced downstream. Pinned in
  `m5x-followup.md` Step C.
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

Closed in the perceus-tier-2 lane (2026-04-29 evening):

- ~~Match scrutinee net-zero workaround~~ — replaced by
  linear consumption (`kai_decref(_scr)` at exit) once §4b
  removed the precondition.
- ~~`kai_field` increfs without paired decref in pat_test~~ —
  added `kai_field_borrow` for read-only test paths.
- ~~`perceus_pass` does not dup multi-read let-bound vars
  across expression trees~~ — closed by `pcs_rewrite_estr_span`.

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

| Item | Cost | Status |
|---|---:|---|
| ~~**`perceus_pass` multi-read let dup**~~ | ~~~1d~~ | **LANDED 2026-04-29 evening (perceus-tier-2 lane).** §4b closed: `pcs_rewrite_estr_span` walks each `#{...}` body, runs the parsed Expr through `pcs_rewrite_expr`, and surgically rewrites the span. |
| ~~**Match-scrutinee real plug**~~ | ~~~0.5d~~ | **LANDED 2026-04-29 evening (perceus-tier-2 lane).** Workaround dropped from `emit_match_expr`; stages 0 + 1 picked up the exit decref so kaic1 / kaic2 binaries also stop leaking match scrutinees. |
| ~~**`kai_field` / `pat_test` balance**~~ | ~~~1–2d~~ | **LANDED 2026-04-29 evening (perceus-tier-2 lane).** Added `kai_field_borrow` runtime helper + `kaix_field_borrow` LLVM symmetric; switched record-pattern test paths in stage 0 / 1 / 2 emitters. Smaller measurable impact than expected on `kaic2` self-compile (only 1 record-pattern test in stage 2's source) but a correctness win for record-heavy programs. |
| **Remaining `kai_prelude_*` helpers** | ~1–2d | Audit every hand-written `kai_prelude_*` in `runtime.h`; flip the borrowing helpers to callee-consume, mirror the 9fe6f6d 12-helper pattern |
| **Stage 0 eager-dup retrofit cleanup** | ~0.5–1d | Single-use, non-captured fast path landed in `7ab3d64` (params) + `b3b1e2f` (lets + match arms). What's left: per-lambda-body counter to extend the same fast path to lambda-local lets, and a Perceus-style decref insertion at scope exit so the multi-use majority stops leaking. |

After the **3 closed items**, `leaked` dropped from 25.4 M to
**13.4 M (−47%)**. The remaining ~13 M needs the prelude helpers
audit + stage 0 eager-dup cleanup to push under the < 5 M
threshold; the named architectural debt (perceus_pass §4b,
match-scrutinee, kai_field balance) is now closed.

## Tier 2.5 — *Unboxing Phase 2* (in-MVP, ~5–7 days)

**Decision pinned 2026-04-30**: Phase 2 unboxing moves into MVP
scope. Without it, the user-facing performance gap vs C native is
~50–100× (every `Int` is a heap-allocated `KaiValue *`). Phase 2
brings the gap to ~5–10×, which is in line with what OCaml /
Haskell / Go ship as their default representation. Phase 3 (the
full Koka feature set listed below) stays post-MVP.

Phase 1 unboxing (small-int + char cache for `[-128, 127]` ints
and `[0, 127]` chars) already landed (commit `69c6166`); Phase 2
is the contained chunk that does **not** require the
multi-threaded scheduler / cross-thread atomics design that
Phase 3 needs.

| Phase | What it does | Status | Performance vs C |
|---|---|---|---:|
| Phase 1 — small-int + char cache | Cache hits for ints in [-128, 127] / chars [0, 127] reuse singletons; no alloc | ✅ landed (`69c6166`) | 50–100× slower (cache hits free, miss boxes) |
| **Phase 2 — locals + return values unboxed** | `Int` / `Bool` / `Char` live in C `int64_t` / `int` directly inside fn bodies; boxing only at function-call boundaries and storage edges. Escape analysis decides what gets boxed. | ⏳ in-MVP, this lane | **5–10× slower (boxing only at boundaries)** |
| Phase 3 — full unboxing (Tier 3 below) | Reuse-in-place, drop specialisation, regions, cross-fiber unboxed messages, type-erased layouts | ⏳ post-MVP | 1–2× slower (close to C) |

**Phase 2 scope** (single lane after `fibers-tier-2` closes):

- Add a "value mode" tag to every Expr node in the typed AST:
  `boxed` (current behaviour, `KaiValue *`) or `unboxed` (raw
  `int64_t` / `int`).
- Escape analysis: a local that flows only into other unboxed
  contexts stays unboxed. A local that flows into a function
  call argument, a heap store, an effect op handler, or any
  other "boundary" boxes at the boundary.
- Emitter generates `int64_t kai_<name>` instead of
  `KaiValue *kai_<name>` for unboxed locals; arithmetic and bit
  ops on unboxed values are direct C operators (no `kai_int(...)`
  wrap).
- Function signatures stay boxed for now (the call boundary is
  the cheapest place to box). Inlining + C compiler LTO recover
  some of this; full unboxed signatures move to Phase 3.

**Costs**: ~5–7 days real. Design-heavy (first time kaikai
tracks distinct representations for the same type) but contained
— no multi-threading, no regions, no alias analysis full.

**Acceptance**: `examples/quickstart/02_fizzbuzz.kai` benchmark
under `-O2` runs within ~5–10× of a hand-written C equivalent
of the same algorithm (today the gap is ~50–100×). Selfhost +
demos baseline 24 hold byte-identical.

## Tier 3 — *Full Perceus* (post-MVP)

The Koka feature set the m5 lane explicitly named "future
milestone". Items don't belong in 1.0; listed so they don't
accidentally get pulled into a 1.0-scoped lane.

| Item | Cost | Why post-MVP |
|---|---:|---|
| **Reuse-in-place** | ~1–2w | Constructor reuses consumed cell instead of `free` + `alloc`; needs alias analysis the type system can prove. Big win on linked-list rewrites; not needed for correctness. |
| **Drop specialisation** | ~1w | Decref chains generated per-type and inlined, instead of going through runtime dispatch. Performance, not correctness. **Investigated 2026-05-01 (Tongariki) — closed as doc-only PR**: implementation reached end-to-end (selfhost C + LLVM byte-identical, tier1 green) but measured −1.7% wall at `-O2` and +5.4% regression at `-O0` on `kaic2` self-compile. Phase 2 unboxing already absorbed the bulk of the addressable overhead. Lane retro: `docs/lane-experience-drop-specialisation.md`. Re-evaluating belongs *after* reuse-in-place lands — the alloc mix shift may unlock per-tag dispatch wins on KAI_RECORD / KAI_VARIANT (61% of allocs) that the v1 scope deliberately left out. |
| **Unboxing Phase 3** (full Koka-style) | ~1–2w on top of Phase 2 | Cross-fiber unboxed messages, type-erased layouts, reuse-in-place coordination. Coordinates with multi-threaded scheduler. |
| **Opt-in regions** | ~1–2w | Arena allocation for parser scratch / lexer state where RC overhead demonstrably costs more than a single arena reset. Power-user feature. |

CLAUDE.md should keep "Full Perceus Phase 3 is post-m12" pinned
alongside the multi-threaded scheduler decision. Phase 2 is
in-MVP per the 2026-04-30 decision above.

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

### TCO half of Tier 1 #2 — landed 2026-04-30 (issue #37)

The "Mandatory tail-call optimisation" half of Tier 1 #2 was
the second hidden footnote: until 2026-04-30, the C the
emitter generated wrapped each recursive call in a Perceus
exit-drop statement-expression, which inhibited gcc / clang
TCO at `-O2`. R5 (issue #34, PR #36) papered over the
resulting Linux SEGV with a runtime `RLIMIT_STACK` bump but
the underlying claim — "constant C-stack for kaikai
self-tail-recursive fns" — remained aspirational.

Issue #37 (this lane) lands the proper fix: a
`tcrec_rewrite_decls` pass between unboxing and Perceus
rewrites every self-tail-call's callee `EVar` into a
`__kai_tcrec|<c_sym>|<dropmask>|<p0>|<p1>|...` sentinel; the
C backend emits a rebind+goto block at each call site and
plants `_kai_<c_sym>_entry:;` before the enclosing return.
Verified empirically: `demos/build/euler4-bin` runs to
completion under `ulimit -s 256` on macOS (the recursion
depth is ~1 M, which would otherwise need ~100 MB of
C-stack), and the runtime `RLIMIT_STACK` bump has been
removed from `stage0/runtime.h`.

Caveats that survive into a follow-up:

- The rewrite lives in `stage2/compiler.kai`, so it covers
  *emitted programs* (demos, kaic2's own selfhost output).
  The bootstrap chain `kaic0 → kaic1 → kaic2` is built by
  stage 0 / stage 1, whose emit still uses the recursive
  shape, so the kaic2 binary's internal `lex_loop` etc.
  remain stack-bound. The PR #36 `RLIMIT_STACK` constructor
  in `stage0/runtime.h` therefore stays on until the rewrite
  is mirrored into `stage1/compiler.kai` (and `stage0/emit.c`
  audited). The honesty claim "Tier 1 #2 holds" applies to
  *kaikai source programs*, not to the bootstrap chain.
- Mutual tail-recursion (`f → g → f`) is out of scope —
  v1 only rewrites self-recursive calls, not cross-fn.
  Trampolining or whole-program analysis would be the
  next step if the gap becomes load-bearing.
- LLVM backend still emits a normal call when it sees the
  sentinel — TCO via the LLVM `tail` marker is a separate
  lane (issue #37 non-goals).
- Fns with any `LUUnused` parameter keep the normal-call
  shape, because the wrap's *entry* drops would re-fire
  on each iteration of the goto loop. Hoisting entry
  drops above the label is a follow-up.

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
