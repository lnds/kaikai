# Lane experience — issue #1158: pre-Perceus small-function inliner

## Scope as planned vs as shipped

**Planned** (issue #1158, koka-parity vision §Axis 3): a cheap, bounded
source-level inliner running before Perceus, so RC analysis sees through
small helpers. The stated payoff was closing the rb-tree C-backend gap
(~1.11× Koka on the fill loop): `is_red`/`balance`-class helpers were
opaque calls to Perceus, forcing conservative dup/drop at their call
boundaries.

**Shipped**: the inliner, correct and semantics-neutral, applying across
the whole program EXCEPT inside TRMC-eligible callers — where a fence
excludes it. The rb-tree gap did **not** move, and the reason is
structural, not a defect of the inliner. Details below.

The pass: post-monomorph, pre-unbox/pre-perceus, single pass over decls
in program order, no fixpoint re-inlining. Modules (all km A− or better,
each < 400 LOC):

- `inline_scan.kai` (A−, 273 LOC) — Koka-style body cost, free-vars,
  bound names, and the TRMC-caller predicate, one scope-aware walk.
- `inline_admit.kai` (A, 94 LOC) — template admission + inline census.
- `inline_paste.kai` (A+, 146 LOC) — capture-safe argument binding +
  substitution.
- `inline.kai` (A, 387 LOC) — decls driver + caller walk.

## The second structural surprise: an RC use-after-free at threshold > 1

An RC use-after-free in the paste was the hardest part of the lane. It
first showed as a self-compile SIGSEGV at threshold 20; the definitive
repro came from CI, where the existing `list_chunk_windows` fixture
segfaulted. **The bug and its fix (found via a design consult):**

```
fn chunk_loop(xs, n, out) = match xs {
  [] -> reverse(out)          # reverse is a forwarder that CONSUMES its param
  _  -> { let parts = split_at(xs, n); chunk_loop(parts.snd, n, [parts.fst, ...out]) }
}
```

Pasting `reverse(out)` let-binds the argument: `let fresh = out;
reverse_loop(fresh, [])`. That `EVar(out)` is now the only textual use of
`out` in the arm, so Perceus MOVES `out` into `fresh` — but `out` is a
parameter with a pending drop-at-exit, and that drop is the second
consumer → **double-free**. The out-of-line call `reverse(out)` worked
because crossing the call boundary made Perceus count `out` as a non-last
use (dup for the consume, keep it live for the exit-drop); the paste
collapsed that boundary and lost the dup.

**Fix (Ruta 1, one branch in `inl_bind_params`):** when the root of an
argument is a caller name with a drop-at-exit — `EVar(nm)` or
`EField(EVar(nm), …)` — wrap the let-binding's RHS in `__perceus_dup`,
the AST token Perceus itself defines for "not the last use, bump the
refcount". A fresh argument (call / literal / constructor) carries no
exit-drop, so it binds plain (wrapping it would leak). The wrap
self-corrects: if the template's callee borrows the slot, the emit strips
the dup. This speaks Perceus's own idiom rather than inserting raw RC —
the AST has no dup node the inliner could otherwise emit (`KDup` is born
later, in KIR).

Two ruled-out attempts left in as soundness improvements: literal-only
direct substitution (no variable substitutes directly) and the linearity
fence (`inl_uses_linear` — every param used exactly once). Neither fixed
the UAF (the crash-site template `reverse` is linear), but both narrow
the RC surface and stay.

Regression coverage: `list_chunk_windows` (already in the tree) is the
`EVar`-root shape; the `EField`-root shape (`f(parts.fst)` where the
template consumes the field) is the trap a "just EVar" criterion would
miss — the fence covers it via the recursive `inl_arg_needs_dup`.

## Cost-model parameters

- **Body-cost threshold**: `inl_max_cost() = 1`. The RC-UAF that
  originally blocked higher thresholds is fixed (the `__perceus_dup`
  wrap), so raising it is now low-risk — but it ships at 1 in this PR and
  the raise is deferred to its own change so the threshold bump gets a
  clean self-compile + full-fixture validation of its own rather than
  riding on the fix. Koka Core
  weights: application/allocation = 1, match = arms−1, variable/literal
  = 0.
- **Linearity fence**: `inl_uses_linear` — a template admits only when
  every parameter is used exactly once in its body (no RC recount).
- **Literal-only direct substitution**: only literal arguments
  substitute directly; every variable/expression argument let-binds to
  a fresh name so unbox/perceus treat it as an ordinary binding.
- **Saturating shapes** (cost = 1000, never admit): handlers, `!`
  propagation, intrinsics, string interpolation, holes, `todo!`,
  extern bodies, surviving `var`/`use`/index-assign statements.
- **Admission fences**: monomorphic, empty declared row, no borrowed
  (`^`) param, not self-recursive (own name in free vars), non-synthetic
  (`__`-prefixed) name.
- **Site fences**: callee resolves in the caller's namespace (same
  module origin, not shadowed by a caller local), no template free var
  shadowed at the site, saturated call only, no placeholder/`_` args.
- **Argument binding**: a plain-var or literal argument substitutes
  directly (re-evaluable, effect-free); anything else let-binds to a
  fresh site-keyed name in call order, preserving evaluation/effect
  order. Capture-safe: substitution narrows the map at every binder.

## The structural surprise the brief did not anticipate: inline ⊥ TRMC

The brief assumed inlining rb-tree's helpers would let Perceus see
through them and close the gap. It cannot, because **inline and TRMC are
mutually exclusive inside a TRMC-eligible caller under the current TRMC
lowering.**

`insert_loop` is TRMC-eligible: its arms rebuild the tree via a data
constructor in tail with the recursive self-call as an argument
(`RBNode(Red, insert_loop(l,...), kx, vx, r)` — modulo-cons). The TRMC
lowering (kir_lower_walk `body_has_trmc_step`; the `cctx_extend_linear`
/`cctx_apply_linear` spine machinery in emit_c) classifies a constructor
as part of the spine by **syntactic tail-position**, NOT by whether it
holds a self-call. So ANY inline that reshapes a TRMC caller's body
breaks it:

1. Inlining a tail-opaque helper (`balance_left`, which builds `RBNode`
   in its own tail) into a tail arm makes TRMC absorb the pasted
   constructor into the caller's `_kai_acc` spine → context closes over
   the wrong node → **infinite loop** (baseline insert_loop: 0
   `cctx_apply_linear`; with inline: 11).
2. Even inlining an INNOCENT helper — `is_red`, which returns `Bool` and
   builds nothing — into the `if` CONDITION reshapes the sibling tail
   branches: `if is_red(l) {...}` with `is_red` inlined turns `ECall`
   into `EMatch`, and TRMC then wraps the non-modulo-cons then-branch in
   `cctx_apply_linear` → same corruption (2 spurious cctx → hang).

Confirmed by experiment: at threshold 0 (walk reconstructs nodes, pastes
nothing) rb-tree terminates in 0.52s; at threshold 20 it hangs. The walk
itself is sound — the paste is what perturbs the shape.

## The fence, and why it is where it is

Two design consultations with the language architect converged on a
per-caller fence:

- First cut was tail-opacity (per-template, per-site): don't paste a
  template that builds a data constructor in its own tail into a tail
  position. Necessary but **not sufficient** — it blocks `balance_left`
  but not `is_red` in the condition, which perturbs siblings.
- Shipped: **`inl_caller_is_trmc`** — a caller is TRMC-protected if its
  ORIGINAL (pre-inline) body has some tail-position that is a data
  constructor carrying a self-recursive call (directly or via a `let`
  binder). If protected, the inliner pastes NOTHING inside it (`tmpls_for`
  hands the walk an empty template list). Evaluated on the pre-inline
  body so the predicate is stable — inlining could otherwise make a
  caller newly TRMC-eligible (circular if evaluated live). This subsumes
  the tail-opacity fence.

The fence is the correct place to solve it *for this lane*: the inliner
introduced the reshaping, and fencing it there is pure code motion,
auditable, and touches no lowering. But the underlying defect is in the
TRMC lowering: spine-absorption should verify **self-recursion identity**
("does this tail constructor contain a self-call to the caller?")
instead of classifying by syntactic form. Koka avoids this — its
`tailmod` promotes only genuine self-tail-calls by identity, so a
constructor from an inlined helper is never absorbed. That structural
fix retires the fence and is what would let inline actually help
rb-tree. It is out of scope here and wants its own issue on the TRMC
lowering.

## Honest rb-tree result

rb-tree fill @1M, C backend, best-of-5 interleaved:

| build            | median |
|------------------|--------|
| inliner + fence  | 0.617s |
| main (baseline)  | 0.616s |

**No movement** (~0.2%, within measurement noise). The fence excludes
`insert_loop`, so rb-tree is byte-for-byte the same code path as
baseline. The gap the brief targeted is untouched, and moving it needs
the TRMC-identity fix above, not the inliner.

This is not a null result for the pass overall. At the shipped
threshold 1, over rb_tree_bench (full stdlib pulled in) the census is
**7 distinct fns inlined** — `mk` (21 sites), `reverse` (21),
`bin_zero` (5), `length`, `merge_by`, `split_at`, `show_char_ascii_table`.
Each is a non-TRMC caller where the paste is safe and lets unbox/perceus
see through the former boundary. (At the blocked threshold 20 it would be
31 fns / 111 sites — `duration_to_nanos`, `bin_byte`, etc. — but that
band is gated on the RC-UAF fix.) The value is real; rb-tree simply is
not where it lands.

## Compile-time budget

rb_tree_bench `--emit=c`, best-of-3, inliner vs main baseline:

| program                          | inliner | main   | Δ      |
|----------------------------------|---------|--------|--------|
| rb_tree_bench                    | 0.342s  | 0.320s | +6.9%  |
| trmc_list_build_large            | 0.297s  | 0.281s | +5.7%  |
| balance_token_donation_1104      | 0.312s  | 0.296s | +5.4%  |

~6% at threshold 20. At the shipped threshold 1 the delta is smaller
(fewer pastes → less C emitted): rebased on the current main the
self-compiled compiler is 145345 lines and self-compile wall is ~62s.
The dominant cost is the O(decls) AST walk that reconstructs every node
(paid even when nothing inlines) plus `inl_caller_is_trmc` per DFn — the
walk is intrinsic; the paste is the cheap part.

A measurement trap worth recording: under CPU/RAM contention from a
concurrent `claude` session, `kaic2b main.kai` gave `exit=-11` (SIGSEGV,
elapsed 0s) and `124` (timeout) that looked like a miscompile but were
not — in isolation the same binary self-compiles byte-identical
(exit=0, 62s). Verify a suspected self-compile miscompile in isolation
before diagnosing; the self-compile is O(n²) in RAM and the OS starves
it under load in ways that mimic a crash.

Inlined-function census over the self-compiled compiler (threshold 1):
the pass inlines cost-0/1 templates across the ~55 compiler modules —
trivial getters, smart-constructors, and short matches. It never pastes
inside a TRMC-eligible caller (the fence) nor a non-linear template (the
linearity fence).

## Fixtures added

- `examples/perceus/inline_is_red_1158.kai` (+`.out.expected`) — an
  `is_red`-class helper pasted into a non-TRMC caller (`count_red`):
  census reports `is_red sites=1` and the emitted C's `count_red` no
  longer calls `is_red`. The acceptance-criterion inlining evidence.
  Target: `test-perceus-1158-inline`.
- `examples/perceus/inline_tail_preserved_1158.kai` (+`.out.expected`) —
  the TRMC-trap shape: `ins_1158` is TRMC-eligible with `red_1158` in
  its `if` condition; the fence keeps `ins_1158` verbatim (emitted C
  still calls `kai_red_1158`, and the run terminates) while `red_1158`
  DOES inline into the non-TRMC control caller `count_red_1158`. Proves
  the fence is per-caller, not a global veto, and that the exact hang
  shape is closed. Targets: `test-perceus-1158-inline-tail`,
  `-native`.

Coverage gap: the fixtures exercise C and native; ASAN of the fixtures
rides tier1-ASAN CI (path-gated on examples/perceus/**), not a dedicated
local target.

## Gates

- Selfhost byte-id C: **green** (compiler self-compiles byte-identical
  with the pass + fence).
- Existing TCO/TRMC + rb-tree #1104 counter fixtures intact: `test-tco`,
  `test-trmc-list-build`, `test-perceus-1104-balance-token-donation`,
  `test-perceus-1104-sibling-param-move`,
  `test-perceus-1053-nested-rotation-reuse` all OK — the fence protects
  insert_loop/balance, so reuse/donation counters are unperturbed.

## Follow-ups for next lanes

- **RC use-after-free at threshold > 1** (blocks raising the cost
  ceiling): the paste↔Perceus interaction produces a heap-UAF in the
  self-compiled compiler's parse chain. Needs `KAI_TRACE_RC`
  instrumentation to find the dup/drop mismatch — see the RC-UAF section.
  Until fixed, `inl_max_cost()` stays at 1.
- **TRMC-identity fix** (structural, separate issue on the TRMC
  lowering): spine-absorption should verify the tail constructor holds a
  self-call before absorbing it. Retires the inliner's TRMC fence and is
  the only path that lets inline help rb-tree. Any future pass that
  reshapes syntactic form before TRMC lowering (fusion, desugar
  reshaping) hits the same wall until this lands.
- The TRMC fence is conservative: it protects the ENTIRE body of a
  TRMC-eligible caller, not just the arms TRMC actually lowers. A
  finer fence (protect only the modulo-cons arms) would let more inline
  through, but only matters once the TRMC-identity fix is unavailable —
  not worth the complexity now.
- The linearity fence is also conservative: a non-linear template is
  safe to inline in many contexts; the fence is a blanket RC-soundness
  guard, not a precise one. Once the threshold>1 UAF is understood, the
  linearity fence may be relaxable.
