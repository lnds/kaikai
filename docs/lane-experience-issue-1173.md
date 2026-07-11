# Lane experience — issue #1173 (var→slot after the inliner)

## Scope as planned vs as shipped

The issue (and the lane brief) called for a **new** KIR-level var→slot
analysis running after `kir_inline_program`, re-evaluating the front-end
escape decision on the inlined KIR. What shipped is a **one-function
relaxation of an existing pass** plus two fixtures: the analysis the issue
asked for already exists — `kvs_program` (kir_varslot.kai, #1158) runs as
the last step of `kir_inline_program` and already demotes canonical
non-escaping var-cell State handlers to slots. The reason it did not fire
on the issue's benchmark was one admission gate upstream: the dead-closure
sweep in kir_inline_beta.kai refused to remove a dead closure unless
**every** capture was an `__alias_id__*` register. The `while { i < n }`
predicate closure captures the loop bound `n` (a boxed param) alongside
the counter's alias id, so the swept-everywhere-else closure survived —
dead, never called, but still holding the alias — and `kvs_program`
correctly refused to demote a cell whose alias is still read.

This is why the #1158 fixture (`while { i < 100000 }`, constant bound,
alias-only capture) passed while the issue's shape (bound in a param) sat
at 8–27× a tail-recursive loop.

## The load-bearing discovery

The sweep's capture restriction rested on a premise its own comment
stated: "the build consumed one ref per capture, so removal orphans
capture refs". The premise is **false**. `kai_closure` (and its native
forwarder `kaix_closure`) increfs every capture itself and the closure's
free decrefs them — the build *borrows* its capture atoms. The KIR
confirms it: there is no `dup n` before the `KClosure` build even though
`n` is read afterwards, and the caller's final `drop n` is still present;
that accounting only balances if the build does not consume. So removing
a dead closure build together with the register's own dup/drops is
RC-neutral for every capture, whatever its kind. The fix deletes the
restriction (and the two helper fns that implemented it) and rewrites the
two comments to state the true invariant.

Verified before believing: read `kai_closure` in stage0/runtime.h (the
m5.x #2 comment block documents the incref-inside contract explicitly),
traced the repro's KIR ref accounting by hand, and re-ran the RC ledger
(`KAI_TRACE_RC`) on both fixtures — leak floor identical to a pure
tail-recursive program.

## Soundness argument (why no new gate was needed)

The lane brief demanded a handler-escape / multi-shot gate for the
demotion. That gate already exists and is untouched: `kvs_program` only
demotes a handler that (a) is the desugar-minted canonical get/set pair
(single-shot resume by construction), (b) has no unaliased State perform
in the fn, and (c) whose alias register is never read as an atom. The
sweep change only affects condition (c)'s inputs: a closure that is
*genuinely* dead (its only uses are its own dup/drop RC ops — nothing
calls, stores, passes or returns it) can never perform the cell's ops, so
removing it cannot widen what kvs admits beyond what is sound. A closure
with any real use — including being stored in a data structure or passed
to a call — keeps the alias read and the cell stays a dispatched State
handler; the negative fixture pins exactly that.

## Results (native, mac arm64, best-of-5 interleaved, N=50M LCG)

| form | before | after |
|---|---|---|
| `while` with `var` counters, bound in a param | 1.17 s | 0.32 s |
| self-tail-rec (state as params) | 0.14 s | 0.14 s |
| ratio | 8.4× | **2.3×** |

(The issue quotes 27×; that measurement excluded process startup and ran
on a different machine state. Same-harness interleaved runs give 8.4× →
2.3×.) The post-inline KIR of the loop fn carries zero `install State` /
`perform State` and both counters live in `__vcell_` registers. The
residual 2.3× is no longer dispatch: it is boxed-cell traffic (dup/drop
per read, `int.box` per constant, boxed arithmetic) vs the tail loop's
raw i64 registers — a separate lever (raw-slot vcells) left as follow-up.

rb-tree native did not regress (baseline 1.36×/C, with fix 1.34×/C, same
harness same session — #1164's win is intact). C emit is byte-identical
on a 6-program corpus (stash/rebuild/diff), as expected: the modified
code is reachable only from the native lowering path.

## Structural surprises

- The brief's mental model ("the decision is never re-evaluated") was one
  pass out of date: #1158 already shipped the re-evaluation; the bug was
  an admission gate, not a missing pass. Reading `kir_inline.kai:57`
  before designing saved a redundant module.
- A dead closure is not free even when RC-cheap: pre-fix, the surviving
  predicate closure cost a dup+drop pair per loop iteration on top of
  blocking the demotion.
- `kaic2-native` smoke: run it from the repo root. From `stage2/` the
  stdlib core-path resolution loops and the compile appears to hang —
  reproduced, not a regression (the selfhost gate runs from root).

## Fixtures

- `examples/perceus/kir_while_param_slot_1173.kai` (positive): while with
  the bound in a param; gate greps the KIR for zero State install/perform
  and `__vcell_` present, plus output golden. `test-kir-while-param-slot-1173`.
- `examples/perceus/var_escapes_stays_state_1173.kai` (negative): var
  captured by a closure stored in a list and called through a pattern
  binding — survives the inliner, must keep `install State`. Output golden
  checks the two-call mutation order. `test-kir-var-escape-state-1173`.
- Both wired into `.github/workflows/tier1-native.yml` (native-only pass,
  native-only tier), alongside the #1158 steps.

Coverage gap left open: no fixture pins the *borrowing* capture semantics
of `KClosure` directly (a unit-shaped KIR golden would); today it is
pinned indirectly by ASAN + the RC ledger over the sweep-affected corpus.

## Cost vs estimate

Well under the brief's implied scope: no new module, net −13 lines of
compiler code. Most of the lane was verification (runtime RC semantics,
baseline benches, stash/rebuild byte-identity, selfhost ×2, parity).

## Follow-ups

- Raw-slot vcells: demote `SBoxed` `__vcell_` registers holding ints to
  `SInt64` slots to close the residual 2.3× (needs the cell's type, the
  same rawness-verdict machinery as #1110).
- The `and`-strictness exponential hang attributed to kaic2-native in the
  #1158 retro did not reproduce here; the "hang" this lane hit was the
  cwd trap above. Worth re-checking that attribution next time the smoke
  runs.
