# m5.x — Perceus + memory follow-up

Tracks every item the m5 lane (rounds 1-3, 2026-04-25/26) deferred
to a future milestone. The common thread is that **m5 v1 ships
scaffolding without flipping the runtime to consume args linearly**
— the dup/drop infrastructure is in tree but inert until stage 1
also has a perceus pass and the runtime can be flipped atomically
across both stages.

The single high-impact deliverable that did land in m5 v1 is **m5 #7**
(constant pool for nullary primitives): self-compile alloc count
dropped from ~131M to ~29.5M, a 77.4% reduction with no emitter
changes. The remaining ~29M still leak — that is the absence of a
Perceus discipline, not a Perceus inefficiency, and what the items
below address.

**Scope decision** (2026-04-29): see `docs/perceus-honesty-targets.md`
for which items below belong to Show-HN-honest, Production-honest 1.0,
and post-MVP tiers. This file is the operational inventory; the
honesty-target file is the strategic scope.

## Deferred items

### 1. m5 #9 step 3/4 — runtime primitive linear consumption *(LANDED 2026-04-28)*

Modify `kai_lt` / `gt` / `le` / `ge` / `eq_v` / `ne_v` / `add` / `sub` /
`mul` / `div` / `idiv` / `mod` / `neg` / `boolnot` / `truthy` / `field`
to decref their args linearly.

**Step 1 — stage 1 perceus port**: LANDED (`f327d34`, 2026-04-26).
`stage1/compiler.kai` now has its own `perceus_pass`, with magic-name
emit for `__perceus_dup` / `__perceus_drop`. Inert under the loose
runtime; ready to balance the runtime once the flip lands.

**Step 2 — atomic runtime flip**: BLOCKED. Attempted in the m5x-1-2
lane (2026-04-26) and reverted. Three compounding issues surfaced
during self-host:

1. C does not specify the evaluation order of function-call arguments.
   The lexically-last "transferred raw" read can fire before the
   dup-wrapped earlier reads (clang on AArch64 evaluates right-to-
   left), freeing the local before the dups execute. A conservative
   variant (any binding with ≥ 2 uses dups every read) is sound but
   not sufficient on its own — the other two issues remain.
2. `stage0/emit.c`'s `emit_pat_test` / `emit_pat_binds` for record
   patterns chains multiple `kai_field(_scr, ...)` on the same
   scrutinee. Under linear consumption the first field-test consumes
   `_scr`; subsequent tests UAF.
3. The kaic1 binary's machine code is whatever stage 0 emits, and
   stage 0 has no perceus pass. An eager-dup retrofit at every local
   read works for simple programs but interacts with closure capture
   construction and match-pattern aliasing in ways that need a
   substantial stage 0 audit.

Detail in `docs/perceus-basic.md` §"Step 3+ outcome (m5.x-1-2 lane)"
and `docs/lane-experience-m5x-1-2.md`.

**Step 2a — Phase 1 of m5.x-flip lane LANDED (2026-04-28):** stage 1 +
stage 2 `pcs_is_non_last` switched to the conservative "dup all ≥ 2
non-lambda uses" variant. Single-use bindings still transfer raw; any
binding with ≥ 2 non-lam uses now wraps every read in
`__perceus_dup`, removing the C-argument-order dependency from the
multi-use path. `pcs_count_non_lam_uses` is the new helper.
Selfhost (C + LLVM) byte-identical, `make test` clean,
`make demos-no-regression` baseline 20 holds. Inert under the loose
runtime; pays its way once step 2c (runtime flip) lands.

**Step 2b — exit drops for multi-use params (LANDED with 2c, 2026-04-28).**
The previous attempt to wrap fn bodies with
`let __pcs_ret = body; <exit_drops>; __pcs_ret` self-host UAFed at
typer time because match-arm and field-access destructures aliased
the producer's storage without an incref. Step 2c (the runtime flip
below) carries the producer-side incref-on-extract that makes those
exit drops sound; both ship together in one coherent commit.

**Step 2c — atomic runtime flip LANDED (2026-04-28):** ships Steps A,
B, C, and a stage-0 retrofit (D) together because each is unsound
without the others. Five concrete changes:

1. **A — Producer-side incref-on-extract (stage 0 + 1 + 2).**
   `emit_pat_binds` gains an `is_alias` flag. Set true at every nested
   destructure (variant args, cons head/tail, list-rest binding) and
   at the top-level match-arm call (because subsequent arms re-read
   `_scr` and a guard inside an arm may consume the binding —
   const-pattern desugar emits `__cv_NAME == NAME()`). Top-level let
   destructures (`_letv_*` from a transferred RHS) and record-field
   destructures (`kai_field` already increfs its return) keep
   is_alias=false. PBind under is_alias=true emits
   `KaiValue *kai_x = kai_incref(scr)` so the binding owns its own
   reference, decoupled from the producer's slot.

2. **B — Exit drops for multi-use / LUBlocked params (stage 1 + 2).**
   `pcs_prepend_unused_drops` now wraps the body as
   `EBlock([entry_drops..., SLet(__pcs_ret, body), exit_drops...],
   Some(EVar("__pcs_ret")))` when any param has LUBlocked or LUAt
   with `pcs_count_non_lam_uses ≥ 2`. LUUnused params keep the
   entry-drop they had under the inert Phase 1. Single-use LUAt
   params still transfer raw (no drop). Test bodies (`DTest`) now
   also run through `perceus_decl` so multi-use let-bindings inside
   tests — especially ones captured by closures — get their dup
   wraps; without this, the first invocation of a closure that
   reads a captured binding twice consumes both refs and the second
   invocation UAFs.

3. **C — 13 primitive flips in `stage0/runtime.h`.**
   `kai_add` / `sub` / `mul` / `div` / `idiv` / `mod` / `neg` / `lt` /
   `gt` / `le` / `ge` / `eq_v` / `ne_v` / `boolnot` decref their args
   after reading every relevant field. `kai_le` / `kai_ge` no longer
   decref `a` / `b` themselves (the inner `kai_gt` / `kai_lt` does);
   they only decref their own intermediate bool. `kai_truthy` is
   intentionally **not** flipped — the LLVM short-circuit lowering's
   phi returns `lhs` itself in the early-exit branch, so consuming
   the truthiness probe's argument would alias-free a value still
   referenced by downstream code. The `kai_apply` and `kai_field`
   primitives also stay non-consuming (apply is closure invocation,
   not a value op; field is introspection that already increfs its
   return).

4. **D — Stage 0 eager-dup retrofit.** Every local-binding read in
   value position (`emit_ident_value` in `stage0/emit.c`) now emits
   `kai_internal_dup(kai_<name>)` rather than the bare `kai_<name>`.
   Stage 0 has no perceus pass; eager dup is a brute-force way to
   keep the binding's reference alive across consuming primitives in
   kaic1's emitted code. Leaks one ref per local read, matching the
   pre-flip leak baseline.

5. **Runtime helper update.** `kai_prelude_map` / `_filter` /
   `_reduce` / `_each` previously borrowed `xs->as.cons.head` and
   passed the alias to `kai_apply`; under the flip the closure body
   consumed those refs and the caller's cons cell dangled (or the
   helper itself decref'd a freed value, as `_reduce` did with
   `acc`). Each helper now `kai_incref`s every arg before
   `kai_apply` so the closure can consume freely; `_reduce` keeps
   its own owned `acc` ref and decrefs the prior one at the end of
   each iteration.

**Numbers (kaic2 self-compile, 2026-04-28):**

| metric        | pre-m5     | m5 #7      | Phase 1 (inert) | Phase 3 (flip) |
|---------------|------------|------------|-----------------|----------------|
| alloc_total   | 130.7 M    | 29.5 M     | 33.0 M          | 69.7 M         |
| free_total    | 3.5 M      | 37         | 39              | 22.8 M         |
| leaked        | 127.2 M    | 29.5 M     | 33.0 M          | 46.9 M         |
| live_peak     | 127.2 M    | 29.5 M     | 33.0 M          | 46.9 M         |
| max RSS       | 6.25 GB    | n/a        | n/a             | 3.02 GB        |
| wall time     | 2.15 s     | n/a        | n/a             | 5.74 s         |

The +36.7 M alloc_total delta vs Phase 1 is the dup machinery
firing — every multi-use binding read calls `kai_internal_dup`
which calls `kai_incref` (counter increments per call). The
**52% RSS reduction** and **63% live_peak reduction** are the
load-bearing wins; wall time grew 2.7× because of the additional
RC bookkeeping on a per-allocation basis. Once Full Perceus lands
(drop specialisation + unboxing of Int / Real / Bool / Char), the
RC overhead should shrink.

**Gates passed (2026-04-28):**
- L1: `make selfhost` byte-identical (stage 1 + stage 2)
- L1: `make -C stage2 selfhost-llvm` byte-identical
- L1: `make test` clean (typer, runtime, emit, blocks, holes,
  effects, sugars, modules, protocols, demos-core, aspirational)
- L3: `make demos-no-regression` 20 passing (baseline 18)

L2 (invariant walker for `EVar` count ≥ 2 wrap-or-return-binding)
not added — the byte-identical selfhost gate establishes it by
construction. Worth adding when Full Perceus introduces more
re-write rules.

**Known remaining leak sources** (not blockers for this lane):
- Match scrutinee `_scr` is never decref'd at match exit; non-PBind
  top-level patterns leak the scrutinee (matches pre-flip baseline).
- `kai_truthy` non-consuming → `if (kai_truthy(<fresh>))` leaks the
  fresh expr per evaluation. Cleanup requires emit-side incref
  before short-circuit phi; deferred.
- `kai_field` always increfs but the test-phase reads in
  `emit_pat_test_record_fields` discard the incref'd value
  unmatched — leak per field test.
- Hand-written `kai_prelude_*` helpers in runtime.h leak their
  own params (no exit drop in C).
- Stage 0's eager-dup leaks one ref per local read in kaic1.

### 2. m5 #6 (doc grain) — `kai_closure` incref of captures *(LANDED)*

Landed (`80b0015`, 2026-04-26). `kai_closure` now increfs each
captured value at construction; the symmetric decref already lived
in `kai_free_value`'s KAI_CLOSURE branch. Inert under the loose
runtime — nothing decrefs primitive args mid-flight — and ready to
balance the closure-capture rc once step 1 above lands.

### 3. m4c retrofit — `clause_fn_name` should be fn-name aware *(LANDED 2026-04-29)*

Pre-fix: `_kai_clause_<line>_<col>_<op>` minted C symbol names by
source position without awareness of the enclosing function.
Duplicating polymorphic function bodies that contain `EHandle`
produced colliding symbol names, which the C linker rejected. That
was why `monomorphise` shipped as identity (m4c #1/#2 in
`stage2-design.md`).

Landed (`m4c #4`, this lane). The clause-info plumbing now threads
the **enclosing fn name** through both the C and LLVM emit paths:

- `ClauseInfo` gained an 8th `String` field carrying the enclosing
  fn name, populated by `lc_record_clauses` from
  `LamCollect.cur_enc_fn`. The collect walker tracks `cur_enc_fn`
  at scope boundaries (DFn / DTest entry, ELambda recursion).
- `clause_fn_name(enc_fn, line, col, op)` mints
  `_kai_<enc>__clause_<line>_<col>_<op>`.
- The two install sites
  (`emit_clause_assignments` / `llvm_emit_handle_clause_assigns`)
  look up `enc_fn` from the matching `ClauseInfo` via
  `lookup_clause_enc_fn` so install code matches the body's symbol.
- `monomorphise` now appends specialised copies of polymorphic
  DFns per distinct call-site type tuple. Originals stay live
  (call-site rewrite is deferred — see `docs/m4c-real-specialisation.md`
  Phase 2 follow-up). The collision-avoidance is exercised end-to-end
  by `examples/effects/m4c_handler_in_body.kai`, which has a
  polymorphic body with an embedded `EHandle` called at two
  distinct (a, b) tuples — three clause symbols emitted (one for
  the original polymorphic body, two for the specialisations) and
  the binary links and runs on both backends.

Selfhost C + LLVM byte-identical. `make test` clean modulo R-interp
and R-m8x2 (both pre-existing on `main` HEAD; pinned in
`docs/known-regressions.md`).

Deferred to follow-up lanes:
- Real call-site rewrite (today the specialisations are emitted
  alongside the original but the original is what gets called).
- Body type substitution to remove `try_rewrite_show_dim_real`.
- Generic prune (drop the polymorphic decl when every reference
  has been redirected). See `docs/m4c-real-specialisation.md`.

### 4. LLVM emit mirror of m5 #3 *(LANDED 2026-04-28)*

Mirrors the m5 #3 C-emitter pass into the stage 2 LLVM emitter.
`llvm_emit_block` now computes the per-block drop set via the same
`block_unused_lets` walker the C-emitter uses, and routes statements
through `llvm_emit_stmts_with_drops` / `llvm_emit_stmt_with_drops`.
For `SLet PBind`, when the bound name appears in the drop set, the
emitter appends `call void @kaix_decref(%KaiValue* <reg>)` after the
binding. For `SLet PWild` whose RHS is a fresh allocation, the same
decref is emitted unconditionally, matching the C path. A new
`kaix_decref` runtime wrapper in `runtime_llvm.c` exposes
`kai_decref` to the LLVM backend.

Selfhost C + LLVM byte-identical, `make test` clean,
`make demos-no-regression` 20 passing (baseline 18). Stage 2's own
self-emit (`kaic2 --emit=llvm stage2/compiler.kai`) now contains 11
`kaix_decref` call sites, confirming the pass is active.

### 4b. perceus_pass should dup let-bound vars read multiple times across an expression tree *(LANDED 2026-04-29 evening — perceus-tier-2 lane)*

Root cause confirmed by inspection: the collector walked through
each interp body's parsed Expr and counted multi-reads correctly,
but `pcs_rewrite_kind`'s `EStr(_, _)` case fell through to
`map_expr_kind` which treats the span as opaque and returns it
unchanged. The inner reads inside `#{...}` were never wrapped in
`__perceus_dup`, even though the outer use-count was ≥ 2.

**Fix landed**: `pcs_rewrite_estr_span` (stage 1 + stage 2). For
each `#{esrc}` body, re-tokenise + re-parse the source, run
`pcs_rewrite_expr` on the parsed Expr (which encodes wrap
decisions in the AST), then walk the rewritten Expr to collect a
`[DupMark]` of (line, col) for each EVar wrapped in
`__perceus_dup`. Token positions in the original `esrc` give us
exact source offsets, so we walk the original tokens and replace
each marked TkIdent with `__perceus_dup(NAME)` while copying the
surrounding source verbatim. The reassembled span rounds back
through the parser at emit time as a regular ECall — no printer
needed.

Required spelling `"#"`+`"{"` (kaikai has no escape for `#{` in
literals; a bare `"#{"` lexes as the start of an unterminated
interp).

With §4b in place, `emit_match_expr`'s entry incref / exit decref
bracket dropped to recover the original 9fe6f6d shape (linear
consumption of `_scr`). Stages 0 and 1 also picked up the exit
decref so the kaic1 / kaic2 binaries themselves stop leaking
match scrutinees during their own runtime.

Numbers (`kaic2` self-compile, `KAI_TRACE_RC`):

| metric        | partials baseline | + §4b + match scr  |
|---------------|------------------:|-------------------:|
| alloc_total   |             33.5M |              34.2M |
| free_total    |              8.2M |              20.8M |
| leaked        |             25.4M |              13.4M |
| live_peak     |             25.4M |              13.4M |

`leaked` cut by 47% on top of the partials.

### 5. Full Perceus (post-m5 milestone)

The m5 lane scope was *basic* Perceus: walker scaffold + last-use
analysis on fn parameters + drop unused fresh allocations + dup/drop
infrastructure. The full optimisation surface stays for a future
milestone:

- **Reuse-in-place** (Koka style): a constructor reuses the
  consumed cell in place of `free` + `alloc` when the type system
  can prove no live aliasing remains.
- **Drop specialisation**: decref chains generated per type and
  inlined, instead of going through the runtime's dispatch.
- **Unboxing**: `Int` / `Bool` / `Char` / `Real` in native machine
  registers inside each fiber. Heap boxing only for compound
  immutable values. Messages across fiber boundaries copy.
- **Opt-in regions**: for intermediate buffers (parser scratch,
  lexer state) where reference counting demonstrably costs more
  than an arena.

**Cost**: weeks, not days. Lands as its own multi-milestone block
post-m12 selfhost checkpoint. Doc placeholder in `stage2-design.md`
§*Full Perceus*.

## Follow-ups inherited from m5 #0 baseline

These are tracked in `perceus-basic.md` §*Follow-ups* and surface
again here for index visibility:

1. **Runtime symbol shadowing**: kaikai functions named `add` /
   `sub` / `neg` / etc. collide with the runtime's `kai_add` / `sub`
   / `neg`. Three fix options with different costs documented in
   `perceus-basic.md`.
2. **m14 nominal migration coupling**: when m14 migrates `list_take`
   → `list.take`, the 16 m14-pre functions need to migrate together.
   The flat-prefix style sits in `stdlib/core/*.kai` (split from the
   former monolith on 2026-04-27); the migration is mechanical but
   coordinated.
3. **kaic2 typer rejects polymorphic prelude calls from foreign
   files**: blocks several tests under `examples/` from running
   against `kaic2` directly. Workaround in place; root cause stays
   in m7b parametric-effect plumbing.
4. **Per-process double `KAI_TRACE_RC` report when run via
   `bin/kai`**: the env var is inherited by the child binary, so
   the wrapper's report and the program's report both fire. Cheap
   fix: unset `KAI_TRACE_RC` before exec'ing the program, or have
   the wrapper opt out.

## What does NOT belong here

- The **stage-1 perceus port** itself (item 1 step 1) is its own
  milestone, not an m5.x deferral. It is the *prerequisite* that
  unblocks the m5.x items.
- Any m12.5 (UoM), m12.6 (refinements + contracts), m12.7 (axiom),
  m13 (bench + bit ops) work — those are independent milestones,
  not Perceus follow-ups.
