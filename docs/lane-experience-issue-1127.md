# Lane experience — issue #1127: borrow generalized to user parameters

## Scope as planned vs. as shipped

**Planned:** generalize the interprocedural borrow machinery to user
function parameters — inferred on non-`pub` functions (relax the
ultra-conservative `pcs_borrow_params`), explicit `^` on `pub` parameters
(ABI), with a stdlib HOF sweep. Two probe shapes to ~0 without annotation;
`^` parses + serializes + elides the caller dup; TCO preserved; reuse
exclusion held.

**Shipped:** the full hybrid model (inferred private + explicit `^` public),
the `^` surface with `pub`-ABI serialization, the stdlib HOF sweep (17
annotations in `stdlib/core/list.kai`), and the soundness gates. Both
backends (C oracle + native) compile and run all shapes, selfhost byte-id C
AND native.

**Scope call, verified by the C selfhost oracle: closure borrow is deferred.**
The runtime `kai_apply` (call_ind) CONSUMES the closure it invokes (issue
#298), and there is no borrowing call variant. Borrowing a CALLED closure
would strip the caller dup while the callee still consumes → a
use-after-free. The native backend hid this; the C selfhost byte-id destroyed
it — the self-hosted C compiler crashed with `attempted to call a
non-callable value` when a compiler-internal HOF's borrowed closure was freed
before a call. So a CLOSURE param (function type) is excluded from the
EFFECTIVE (codegen) borrow set: the `^` still SERIALIZES in the ABI
(ABI-ready), but codegen keeps the closure owned. The design's "big one" —
the HOF closure win, the RC collapse of the closure-threaded loop — is gated
on a borrowing call variant (`call_ind_borrow`), tracked as a follow-up. What
DID ship: the read-path (non-closure) borrow, inferred and sound; the surface
and ABI; and the stdlib `^` annotations, inert-but-sound.

## The cause chain, verified (the brief warned soundness would be subtle)

The inference was the easy half; the load-bearing work was making the
**native KIR lowering** sound for a borrowed scrutinee whose arms bind
children. The conservative rule never bound a used child under a borrowed
scrutinee, so the native path had never exercised it. Five distinct native
gaps surfaced, each caught by running a real program (not by the inference):

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

## Measured motivation (KAI_TRACE_RC)

| shape | before (owned) | after (this lane) |
|---|---:|---:|
| closure threaded (P1) — closure borrow deferred | 2000 incref, leaked=7 | 2000 incref, leaked=7 (SOUND, no collapse) |
| read-only tree descent (P2), native | 23097 incref | 21097 |
| stdlib `foldl` over 10k, non-closure RC | — | closure `^` serialized, still consumed |

P1 (the closure-threaded loop, the design's "big one") does NOT collapse: the
closure stays owned because call_ind consumes it (see the scope call above).
The gate pins soundness — `leaked` stays bounded, no UAF — not the collapse.
P2 (read-descent over a value type, no closure) improves partially: the
container re-thread is elided, but the per-level dup of a bound child that
flows to a borrow-through position is NOT. Collapsing P2 fully needs a bound
child whose ONLY use is a borrow-through position to bind borrowed (no incref)
rather than the current unconditional is_alias dup — a finer analysis than the
one-step fixpoint this lane shipped, and a follow-up. The safe, sound choice
here was to dup the child (never a UAF, a missed elision at worst).

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
