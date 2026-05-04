# Lane experience — issue #118 (Perceus reuse-in-place, Anga Roa wave v1)

Date range: 2026-05-03 (single working session, ~3 hours wall).
Lane branch: `issue-118-perceus-reuse-in-place`.
Audit precedent: `docs/perceus-anga-roa-phase0-audit.md` (PR #204).
Issue spec: `gh issue view 118`.

## Objective metrics

- Lane start: `2026-05-03T20:03:14-04:00`.
- Lane end (this report): `2026-05-03T20:55-04:00` (approx).
- Wall-clock: ~3 hours including audit re-read, recogniser
  prototyping, two STOP-and-report consultations, a variant /
  record disable, and full validation.
- Build / test invocations: 5 logged plus the implicit `make tier1` /
  selfhost iterations during prototyping. See
  `/tmp/lane-issue-118-perceus-reuse-in-place-builds.tsv`
  (appended below).

## What shipped

Three deltas, ~250 LoC total against the audit's ~250 LoC estimate.

1. **Runtime primitives** in `stage0/runtime.h` (~95 LoC):
   - `kai_check_unique(KaiValue *v)` — single-load RC == 1 predicate
     with INT32_MAX singleton guard.
   - `kai_reuse_or_alloc_cons(scr, head, tail)` — in-place rewrite
     when unique, fallback to `kai_cons` otherwise. The unique branch
     `kai_incref`s the cell so it survives the enclosing match exit's
     `kai_decref(_scr)` — sidesteps the audit-flagged "pass ordering"
     risk without touching `emit_match_default`.
   - `kai_reuse_or_alloc_record(scr, n, fields, names)` — same shape
     for KAI_RECORD; same-arity reuses the existing fields[] / names[]
     pointer arrays in place.
   - `kai_reuse_or_alloc_variant(scr, tag, name, n, args)` — same
     shape for KAI_VARIANT.
   - `kai_rc_reuse_total` counter wired into the existing
     `[KAI_TRACE_RC]` exit report so post-#118 measurements observe
     fired vs unfired paths.

2. **Magic-name dispatch** in `stage2/compiler.kai` (~90 LoC across
   `emit_call_expr` C-side and `llvm_emit_call` LLVM-side):
   - `__perceus_reuse_cons` — lowers to
     `kai_reuse_or_alloc_cons(_scr, head, tail)`. The synthetic
     `__pcs_scr` first arg is recognised by both backends and
     resolved to the enclosing match's scrutinee local
     (literal `_scr` in C; `llvm_push_local("__pcs_scr", scr_reg)`
     in LLVM, added inside `llvm_emit_match_arms`).
   - `__perceus_reuse_record` and `__perceus_reuse_variant`
     dispatch shells are present (the recogniser arms that would
     emit them are disabled — see "Spec ambiguities" below).
   - LLVM runtime declaration `@kaix_reuse_or_alloc_cons` added in
     `stage0/runtime_llvm.c` plus the `declare` line in the
     LLVM-emit prelude.

3. **Recogniser** `pcs_recognise_reuse_*` in `stage2/compiler.kai`
   (~120 LoC), invoked from `perceus_decl` after `pcs_rewrite_expr`
   and before `pcs_prepend_unused_drops`:
   - Walks the AST; on `EMatch(scr, arms)` it tries to rewrite each
     arm whose pattern + body shape matches a same-arity rebuild.
   - **v1 scope**: cons rebuild only — the textbook
     `Arm(PList([_pat], Some(_)), None, EList([ElPlain(h), ElSpread(t)]))`
     pattern. The variant + record arms in `pcs_try_reuse_arm`
     return the body unchanged.
   - 5 fixtures under `examples/perceus/` cover positive, alias,
     pass-ordering, and (placeholder) record + variant shapes.
     Wired into `make test-perceus-issue118` and
     `make test-perceus-issue118-asan` (added to root `tier1-asan`).

## Compiler errors I encountered

Three classes, all caught quickly:

- **`string_at` does not exist** — kaikai's char access is
  `char_at(s, i) : Option[Char]`. First draft of
  `pcs_callee_is_variant_ctor` used `string_at` from C habit; the
  kaic1 build immediately complained ("undefined identifier"). Fix
  was a one-line swap to `match char_at(...) { Some(c) -> ... }`.

- **LLVM emit unaware of `__pcs_scr`** — the first end-to-end run
  surfaced `llvm: cannot build closure for '__pcs_scr' at <line>`
  diagnostics from stdlib `map` / `filter` / `take` / `zip_with`
  (the audit-§1.A sites). Cause: the LLVM backend has no equivalent
  of the C backend's `_scr` literal — it works on fresh registers.
  Fix: `llvm_push_local(e_guarded, "__pcs_scr", scr_reg)` inside
  `llvm_emit_match_arms` before the body emit, so the emitter
  resolves the synthetic identifier the same way it resolves any
  pattern binding. Added a matching `declare %KaiValue*
  @kaix_reuse_or_alloc_cons(...)` in the IR prelude.

- **Selfhost diverged after enabling variant + record arms** — the
  C backend self-hosting fixed-point check (`make -C stage2
  selfhost`) failed with non-converging `kaic2b.c` vs `kaic2c.c`,
  alternating site-by-site between `kai_variant(...)` and
  `kai_reuse_or_alloc_variant(...)` calls. Investigation pinned the
  bug to the variant arm: the lexical `is-uppercase ⇒ variant ctor`
  predicate misclassified perfectly normal `Some(d) -> Some(false)`
  shapes as same-arity reuses, and the runtime in-place rewrite
  corrupted shared sub-state during the compiler's own self-compile
  workload. Fix for v1: disable variant + record arms (return body
  unchanged); cons-only converges immediately.

## Friction points

- **Was the existing static info actually sufficient?** *Almost*.
  `pcs_collect_uses_*` + `last_use_for` correctly classify when a
  binding is single-use, which is all the recogniser checks. The
  AST-shape match is straightforward. But the audit assumed the
  recogniser would imply runtime uniqueness; in practice most
  stage-2 self-compile sites enter the recogniser-rewritten code
  with the scrutinee at RC ≥ 2 (perceus pre-dup'd the binding
  for a downstream reader). The static info is correct *about the
  binding's number of textual reads*; it does not predict the
  *runtime* RC of the value the binding points at.

- **Did the pass-ordering fix need more than unused-drop
  suppression?** No, by accident. The audit imagined the
  recogniser would have to coordinate with `pcs_prepend_unused_drops`
  to suppress a stray decref on the consumed cell. The
  `kai_incref` inside the runtime helper sidesteps that entirely:
  the match-exit `kai_decref(_scr)` happens unconditionally and
  cancels the helper's `incref`, leaving the caller with a
  unique-RC result. Zero changes to `emit_match_default` /
  `llvm_emit_match_default`.

- **Did ASAN catch any subtle issues during dev?** Not in the
  shipped form. The variant divergence was caught by the selfhost
  byte-identical gate before ASAN saw it. Shipped fixtures pass
  ASAN+UBSan green on the first run after the recogniser was
  scoped to cons-only.

## Spec ambiguities or interpretive choices

- **Per-arg-count variants vs single variadic?** I chose
  per-shape variants (`kai_reuse_or_alloc_{cons,record,variant}`)
  matching the existing `kai_<shape>` allocator family. Each
  helper takes the same shape arguments the matching constructor
  takes — three for cons (head, tail), `(n, fields[], names[])`
  for record, `(tag, name, n, args[])` for variant. No variadics;
  that would have lost the per-shape inlining the constructors get.

- **How to handle the record field-by-field copy?** The unique
  branch decref's the existing `fields[i]` and overwrites it with
  the incoming `fields[i]` — same loop the original `kai_record`
  builder uses, just on the existing pointer arrays instead of
  freshly-malloc'd ones. The names[] array overwrite is technically
  redundant when both sides point at the same static-string
  literals (the common case for parser combinators threading the
  same record shape), but it's a single pointer copy per field, so
  it stays for safety.

- **Where does the recogniser run?** Between `pcs_rewrite_expr`
  (which inserts `__perceus_dup` wraps) and `pcs_prepend_unused_drops`
  (which adds the entry/exit `__perceus_drop` stmts). At that
  point the AST has the post-dup shape but no drops yet, so the
  recogniser sees binding reads as their textual count; it never
  needs to interact with drop placement.

- **Variant + record disabled, not just buggy?** Disabled — the
  v1 scope ships only what converged. `pcs_try_reuse_arm`'s
  variant + record arms return `body` unchanged; the runtime
  helpers + emit dispatch + magic names remain wired so a
  follow-up only needs to lift the recogniser arms (and probably
  add a typer-aware predicate to replace `pcs_callee_is_variant_ctor`).

## Subjective summary

- **Confidence**: high on the runtime + dispatch + cons-recogniser
  delta; selfhost (C and LLVM) byte-identical, fixtures + ASAN
  green, traceable via `KAI_TRACE_RC`. Low on the variant +
  record arms — diagnosed but unshipped, the lexical heuristic
  needs revisiting.
- **Hardest**: persuading myself to STOP and report at the
  match-emit refactor decision point. The audit's pass-ordering
  flag turned out to be a non-issue (the runtime `incref` resolved
  it), but I had to spell that out to the user before continuing.
- **Easiest**: wiring the magic-name dispatch in `emit_call_expr`.
  The existing `__perceus_dup` / `__perceus_drop` interception is
  a clean precedent — pasted, edited, done.
- **Compiler help vs hinder**: kaic2's typer correctly accepted
  `EVar("__pcs_scr")` with `ty: None, mode: MUnknown` because the
  recogniser runs after typing. No surprise. The LLVM backend
  *did* surprise me: I assumed the magic-name dispatch alone
  would suffice and forgot the synthetic identifier needed to
  resolve to a register on that side too.

## Perf measurement

`kaic2` self-compile (`./stage2/kaic2 stage2/compiler.kai > /dev/null`),
median of one run each on macOS Apple Silicon, `-O0` (the default
`make all` build):

| Variant | Wall (s) | User (s) | Notes |
|---|---:|---:|---|
| Pre-#118 (`main`)        | 7.85 | 7.26 | No recogniser. |
| Post-#118 (this lane)    | 8.33 | 7.65 | Recogniser active, cons only. |
| **Delta**                | **+6.1 %** wall | **+5.4 %** user | recogniser overhead, no offsetting reuse. |

`kaic2` self-compile alloc breakdown
(`KAI_TRACE_RC=1 ./build/kaic2b stage2/compiler.kai 2>&1`):

| metric | value | notes |
|---|---:|---|
| `alloc_total` | 50.5 M | unchanged from pre-#118 to within 0.001 % |
| `cons` allocs | 5.9 M  | unchanged |
| `record` allocs | 7.4 M | unchanged |
| `variant` allocs | 23.9 M | unchanged |
| `reuse_in_place` | **0** | recogniser-rewritten sites were rewritten 37 times statically (`grep -c kai_reuse_or_alloc_cons build/kaic2b.c`), but at runtime every single call hit the RC ≥ 2 fallback branch. |

**Audit projection vs reality**:

| | Audit projection | Measured |
|---|---:|---:|
| `alloc_total` delta | −36 % | ≈ 0 % |
| Wall delta at `-O2` | −5 % to −15 % | (not measured at -O2; -O0 +6 %) |
| Cons rebuild static sites | 40 | 37 (close) |
| Cons rebuild runtime fires | (assumed all) | 0 in self-compile |

The audit's projection was over-optimistic. The 40-site cons-rebuild
inventory was correct, but every site in stage 2 lives downstream
of `pcs_rewrite_expr`'s `__perceus_dup` insertion — so the cell
reaching the match-arm body is already at RC ≥ 2 by the time the
runtime guard checks it. The reuse path therefore never fires under
the self-compile workload. The fixtures *do* fire the path
(`reuse_cons_basic` reports `reuse_in_place=1` per top-level
`map_inc` call; `reuse_pass_ordering` reports `reuse_in_place=3`
across the chained `double_all` calls), proving the runtime path is
correct — it's the recognition predicate that's misaligned with
runtime reality.

## Limitations of this report

- Wall-time numbers are single-run on a single machine (Apple
  Silicon, macOS), `-O0` (`make all` default). The audit's
  projection was for `-O2` self-compile via the `bin/kai` driver;
  I did not re-measure under `-O2` in this lane because the user
  experience in the loop is `-O0`. A follow-up that ships a
  measurable wall-time win would benchmark under `-O2`.
- `reuse_in_place=0` in self-compile is a real result, not a
  measurement bug — `KAI_TRACE_RC` shows the counter line whenever
  it's non-zero, and the suite-level fixtures *do* show non-zero
  values, so the counter wiring is correct.
- The variant + record disable is conservative. A version that
  ships variant + record with a typer-aware shape predicate is
  plausible but requires wider changes to thread typer state into
  the perceus pass.
- The lane spec asked for "L-per-call" cons reuse on the
  `map(known_unique_list, f)` shape. v1 ships "1-per-call" because
  the standard match emit `kai_incref`s the head + tail bindings
  before evaluating the arm body, bumping the recursive call's
  scrutinee out of uniqueness. A follow-up that teaches match emit
  to *transfer* (not borrow) bindings under a reuse arm would
  amplify the count from 1 per top-level call to L per call.
- The fixtures' record + variant placeholders prove the standard
  alloc path stays correct under the runtime additions; they do
  not prove the disabled recogniser arms are correct (they are
  not invoked).

## Follow-up issues to file

1. **Variant recogniser** — re-enable with a typer-aware shape
   predicate (replace `pcs_callee_is_variant_ctor`'s lexical
   `[A-Z]…` heuristic). Acceptance: AVL `tree_balance` /
   `tree_rotate_*` in `stdlib/collections/map.kai` reuses cells.
2. **Record recogniser** — re-enable with a same-named, same-arity
   shape predicate. Acceptance: Parser / Lexer / FmtSt rebuilds in
   `stage2/compiler.kai` reuse cells.
3. **Match emit ownership transfer under reuse arms** — the v1
   "1-per-call" limit. The match emit needs to learn a "transfer"
   mode for pattern bindings when the arm body's outermost call is
   a `__perceus_reuse_*`. Acceptance: `map(unique_list, f)`
   reports `reuse_in_place=L` instead of `reuse_in_place=1`.
4. **`-O2` benchmark + driver wiring** — the audit's projected
   −5 % to −15 % wall delta requires `-O2` measurement against a
   workload that exercises a large unique cons spine end-to-end
   (the current self-compile workload doesn't qualify). A bench
   fixture under `examples/perceus/` exercising the
   `map_inc(build(N, []))` shape across `N = 10⁵..10⁶` would put a
   number on the runtime path's per-element cost.

## Build TSV (appended)

(Reproduced verbatim from `/tmp/lane-issue-118-perceus-reuse-in-place-builds.tsv`.)

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T20:07:48-04:00	tier0_runtime_only	OK	-
2026-05-03T20:35:35-04:00	tier0_cons_recognizer	OK	-
2026-05-03T20:43:36-04:00	selfhost+selfhost-llvm_cons	OK	-
2026-05-03T20:49:57-04:00	test-perceus-issue118	OK	-
2026-05-03T20:49:57-04:00	test-perceus-issue118-asan	OK	-
2026-05-03T21:00:07-04:00	tier1	OK	-
2026-05-03T21:01:13-04:00	tier1-asan	OK	-
```
