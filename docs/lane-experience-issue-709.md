# Lane experience — issue #709: LLVM TCO accumulator leak

**Scope:** the LLVM TCO back-edge landed in #706 (PR #708) overwrote a
param's alloca slot without dropping the outgoing boxed value, leaking
one box per iteration for any self-tail-recursive loop with an
arithmetic accumulator. `loop(50M, 0)` ran in ~2.4 GB RSS under LLVM vs
~1.5 MB under C. The fix restores bounded memory. One-file change in
`stage2/compiler/emit_llvm.kai` plus a regression fixture + Makefile
target.

**Layer:** codegen consumer (`emit_llvm.kai`). The shared tcrec analysis
(`tcrec_*` in `emit_c.kai`) and the dropmask were **not** touched — the
fix is LLVM-back-edge-local, which the selfhost byte-identity gate
proves.

## Root cause — the dropmask is the C *boxed* model, but `loop` is a UFn

The subtle part the issue's hypothesis under-specified: the leak is not
"drop the outgoing slot value for every overwritten param" (that
double-frees the consumed-param case). The real divergence is **UFn**.

In C, a function whose params are all unboxable scalars (`Int`, `Real`,
`Bool`, `Char`) is classified as a *UFn* (`classify_unbox_sig`,
fnreg.kai): its params become raw `int64_t`/`double` with **no
refcount**. `tcrec_emit_goto` (emit_c.kai:9770) has two branches:

- `Some(US(...))` (UFn): evaluate args as raw scalars, **emit ZERO
  drops** — raw scalars carry no RC. The shared dropmask is simply
  *discarded*.
- `None` (boxed): evaluate args boxed, emit the dropmask drops.

`loop(n, acc)` is a UFn — C emits `kai_loop(int64_t kair_n, int64_t
kair_acc)` and zero drops, so C never leaks regardless of the dropmask.

The LLVM backend has **no raw-scalar param path** — every param is boxed
`%KaiValue*` (lane-706 retro, §"no UFn param path"). The body reads each
param via *borrow-readers* (`kaix_to_int`, which reads `v->as.i` without
decref) — so at the back-edge every reloaded `%p_<name>` is still live.
But `llvm_emit_tcrec_goto` applied the C *boxed-model* dropmask, which
for `loop` is `{n}` only (`acc` has a single branch-local use, so the
branch-aware analysis does not mark it). Result: `n`'s slot value gets
dropped, `acc`'s does not → one boxed `Int` leaked per iteration.

Why `acc` and not `n`? The dropmask was computed for the boxed model
where `acc`'s single use transfers ownership; in LLVM's borrow-read path
nothing is transferred. The dropmask is correct for C (which discards it
anyway for this UFn) and wrong for LLVM (which has no raw path and must
drop *all* the live boxed slots).

## The fix — reconcile to a SINGLE drop site, UFn-gated

`llvm_emit_tcrec_goto` now looks the callee up in the UFn registry via
the sentinel's `c_sym` (previously discarded):

- **UFn callee** → `llvm_emit_tcrec_drops_all`: drop **every** param slot
  value (`%p_<name>`). The borrow-read body consumed none of them, so
  each is live and dropped exactly once. This **subsumes** the dropmask
  — it does not stack on it.
- **non-UFn callee** → the existing `llvm_emit_tcrec_drops(dropmask)`,
  verbatim. Those bodies use the classical boxed dup/drop discipline the
  dropmask was computed for; a param consumed in the args (e.g.
  `kaix_add(%p_acc, …)` in a list-fold) is already dead and must NOT be
  dropped again.

A local mirror `llvm_lookup_ufn_by_csym` (≈7 lines) avoids importing
`emit_c.kai` into `emit_llvm.kai` — it uses only `c_sym` (emit_shared)
and `EFn`/`UFnSig` (fnreg), both already imported. Downward glue only,
per the module-extraction discipline.

### Why exactly one drop, no double-free

At the loop head, `%p_<name>` is loaded from the slot (one ref). A UFn
body only borrow-reads its params, so `%p_<name>` is never consumed; the
slot still holds that one ref at the back-edge. Dropping `%p_<name>`
once balances it; the new `store` rebinds the slot to a fresh value.
Exactly one drop per param per iteration. For `n`, this replaces the
identical dropmask drop (same value, same count); for `acc`, it adds the
missing drop. No path drops a value twice. Verified empirically: `loop`
runs 50M iterations under LLVM with no crash (a double-free would
SIGSEGV/abort) and bounded RSS.

## Verification — the trace check, not ASAN

The #706 lane passed ASAN + selfhost and **still shipped this leak**,
because (a) selfhost loops are bounded, so a per-iteration leak does not
exhaust memory, and (b) macOS ASAN runs `detect_leaks=0`. A leak is not
a use-after-free; ASAN is necessary (it catches the double-free a wrong
drop count would cause) but **not sufficient** for this bug.

The real gate is `KAI_TRACE_RC`'s `leaked=` line at two iteration
counts:

| case                          | N=1000 leaked | N=100000 leaked |
|-------------------------------|---------------|-----------------|
| accumulator, **before** fix   | 877           | 99877 (∝ N)     |
| accumulator, **after** fix    | 5             | 5 (constant)    |
| single-arg `loop(n-1)`        | 1             | 1               |
| passthrough `loop(n-1, acc)`  | 1             | 1               |
| list `drain` (non-UFn)        | 6             | 6 (unchanged)   |

The residual `leaked=5` is constant setup allocation, independent of
iterations — the lane closes the *linear* leak, which is the regression.
RSS at 50M iters: **1.75 MB** (was ~2.4 GB); C oracle 1.5 MB.

Fixture `examples/perceus/tco_accumulator_no_leak.kai` (`loop(1000,0)` →
golden `500500`) + Makefile target `test-tco-llvm-709` (stage2). The
target is the first in the tree to **assert a bounded leak count**: it
recompiles the fixture's loop at N=1000 and N=100000 and asserts the
`KAI_TRACE_RC` `leaked` values are equal. No prior harness pattern
existed for leak-count assertions — the existing perceus-asan recipes
all run `detect_leaks=0`. Wired into `test`, `test-fast`, and `.PHONY`
alongside `test-tco-llvm-706`.

## Gates

- `make selfhost`: `kaic2b.c == kaic2c.c` byte-identical — proves the C
  path (and the shared analysis) is untouched.
- `make test-tco-llvm-709`: OK (bounded leak).
- Full TCO + perceus battery (`test-tco`, `-regression`, `-zero-arg`,
  `-309`, `-perceus-wrap`, `-llvm-706`, `-rule3-*`, `-issue-680`,
  `perceus-issue703`, `rc-trace-callsite`): all green.
- `tools/test-backend-parity.sh`: no new divergence.

## Out of scope / follow-ups

- **Non-UFn list-fold leak.** `drain([Int], acc)` leaks 6 boxes for 5
  elements with `free_total=0` — the back-edge never drops the consumed
  cons cells. This is a **pre-existing** LLVM RC gap (present before
  #709, unchanged by this fix), independent of the UFn accumulator. The
  non-UFn path here mirrors the C dropmask, which itself documents a
  bounded R6 leak (issue #43/#92). Not opened as a new issue without
  authorization; flagged here with the repro (`leaked=6, free_total=0`).
- **Gap 1 — LLVM Int unboxing.** LLVM boxes every Int in loop bodies
  (alloc_total ∝ N, but freed). This is the known typed-construction
  carve-out (#383/#440 landed for C only); allocating-but-freeing is
  wasteful, not a leak. Separate mechanical lane.

## Cost vs estimate

Estimated a "subtle RC-arithmetic" lane. The implementation is ~50 lines
(two helpers + a 3-line branch + a comment block + the sentinel
plumbing). The cost was entirely in **diagnosis**: the issue's
"drop the outgoing slot value" hypothesis is the right *mechanism* but
the wrong *predicate* — applied universally it double-frees the consumed
list-fold case. Reading `tcrec_emit_goto`'s two-branch UFn structure in
emit_c.kai reframed it: the correct predicate is "is the callee a UFn",
which the C path already computes and discards. Real cost ≈ estimate,
front-loaded into reading the C analysis rather than writing LLVM IR.
