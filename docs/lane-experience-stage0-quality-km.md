# Lane experience — stage 0 quality pass (km-guided)

## Scope as planned vs. as shipped

**Planned (initial brief):** capture a km (kimun) quality baseline for `stage0/`,
draft a prioritised refactor plan, and stop for instructions. Strictly
measure-and-plan; touch no code.

**As shipped:** the measure-and-plan phase ran as briefed, then the user
authorised execution in stages. The lane ended up lifting 7 of the 8 `stage0`
`.c` files to grade **C- or better** under `km score`, while holding two hard
invariants the whole way:

- **kaic0's emitted C for `stage1/compiler.kai` is byte-identical** to the
  pre-refactor baseline after every single commit (sha pinned in a scratch
  file, `cmp -s` gate).
- **Warning-clean** under `-std=c99 -Wall -Wextra -Wpedantic` (the stage0
  build flags), and **tier0 green** (selfhost deterministic + demos 34/34) at
  every block boundary; **tier1 full green** at the Phase 0+1 checkpoint.

Per-file outcome:

| file | before | after | grade |
|---|---|---|---|
| ast.c | A+ | A+ | — |
| runtime_llvm.c | B+ | A- | — |
| check.c | 55 F | 84 B+ | flatten only |
| main.c | C | B- | (incidental) |
| lexer.c | 46 F- | 69 C- | flatten only |
| parser.c | 28 F-- | 67 C- | flatten + split |
| parser_expr.c | (new) | 68 C- | — |
| emit.c | 31 F-- | 49 F- | flatten only; split deferred |

8 refactor commits + this retro.

## Design decisions and alternatives considered

1. **Invariant = kaic0 output, not kaic0 internals.** The strongest cheap
   check for a stage0 refactor is that the C kaic0 *emits* for a fixed input
   is unchanged. Internal reorganisation (extracting helpers, splitting
   translation units, dropping `static`) is invisible to that output at `-O0`.
   This let us move fast with high confidence and reserve the slow gate
   (tier1/selfhost) for block boundaries.

2. **enum-indexed tables over exhaustive switches** for `tk_name`/`nk_name`.
   Used designated initializers (`[N_KIND] = "..."`) so reordering the enum
   cannot desync the table, guarded by a **C99-portable static assert**
   (negative-size-array typedef). The obvious `_Static_assert` was rejected:
   it is C11 and trips `-Wpedantic` under `-std=c99`. This is the load-bearing
   portability constraint of stage0 and it bit immediately.

3. **The real km lever is per-function cognitive, not cyclomatic.** km's
   `score` weights Cognitive Complexity (SonarSource: nesting-penalised) at
   30%. Flattening the single worst function in a file (e.g. `register_top_level`
   44→12, `lex_string` 55→28, `check_node`'s logic into helpers) moved grades
   far more than any cyclomatic change. Extraction *redistributes* cyclomatic
   (lowers max, total roughly flat) but genuinely lowers cognitive because the
   penalty is for depth.

4. **Splitting a file only matters when File Size saturates.** Calibration
   (measuring sub-ranges as standalone files) proved that flattening alone
   could not lift parser.c/emit.c out of F: at ~1500–2100 LOC the File Size
   dimension caps the grade regardless of cognitive. The split into
   `parser.c` + `parser_expr.c` (sharing a private `parser_internal.h`) was
   what crossed C-. Helpers lost `static` and got prototypes in the internal
   header; they stay stage0-internal (the header is not installed).

5. **runtime.h deliberately not touched.** It is the runtime of the *entire*
   bootstrap tower — stage0/1/2, both backends (C + `runtime_llvm.c`), every
   demo/fixture/test, hundreds of `-I ../stage0` compile lines. Its F-- comes
   from File Size + Halstead Effort (volume), not real cognitive load. The
   blast radius of a split is the whole tower; the grade payoff is volume
   redistribution that does not reduce total effort. The user's call was: not
   worth it. See the km bug below.

6. **emit.c split deferred.** emit.c only crosses C- via a split, but its core
   and its declaration/TCO/top-level region share ~50 functions — a far denser
   coupling than parser.c's clean expr/decl seam. A correct split needs a wide
   `emit_internal.h` or a finer cut along the use-counter/lambda subsystem.
   Shipped the cognitive flattening (max 36→24) as independent value and left
   the split for a follow-up lane.

## Structural surprises the brief did not anticipate

- **km miscounts a one-line `#define` macro as a function** and attributes the
  cognitive of the surrounding `#ifdef` blocks (and the next real function) to
  it. On runtime.h this produced a phantom `KAI_VAR_NAME_ALLOC` with cognitive
  **1556** — 92% of the file's reported total — while the worst *real* function
  is 22. This distorts runtime.h's grade upward in penalty and hides its real
  (healthy) cognitive profile. Filed as **lnds/kimun#36** with a synthetic,
  kaikai-free repro. Lesson: read km's per-function output critically; a single
  four-figure "function" in a header full of trace macros is almost certainly
  this artifact, not real debt.

- **A clean cyclomatic number is not a clean cognitive number.** `check_node`'s
  exhaustive leaf-case `switch` keeps a high *cyclomatic* count (one per case,
  kept on purpose so a new enum value fails the build) but near-zero
  *cognitive* (a flat switch is one structure). We optimised for cognitive and
  left cyclomatic where exhaustiveness demanded it.

## Fixtures / coverage

No new fixtures: this is a pure internal refactor with a byte-identical output
contract, so the existing selfhost + demos baseline *is* the regression net —
any behavioural drift would change kaic0's emitted C and fail `cmp`/selfhost.
The coverage probe baseline is untouched (no new testable behaviour).

## Real cost vs. estimate

Larger than the "measure and stop" brief, by explicit user extension. The
expensive part was not the edits but the discipline: rebuild + byte-compare
after every extraction, tier0 at every block. The split of parser.c was the
single biggest step (moving ~830 LOC across a new translation unit + private
header) and went in clean because the expr/decl seam was genuine.

## Follow-ups for next lanes

1. **emit.c split** to lift it from F- to C- — needs `emit_internal.h` (wide,
   ~50 core fns) or a use-counter/lambda subsystem extraction. Cognitive is
   already flattened; this is the remaining lever.
2. **runtime.h** stays F-- by design until/unless a tower-wide split is judged
   worth the blast radius. Its grade will also rise on its own once
   **lnds/kimun#36** is fixed (the phantom-macro cognitive disappears).
3. **lexer.c** sits just over C- (69); `lex_string`/`lex_char`/`lex_number`
   are the remaining cognitive if a higher grade is ever wanted.
