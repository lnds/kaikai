# Lane experience — issue #1127: borrow generalized to user parameters

## Scope as planned vs. as shipped

**Planned:** generalize the interprocedural borrow machinery to user
function parameters — inferred on non-`pub` functions (relax the
ultra-conservative `pcs_borrow_params`), explicit `^` on `pub` parameters
(ABI), with a stdlib HOF sweep. Two probe shapes to ~0 without annotation;
`^` parses + serializes + elides the caller dup; TCO preserved; reuse
exclusion held.

**Shipped:** the `^` surface with `pub`-ABI serialization, the stdlib HOF
sweep (17 annotations in `stdlib/core/list.kai`), the conservative inference
(the pre-existing `is_red` rule, kept), and the emit soundness gates. Two
independent oracles forced two scope calls, and the RELAXED inference was
removed entirely — the surface and ABI are what ships.

**Scope call 1 (C selfhost oracle): closure borrow deferred.** The runtime
`kai_apply` (call_ind) CONSUMES the closure it invokes (#298); no borrowing
call variant. Borrowing a CALLED closure strips the caller dup while the
callee consumes → use-after-free. The native backend hid it; the C selfhost
byte-id caught it (the self-hosted C compiler crashed "attempted to call a
non-callable value" on a compiler-internal HOF). So a closure (function-typed)
param is excluded from the effective codegen borrow set — the `^` SERIALIZES
in the ABI (ABI-ready) but codegen keeps the closure owned.

**Scope call 2 (modular selfhost oracle): relaxed inference reverted.** The
relaxed read-path inference (children-of-borrowed, borrow-through) passed
every single-TU gate (selfhost byte-id C+native, tier0, rc-detector,
tier1-asan) but the SEP-COMP `test-modular-selfhost` failed: the compiler
compiled in `--emit=c-modular` mode crashed compiling a real program
(`portfolio.kai`) with `panic: non-exhaustive match` — a UAF. Bisection was
decisive: with the relaxed inference disabled (conservative only), the modular
binary compiles portfolio cleanly. The emit is not a pure function of the
whole-program borrow map: it depends on the decl grouping and per-partition
linkage a monolithic TU only guarantees incidentally (the relaxation planted a
dup/drop whose correctness rode that incidental invariant; asu's leading
suspects are TRMC-plant-before-goto and per-partition static-ization). Per the
project tie-breaker *safety beats ergonomics* and the rule *never ship
something that breaks the selfhost*, the relaxed inference was removed from the
pass (not flagged off — dead code behind a flag lies about a supported path).
The read-path win it bought (P2 descent, which was only PARTIAL anyway,
23097→21097) is reopened as its own issue whose reopening gate is the modular
fixture in red.

**What ships, all sound in single-TU AND modular:** the `^` surface, the
`pub`-ABI serialization, the conservative inference, the stdlib `^`
annotations (inert-but-sound — closures stay owned), and the emit soundness
gates (arm-move / goto-tail / reuse-recogniser inhibited over any borrowed
scrutinee, TCO dropmask unions the borrow set). Selfhost byte-id C AND native;
modular selfhost green.

## The cause chain, verified (the brief warned soundness would be subtle)

The relaxed inference (later reverted, see above) was the easy half; the
load-bearing work was making the **native KIR lowering** sound for a borrowed
scrutinee whose arms bind children — a shape the conservative rule never
produced (its arms bind nothing used), so the native path had never exercised
it, but which an explicit `^` on a value-typed param CAN produce. Five distinct
native gaps surfaced while the relaxed inference was in the tree, each caught by
running a real program. The emit gates that close them stayed in (they are
correct for an explicit `^` borrow too); the relaxed inference that first
exposed them did not:

1. **Reuse recogniser marked reuse over a borrowed scrutinee** (the root
   cause of nearly every `unbound register` abort). `pcs_recognise_reuse_expr`
   runs BEFORE `pcs_wrap_borrow_scrutinees`, so it saw a bare `EVar(param)`
   scrutinee and marked `__perceus_reuse_cons`. The native lowering then read
   the reuse's scrutinee-drop register (`mscr`), which a borrowed match never
   declares (`ls_set_mscr` is gated on `owned`) → `KDrop(KVar(""))` → unbound.
   Fix: thread the `borrowed` set into the recogniser so a match on a borrowed
   param is not-owned and reuse is skipped (rule 5). This single fix cleared
   `hi`, `insp`, and the cons-rebuild shapes at once.

2. **Arm-move (#817) / goto-tail move over a borrowed scrutinee.** Both move
   an arm's binder assuming the scrutinee's cell is stolen by reuse. A
   borrowed scrutinee frees no cell, so moving its binder is a UAF. Fix:
   `pcs_scrut_is_borrowed` gates both passes (threaded `borrowed` through the
   whole `pcs_collect_arm_skip_*` / `pcs_gtm_*` cascade).

3. **The KIR arm-dup gated on `owned`.** `dup_variant_arm_binders` /
   `lower_list_arms` only structurally dup'd alias binders for an OWNED match.
   A bound child of a BORROWED scrutinee also flows to an owned position and
   needs its own ref (the C oracle's unconditional `is_alias` incref). Fix:
   gate the dup on `not arm_body_uses_reuse` only, not on `owned`.

4. **The TCO dropmask dropped a borrowed borrow-through param.**
   `tcrec_compute_site_dropmask` (emit_c.kai) derives its own skip-set via
   `pcs_branch_aware_skip_params`, which does NOT include the relaxed borrow
   set. So a closure `f` re-passed to its own position of a self-tail-call was
   in the dropmask (dropped) AND re-read (`p1 <- f`) → the native step freed
   then re-read `f`. Fix: union the hybrid borrow set into the tcrec skip-set
   (both branches).

5. **The kaic1 file-top `#[doc(""" """)]` trap.** The new module started with a
   triple-quoted module doc; concatenated into the bundle it broke kaic1's
   top-level signature collection, silently dropping every fn after it (the 4
   spurious "undefined name" errors). Fix: `#` header comment, not `#[doc]`.
   (Known trap, re-confirmed.)

The C oracle handled all five from the start — it was the native lowering that
had the gaps. Every gap was invisible until a real program ran on the default
(native) backend; `--emit=kir` textual output looked clean.

## Measured motivation (KAI_TRACE_RC) — and why neither win landed

| shape | before (owned) | after (this lane) | why deferred |
|---|---:|---:|---|
| closure threaded (P1) | 2000 incref, leaked=7 | unchanged (SOUND) | call_ind consumes; C selfhost UAF |
| read-only tree descent (P2) | 23097 incref | unchanged | relaxed inference UAF'd modular selfhost |
| stdlib `foldl` closure RC | dup per call | unchanged | closure `^` serialized, still consumed |

Neither RC win landed. The relaxed inference that would have collapsed P2
(measured 23097→21097 while it was in the tree — only a PARTIAL collapse
anyway) UAF'd the sep-comp compiler and was reverted; the closure borrow that
would have collapsed P1 is a call_ind-consume UAF. Both gates pin SOUNDNESS
(leaked bounded, no UAF; modular byte-id), not RC collapse. The wins are real
but each is blocked on runtime-ABI / sep-comp work and reopened as its own
issue. The safe, sound choice throughout was to keep the value owned — never a
UAF, a missed elision at worst.

## Design decisions and alternatives

- **Optimistic one-step fixpoint (asu), not a global fixpoint.** For a fn F, a
  param passed to its OWN position of a self-recursive call is assumed
  borrow-through and confirmed by the body-consistency check. Mutually
  recursive SCCs fall to owned (a perf loss, never a soundness one) — the same
  choice Koka makes without annotations.
- **Analysis conservative on owned exits (asu gate 1).** A param is
  disqualified if it flows to ANY owned position that is not self-borrow-
  through — the analysis never relies on the emit to rescue a wrong borrow,
  because the arm-move / caller-strip machinery transfers ownership at points
  an is_alias incref does not cover.
- **`pub` never inferred.** Borrow-ness is ABI; a `pub` fn borrows only its
  `^` marks. The interface serializes the bit alongside the effect row.

## Fixtures added

- `examples/perceus/borrow_closure_1127.kai` (+`.out.expected`) — P1.
- `examples/perceus/borrow_descent_1127.kai` — P2.
- `examples/perceus/borrow_stdlib_hof_1127.kai` (+`.out.expected`) — map /
  filter / foldl over one closure, both backends.
- Negative parse cases (`^` on a lambda, `^` outside param position) verified
  as parse errors inline; goldens wired in the closing commit.

## Composition with #1128 (integrator rebase)

#1128 (`fix #1126`) landed mid-lane, touching the same `perceus_decl_b` lines
(a `rewrite_skip_set` for borrow-discounted re-threads). The rebase was clean;
both changes use the `borrowed` set — mine now the larger hybrid set, which
#1128's `pcs_collect_borrow_move_last_params` excludes correctly (excludes
more, still sound). #1128's read-loop-drop fixtures stay green.

## Follow-ups

- **`call_ind_borrow` + the HOF closure win (the deferred "big one").** A
  runtime call variant that invokes a closure WITHOUT consuming it, a KIR node
  routing it, and the C runtime.h + native shim. Only then does borrowing a
  called closure become sound and the closure-threaded / HOF RC collapse. The
  compiler-internal HOF that crashed the C selfhost is the first negative
  fixture. This is a runtime-ABI change, not a perceus tweak — its own issue.
- **Collapse P2 fully:** bind a child whose only use is a borrow-through
  position borrowed (no incref), not the unconditional is_alias dup.
- **Native borrow-through element unboxing** — orthogonal, the boxing border
  lane owns it.
- **Sweep `^` into `stdlib/core/string.kai` / other HOF families** — this lane
  did `list`; the same mechanism applies.
