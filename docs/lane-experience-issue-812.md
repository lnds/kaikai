# Lane experience — issue #812 (RC tracer fix; stream leak deferred to #817)

## Scope as planned vs as shipped

**Planned (issue #812):** two bundled bugs — (A) the 6 GB-RSS stream leak
in `demos/wc.kai`, and (B) the `KAI_TRACE_RC` `incref_total`/
`decref_total` counters reporting 0 despite RC ops present (every "RC
balanced" gate vacuous).

**Shipped:** **(B) only.** Finding B is fully fixed + regression-tested,
independent and C-only. Finding A (the leak) turned out to be FOUR
distinct root causes; the three I could implement cleanly all required a
frontend AST reshape that the **native backend cannot lower** — they
break tier1-native broadly (fizzbuzz, collatz, … included) and per the
"coupled reshape lands with its consumer" rule must NOT merge frontend-
alone. The whole leak (all four causes) is deferred to **#817**, to be
resolved coordinated with the native/KIR lane. #812 closes Finding B; the
issue stays open for Finding A via #817.

## Finding B — the tracer (the shipped fix)

The counters were not "lost in the runtime unification" as the issue
guessed. The asymmetry was structural and is the lesson worth pinning:

- `kai_rc_alloc_total++` / `kai_rc_alloc_by_tag[]++` sit in `kai_alloc`
  with **no `#ifdef`** — always compiled, gated only by the
  `getenv("KAI_TRACE_RC")` at the atexit report.
- `kai_rc_incref_total++` / `kai_rc_decref_total++` sat **inside `#ifdef
  KAI_TRACE_RC`** in `kai_incref` / `kai_decref`.

So a binary built by `bin/kai build` (default C backend, which does NOT
pass `-DKAI_TRACE_RC`) run with `KAI_TRACE_RC=1 ./bin`: the per-tag alloc
counters fire (always-compiled) but the incref/decref counters were never
compiled → `incref_total=0 decref_total=0` while per-tag allocs counted.
Exactly the issue's Finding B symptom. The Makefile tier1 paths add
`-DKAI_TRACE_RC=1` so they hid it; the user-facing `kai build` path
exposed it.

Fix: move the two `++` out of the `#ifdef` (keeping `kai_rc_history_log`,
which depends on the ifdef-only history machinery, guarded), in BOTH
`stage0/runtime.h` and `stage2/runtime.h`. One extra global increment per
RC op on the hot path — but it parallels the alloc counter that was
ALREADY unguarded there, so it's consistent, not new cost.

**The lesson:** when adding an RC counter, make it always-compiled
(parallel to `alloc_total`), gate ONLY at the report. A counter behind
the ifdef reads as 0 on the path users actually run. The gate convention
now requires NON-ZERO RC activity, not just balance —
`examples/perceus/rc_counters_nonzero.kai` + `test-perceus-812-tracer`
build WITHOUT the define and assert both counters > 0, so the
vacuous-zero mode is itself regression-tested (as the issue demanded).

## Finding A — the leak: four root causes, all deferred to #817 and why

With the restored tracer + `tools/symbolize-rc-trace.sh` I separated the
6 GB leak into four causes (measured ANTES on `demos/wc.kai`, ~14
leaked/line, all four contributing):

1. **State `set` clause** overwrote `self->state` without dropping the
   old accumulator (emit_c, 2-arg `resume`). C-only fix — but see below.
2. **State `get` clause** double-`__perceus_dup`'d the slot (perceus).
3. **Raw self-tail-recursive `match` scrutinee** not dropped on the goto
   edge — `emit_match_arm_raw` lacked the #309 pre-inject its boxed mirror
   `emit_match_arm` has (emit_c). C-only fix.
4. **Owned arm-binder head** in `list.filter_loop` (the dominant
   remaining one) — `kai_h` extracted with `kai_incref`, consumed on the
   keep path, dead on the drop path, so `pcs_branch_aware_skip_locals_b`'s
   consumed-on-EVERY-path guard correctly excludes it from the move-set,
   leaving it dup'd-and-orphaned → one split-word string per kept element.

### Why none of it shipped, despite three being implementable

I built all three of #1–#3 plus a per-control-path MOVE for #4. Every
gate caught a real problem that this lane could not legally fix:

- **#4 (filter binder move)** bounded the leak but is **entangled with
  TRMC's modulo-cons reuse**: stripping the cons dup disqualified one
  `filter_loop` tail leaf from TCO → `map`/`filter`/`flat_map` over 40 K
  elements in a fiber overran the 64 KiB stack (`test-issue-668`). A
  naive earlier version also broke selfhost (moved a borrow-slot binder →
  freed under the inspect-only callee → `panic: non-exhaustive match`).
  Reverted.
- **The lambda parameter exit-drop** — needed because the `stream.fold`
  CALLBACK is a lambda whose params (`acc`, `x`) leak, AND for the
  predicate closure `(w) => not is_empty(w)` — wraps every used-param
  lambda body in `EBlock([SLet(__pcs_ret, body), drop…], ret)`. The
  native/KIR backend handles that wrap for `fn` bodies but ABORTS on it in
  a CLOSURE body → broad tier1-native regression (CI: fizzbuzz, collatz,
  factorials, …). That is the exact "coupled reshape lands with its
  consumer" trap: a frontend AST reshape whose native consumer aborts must
  not merge frontend-alone. Reverted.
- **#1/#2/#3 alone** (the C-only/perceus fixes WITHOUT the lambda-drop) do
  NOT bound `wc.kai` or even a `stream.fold`: the fold accumulator flows
  through the callback lambda, which leaks without the lambda-drop. So
  shipping the C-only fixes in isolation would be a partial,
  unverifiable-as-bounded change with no clean fixture — and the
  lambda-drop they need to be effective is the thing that breaks native.

So the entire Finding-A fix is **one coupled cluster** that must land with
the native-side change. It goes to #817 as a unit: the State set/get
drops, the raw-scrutinee #309 port, the lambda-param exit-drop, and the
filter arm-binder move — all gated together on tier1-native staying green.

## Design decisions & alternatives considered

- **Ship Finding B alone vs hold #812 for the whole leak.** Chose to ship
  B now: it is independent, C-only, regression-tested, and unblocks every
  future RC-sensitive gate from passing vacuously (the gate-integrity bug
  is arguably more dangerous than the leak — it let real leaks hide). The
  leak is a coherent follow-up cluster, not a loose end.
- **Tracer: move the `++` out of the ifdef vs add `-DKAI_TRACE_RC` to
  `bin/kai`.** Chose the former — making the counter always-compiled is
  the consistent design (parallels the alloc counter) and fixes the gate
  for EVERY future binary, not just ones built with a special flag.

## Structural surprises the brief did not anticipate

- The leak was four causes, not the issue's three hypotheses; the actual
  mechanisms were emit-clause RC bugs, a lambda-param leak, and an
  arm-binder/TRMC entanglement.
- The leak fix is not separable into "safe C-only" and "risky native"
  halves: the C-only pieces are inert without the lambda-drop (the fold
  callback is a lambda), and the lambda-drop is the native-breaking AST
  reshape. It is one coupled cluster.
- `var` has TWO lowerings — `State[T]` handler (escaping/closure-captured;
  cause #1) AND an array-slot specialisation (`array_set` for simple
  `@x`/`x:=`); the array-slot `set` ALSO leaks its old value (a third
  var-lowering leak). Noted for #817.

## Fixtures added

- `examples/perceus/rc_counters_nonzero.kai` — tracer anti-vacuo (Finding
  B). Gate `test-perceus-812-tracer` builds WITHOUT `-DKAI_TRACE_RC` and
  asserts `incref_total > 0 AND decref_total > 0`. The vacuous-zero mode
  is now itself regression-tested.

(No leak fixture ships here — a bounded-memory fixture cannot pass without
the deferred coupled cluster; it lives in #817 with the fix.)

## selfhost / byte-id

The tracer fix is in `runtime.h` (pure C, no AST), so it does NOT move the
emitted C — selfhost stays deterministic, byte-id unaffected, native
unaffected. The full RC/TCO/reuse regression suite + tier1-native were the
gates that caught the leak-fix cluster's native breakage (which is now
not in this PR).

## Cost vs estimate

Far over a "two-bug" estimate. The leak fanned out to four causes; I
implemented all four (one reverted twice on selfhost + issue-668) with two
asu consults, then CI surfaced that the lambda-drop the fixes depend on
breaks native — so the entire leak fix had to be pulled to #817 as a
coupled cluster. The shipped delta is the tracer alone: small,
high-confidence, and the gate-integrity fix that makes #817's eventual
leak fix verifiable in the first place.

## Follow-ups (all in #817)

1. The Finding-A leak cluster — State set/get drops + raw-scrutinee #309
   port + lambda-param exit-drop + filter arm-binder move — landing
   together with the native/KIR lowering for the lambda-body drop wrap and
   the TRMC-coordinated filter move. Gate: bounded `KAI_TRACE_RC` +
   selfhost + ASAN + tier1-native green (no new gaps) +
   `test-issue-668`/`trmc-list-build`/`arm-top-reuse` unregressed.
2. Smaller orthogonal leaks surfaced en route: partial-sentinel
   `pump_lines` chunk-Result scrutinee; array-slot `var` `set` old-value;
   `from_list`/`list.repeat` materialised input cons spine.
