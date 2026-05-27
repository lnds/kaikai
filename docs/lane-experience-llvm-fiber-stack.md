# Lane experience — LLVM↔C parity: fiber stack + obsolete issue_107 skip

**Lane**: fix-fiber-stack (final lane of the autonomous overnight LLVM↔C
parity batch, 2026-05-26→27). Refs #622.
**Scope as planned**: remove the obsolete `issue_107_signal_trap` skip
(task a, trivial) and fix-or-diagnose `m8_fiber_stack_overflow` (task b,
flagged POSSIBLY STRUCTURAL). Combined in one lane to avoid a skips-file
conflict.
**Scope as shipped**: task (a) shipped (skip removed, parity confirmed).
task (b) diagnosed as structural and deferred to a new issue (#718); its
skip re-annotated with the real root cause. No compiler or runtime code
changed — the diff is `tools/backend-parity-skips.txt` + this retro.

## Task (a) — issue_107_signal_trap was stale

The skip claimed "C exits 124, LLVM exits 139 (different failure mode)".
That was false. Verified two ways:

- **Dedicated `sigharness.c`** (fork the child, wait, send SIGTERM, wait):
  both backends → harness rc=0, stdout `ready\ngot SigTerm`, 3/3 runs
  deterministic. Matches `issue_107_signal_trap.out.expected` exactly.
- **Generic parity harness** (no signal, `timeout` sends SIGTERM on
  expiry): both backends → exit 124, identical stdout. `diff -q` clean.

Fixed by #716 (the Cancel longjmp/unwind lane — Signal trap's runtime
neighbor). The skip was a bookmark for a bug that another lane closed
without removing the line. Removed.

## Task (b) — m8_fiber_stack_overflow is structural (NOT a stack-size bug)

The skip claimed "C exits 0, LLVM exits 138 (SIGBUS); genuine non-tail
fiber stack" and the brief hypothesized a fiber-stack **sizing** mismatch
(LLVM hardcoding a smaller default than C, a one-line constant fix).

**Both halves of that framing were wrong.**

### The sizing is identical

`stage0/runtime_llvm.c` does `#include "runtime.h"`, so
`kai_fiber_stack_size()` (64 KiB default, env-overridable via
`KAI_FIBER_STACK_SIZE`) is the **same symbol** on both backends. There is
no second constant to align. A first, naive reproduction with raw `cc -O0`
showed BOTH backends overflowing identically (exit 138, same diagnostic) —
which initially looked like perfect parity and nearly led to wrongly
declaring (b) a second stale skip.

### The divergence is codegen, surfaced by `-O2`

The real reproduction is via `bin/kai build` (which uses `-O2` for both
`cc` and `clang`):

```
C   (cc -O2):    50000        exit 0   — no overflow
LLVM (clang -O2): kai: fiber stack overflow at <ptr>   exit 138
```

Threshold sweep: C holds ≥50000 frames; LLVM overflows already between
N=1000 and N=2000 — a 25-50× difference in stack density per frame.

Root cause, confirmed by reading the emitted artifacts:

- **C backend** emits `static int64_t kai_deep(int64_t kair_n)` — an
  unboxed scalar signature. cc -O2 collapses each frame to ~16 bytes.
- **LLVM backend** emits `define %KaiValue* @kai_deep(%KaiValue* %p_n)`
  with a `kaix_int` heap box per operation and several `%KaiValue*` live
  per frame — frame ~25-50× larger, overflowing the same 64 KiB stack.

The unbox pass (`compiler/unbox.kai` + `fnreg.kai` `UFnSig`) runs before
emit. `emit_llvm.kai` unboxes **local expressions** only — the comment at
`emit_llvm.kai:1493` states "the unbox pass excludes function calls". The
C emitter consumes the scalar `UFnSig` at the function signature; the LLVM
emitter never does. **Function-signature unboxing is a backend feature the
C path has and the LLVM path never implemented.**

### Why deferred, not forced

"Aligning" here means implementing scalar function signatures in
`emit_llvm.kai`: read the `UFnSig`, emit `define i64 @kai_deep(i64)`,
rewrite every call-site + the closure thunk + the body to use raw scalars,
and preserve `make selfhost` byte-identity. That is a deep codegen change,
not a constant tweak — exactly the "reworking" the brief says NOT to force
at this hour. Forcing it risks the fake-green the batch forbids.

This is the architectural sibling of **#706** (LLVM lacked TCO): both are
the LLVM emitter dropping an optimization the C path has, both mis-readable
as a runtime/fiber-stack bug when the root cause is codegen. The lane
that mis-clustered the TCO gap as runtime cost the project a wrong triage;
the skip is re-annotated with the *real* cause (UFnSig not consumed at the
LLVM call boundary) precisely to prevent the next lane repeating that.

Opened **#718** (`unboxing`, `compiler` labels), framed as "emit_llvm:
consume scalar UFnSig at the call boundary", targeting Orongo — it is an
LLVM-definitive oracle-parity requirement and a direct Tier 1.2 obligation
("primitives unboxed inside fibers"). The issue notes the fix should first
verify whether `fnreg.kai` already exposes the scalar `UFnSig` (the C path
consumes it), which would narrow the work to the LLVM emit side only.

## Consultation

Consulted the **asu** language-architect agent. It confirmed:
(1) the diagnosis (UFnSig not consumed at the LLVM call boundary, not
sizing); (2) that growing the LLVM fiber stack default would be hiding the
problem — the 25-50× factor scales with frame body shape, so no constant
is safe, and it breaks oracle symmetry; (3) that signature unboxing is
legitimate Orongo work (Tier 1.2), not a deliberate omission — the C path
just reached the optimization first and LLVM inherited half of it. asu also
insisted the skip be annotated with the real cause to avoid #706-style
mis-clustering — done.

## Verification

- **issue_107**: both backends exit 0 (sigharness) / exit 124 identical
  (generic harness), stdout identical, 3/3 deterministic.
- **m8_fiber_stack**: skip retained (re-annotated, marker flipped from
  `:622:` to `:718:`).
- **Parity harness**: HEAD = `pass=357 fail=4 skip=95`; this lane =
  `pass=358 fail=4 skip=94` — one skip removed, one pass gained
  (issue_107), the SAME 4 pre-existing failures (blackjack, poker_dealer,
  issue_141_log_default = RNG/timestamp nondet; auto_install = #626
  package harness gap). **Zero new divergence introduced.**
- **selfhost**: byte-identical (no compiler/runtime code changed).
- **tier0 / tier1 / tier1-asan**: green (diff is skips-file + doc only;
  ran the gate anyway per the batch's non-negotiable rule).

## Cost vs estimate

Estimate: task (a) trivial, task (b) the real work. Reality matched, with
one trap: the naive `cc -O0` reproduction made both backends look
identical (perfect parity), which would have wrongly closed (b) as a
second stale skip. Reproducing through `bin/kai build` (the harness's
actual `-O2` path) surfaced the true divergence. **Lesson: reproduce
parity through the same compiler invocation the harness uses, not a
hand-rolled `cc`** — optimization level changes the observed contract.

## Parity batch — final state

This is the last lane of the autonomous overnight LLVM↔C parity batch.
After this lane, the `#622`/`#618` parity-bug section of
`tools/backend-parity-skips.txt` contains **one** entry:
`m8_fiber_stack_overflow` (now marked `:718:`, diagnosed + deferred). Every
other #622/#618 bug shipped a fix across the batch (clusters A/B/D/E/F +
the TCO/Cancel/process/variant lanes). The remaining harness skips are
`#626`/`#357` suite-categorization (negative tests, aspirational demos,
library files, multi-package fixtures) and one `exempt-nondet`
(rb_tree_bench wall-clock). The 4 live parity-harness failures
(blackjack/poker_dealer/issue_141/auto_install) are nondet + harness-gap,
not tracked #622 bugs — candidates for the eric-proposed split into
`parity-bugs.txt` vs `parity-harness-skips.txt`.

## Follow-ups left for next lanes

- **#718** — implement scalar function-signature unboxing in `emit_llvm`.
  The one real #622 parity bug remaining; closing it empties the section
  before Orongo.
- The 4 live nondet/harness failures want categorization (move
  blackjack/poker_dealer/issue_141 to `exempt-nondet`, auto_install to
  `#626`) — out of this lane's scope, but the parity harness will keep
  reporting them as red until then.
