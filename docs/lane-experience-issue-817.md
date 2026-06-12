# Lane experience — issue #817 (filter/map/flat_map over-heap arm-binder leak)

## Scope as planned vs as shipped

**Planned (issue #817):** the general Perceus leak in `list.filter` /
`map` / `flat_map` over heap elements — one heap value leaked per kept
element, linear in the input. The 4th and dominant cause left out of #816
(the `demos/wc.kai` 6 GB stream-fold leak). The issue framed it as a
four-cause cluster that "cannot ship frontend-alone" because the
lambda-param exit-drop reshape breaks the native/KIR backend (the
"coupled reshape lands with its consumer" rule).

**Shipped:** the DOMINANT causes are closed, native-safe — the issue's
"inseparable cluster" framing was wrong about the bulk: the arm-binder and
scrutinee leaks are emit_c/perceus-pure and ship alone. The issue WAS
right about one residual piece (the lambda-param wrap) being
native-coupled — that one is deferred. Two coordinated frontend fixes plus
a runtime fix the leak fix exposed:

1. **#3 — raw self-tail-recursive `match` scrutinee drop** (emit_c).
   `emit_match_arm_raw` lacked the #309 `tcrec_tail_always_sentinel →
   kai_decref(_scr)` pre-inject its boxed mirror `emit_match_arm` has,
   GATED to the owned branch (`scr_owned`) so the borrowed branch never
   double-frees. Closes the `split + length`-shaped leak (`free_total=1`
   → fully bounded). Dominates the str-leak.
2. **lambda-param exit-drop — ATTEMPTED, then DEFERRED to the native
   lane.** The lambda's own params leak (dup'd-and-never-dropped:
   `(w) => not is_empty(w)` leaks one `w` per call). The fix gives them
   the `pcs_prepend_unused_drops` discipline a top-level `fn` gets — but
   that produces the `EBlock([SLet(__pcs_ret, body), drop], __pcs_ret)`
   wrap, and tier1-native CONFIRMED the issue's warning: the native/KIR
   backend lowers that wrap for `fn` bodies but NOT for CLOSURE bodies. It
   added native-parity gaps (huffman/weather/go/python/scala/ffi) and the
   `test-kir closures` golden changed (`ret t0` → the `__pcs_ret` wrap).
   This is the exact "coupled reshape lands with its consumer" piece. The
   attempt was reverted; the lambda-param residual goes to the native/KIR
   lane WITH the closure-body drop lowering. The leak is identical with a
   NAMED `fn` predicate (no lambda), so this is a residual, not the
   dominant slice — the #3 + #4 fixes (the dominant slices) ship here and
   are native-safe (tier1-native passes). The fixture uses a named `fn`.
3. **#4 — the arm-binder MOVE-at-last-use** (perceus). The root cause was
   NOT a missing drop but a SPURIOUS dup: `pcs_max_paths_b_expr` for `EIf`
   is `cond + max(then, else)`, so `filter_loop`'s `h` (read in `p(h)` +
   the cons) counts 2 → `pcs_is_non_last`'s `count >= 2` dup'd EVERY read
   including the last → the birth `kai_incref` orphaned. Fix: move the
   LAST use (the cons consumes the birth ref, no orphan), keep the
   predicate read dup'd (the closure could retain its copy); move the tail
   `t` on every use (read once per branch → safe); drop the dead-branch
   `h` per-branch. Restricted by a cons-cascade-safety gate (see below).
4. **Iterative cons-spine free** (runtime, both `stage0` + `stage2`
   `runtime.h`). The frontend fix EXPOSED a pre-existing runtime bug:
   `kai_free_value` freed the cons tail RECURSIVELY, so once the spine
   stopped leaking, a 40K list in a 64 KiB fiber overflowed the stack in
   `kai_free_value → kai_decref → kai_free_value → …` (`test-issue-668`
   bus error). Made the unique-tail spine free iterative
   (`kai_free_cons_spine`), reusing a factored `KAI_RECYCLE_CELL` macro so
   the counter/trace/pool discipline stays intact.

## RSS — before vs after

The issue's minimal repro (NO streams): a loop running
`string.split(line, " ") |? (w) => not is_empty(w) |> list.length`.

| input         | before (HEAD) | after          |
|---------------|---------------|----------------|
| 200K iters    | 442 MB        | **1.72 MB**    |
| 2M iters (10×)| ~3.0 GB       | **1.72 MB**    |

Bounded, independent of input — the success criterion. `KAI_TRACE_RC`
`leaked` is a small constant (9–12) at any iteration count (was 8.2M at
200K).

`demos/wc.kai` (the bench): 48 MB input → 174 MB, 96 MB → 346 MB. The
arm-binder + lambda + scrutinee leaks (the #4 cluster, ~6 GB) are closed;
the residual that still scales is the **orthogonal stream leaks the issue
lists separately** (partial-sentinel `pump_lines` chunk-Result scrutinee,
`from_list`/`read_lines` materialised cons spine) — out of scope for #4.
The filter-over-heap component no longer contributes (the named-`fn`
filter repro is `leaked=10` constant).

## Design decisions & alternatives considered

- **Separating the cluster vs the issue's "inseparable" framing.** The
  issue (and #816's retro) said the four causes are one coupled cluster
  that must land with the native lambda-body wrap. Empirical isolation
  with the fixed #816 tracer disproved this: micro-repros with a NAMED
  `fn` predicate (zero lambdas) leaked the dominant slice, so the bulk is
  emit_c/perceus-pure and ships without touching native. The retro had
  fused "missing spine drop" (dominant) with "lambda-body param drop"
  (residual). Decision: ship the separable bulk; the lambda-param fix
  lands via the native-safe `pcs_prepend_unused_drops`, not the
  native-breaking `EBlock(__pcs_ret)` wrap on a CLOSURE body.

- **#4: move-at-last-use vs drop-the-orphan.** Both bound the leak, but
  the move is strictly better (no incref/decref pair that cancels) AND
  dissolves the AST-order problem: with no orphan there is no drop to
  order relative to the goto. The drop-in-the-goto approach (emit_c
  `tcrec_emit_goto`) was built first and reverted — its syntactic filter
  (`tcrec_binder_moved_in_args`) mis-classified `pae_variants`' nested
  `match v` (a sibling consumed in an inner-match scrutinee, not in the
  self-call args) and double-freed under selfhost.

- **The cons-cascade-safety gate.** The move's hardest trap: the #3
  `kai_decref(_scr)` frees the matched cons cell when its rc hits 1 and
  cascades `decref` to ALL slots. Moving a binder strips the dup that kept
  the cell's rc above 1, so the cascade fires — and if a SIBLING binder is
  consumed OUTSIDE the self-call (a leading `SLet` / inner-match, the
  `vpa_sig_params` `match p.ptype` shape), the cascade double-consumes it
  and the inner-match reads it after → UAF. Gate: a goto-tail arm is
  move-safe only when its body flows STRAIGHT to the self-call (no leading
  block statements). `filter`/`map`/`flat_map`/`reverse` all pass;
  `vpa_sig_params` (a leading `let e1 = match p.ptype`) is excluded. This
  is `pcs_cons_cascade_safe` + the `EBlock` non-empty-stmts rejection in
  `pcs_arm_all_tails_self_call`, applied to BOTH the move-all set
  (`goto_moves`) and the move-at-last set (`move_last_set`, intersected
  with `pcs_collect_safe_arm_binders`).

- **Two move mechanisms, not one.** `h` (read in the cond + the cons, max-
  path 2) needs move-at-LAST-use only (moving the cond read too is unsound
  — the opaque predicate could retain `h`). `t` (read once per branch,
  max-path 1) is move-on-EVERY-use (the skip-set discipline). They coexist
  cleanly: `t` goes in `skip_set` via `goto_moves`; `h` in `move_last_set`;
  the `pcs_strs_minus(move_last_set, skip_set)` keeps them disjoint.

- **Iterative free: separate fn + factored macro vs inline trampoline.**
  Chose `kai_free_cons_spine` (separate) over a `goto restart` in
  `kai_free_value` — the latter re-enters a 200-line switch per spine cell
  and spikes cognitive complexity. The recycle/counter tail is factored
  into `KAI_RECYCLE_CELL` (used by both the spine loop and the non-cons
  cases), so duplication goes DOWN, not up. The per-spine-cell
  `kai_rc_decref_total++` + history log keep the trace byte-identical to
  the recursive version.

## Structural surprises the brief did not anticipate

- **The leak is NOT native-coupled.** The brief's central premise (and
  #816's) was that the whole cluster must land with the native/KIR
  lambda-body wrap. The dominant causes are emit_c/perceus-pure; only a
  residual lambda-param slice would have needed the wrap, and even that
  was done native-safely. The "coupled reshape" rule did not bind here.

- **The runtime exposed a latent bug.** Closing the spine leak turned a
  silent leak into a deep recursive free that overflowed the fiber stack
  — a problem that only surfaces ONCE the lists are actually freed. The
  fix is not optional: without it, #4's frontend fix regresses
  `test-issue-668`.

- **`pcs_max_paths` counts `EIf` as `cond + max(branches)`.** This is the
  exact line that makes `h` look multi-use (cond `p(h)` + branch cons) and
  forces the spurious dup. The fix targets the consequence (move the last
  use) rather than the count, because the count is correct for the dup
  decision on the cond read.

## Fixtures added

- `examples/perceus/filter_heap_bounded_817.kai` + gate
  `test-perceus-817-filter-leak` (wired into the tier1 light targets):
  filters over freshly-split heap strings 5000×, asserts `leaked < 1000`
  built WITHOUT `-DKAI_TRACE_RC`. With the leak it grows ~14/iter (≈70000);
  fixed it is `leaked=12`. Also exercises the iterative cons-spine free
  over the long result lists.
- `test-issue-668` (pre-existing) now gates the runtime iterative free —
  it was the bus-error canary for the recursive-free regression.

Coverage gap: no dedicated shared-tail (rc>1) fixture in the repo yet; the
shared-tail soundness was verified manually (a list with two owners; one
dropped, the other survives; ASAN clean). `test-issue-82-leak-audit` and
the rb-tree reuse fixtures exercise shared structure under free indirectly.

## selfhost / byte-id

Byte-id is FALSE-GREEN for the leak (the compiler does not run
filter-over-heap in the leaking shape), so the real gates were
`test-perceus-817-filter-leak` (KAI_TRACE_RC `leaked` bound) + ASAN +
`test-issue-668`. The emitted C DID change legitimately (the move strips a
dup, the #3 fix adds a decref, the iterative free changes `kai_free_value`)
— selfhost stays DETERMINISTIC (`kaic2b.c == kaic2c.c`), which is the gate
that matters; the byte stream moved because the compiler's own
self-tail-recursive arms now move their binders where safe. selfhost was
the load-bearing gate: it caught the `vpa_sig_params` cons-cascade UAF
FOUR times before the gate converged (the compiler's `modules.kai` carries
the exact `[p, ...rest] -> { let e1 = match p.field; recur }` shape).

## Cost vs estimate

Far over a "one frontend fix" estimate. The #4 move went through six
reverted approaches (emit_c drop with syntactic filter → double-free;
per-branch drop in perceus → UAF from arg-dup ordering; unconditional
skip-set move → UAF on the predicate read; move-at-last-use → exposed the
`t` residual; goto-tail move-all → the `vpa_sig_params` cascade UAF; and
finally the cascade-safety gate). Each reverted approach was caught by
selfhost or ASAN, never shipped. The asu architect consults were
load-bearing — the "spurious dup, not missing drop" reframe and the
cons-cascade model were the two insights that converged it. The runtime
iterative free was an unplanned but mandatory follow-on.

## Code review (linus) — fixes applied before PR

- **`ELambda` descent in the goto-tail collectors** (`pcs_gtm_kind`,
  `pcs_sab_kind`): both descended into lambda bodies with the ENCLOSING
  fn's `self_name`, which could misclassify a capture as a goto-tail
  binder. Inert today (the rewriter zeroes `move_last_set` on lambda
  entry), but a logic error — changed to `ELambda(_, _) -> []` to match
  `pcs_atsc_kind`.
- **`EModCall` self-call** in `pcs_atsc_kind` / `pcs_pisc_kind`: a
  module-qualified self-call (`EModCall("list", "filter_loop")`, the
  resolved shape) fell through to `false`, so the move never fired for
  resolved fns. Added the `EModCall(_, c) -> c == self_name` arm (mirror
  of emit_c's `tcrec_tail_always_sentinel`).
- **Cross-arm name collision** in `pcs_collect_move_last_set`'s flat
  `uses`: documented as benign (the `(line,col)` identity gate makes a
  false move impossible; worst case is a missed move) rather than
  restructured per-arm — proportional to the bounded risk.

## Follow-ups left for next lanes

1. The orthogonal `demos/wc.kai` stream leaks (still listed open in #817's
   body): partial-sentinel `pump_lines` chunk-Result scrutinee;
   `from_list`/`read_lines` materialised input cons spine; array-slot
   `var` `set` old-value. These keep `wc.kai` scaling with input; they are
   NOT the arm-binder cluster and want their own lane.
2. A dedicated shared-tail (rc>1) free fixture — the iterative spine free's
   boundary case is currently only manually verified.
3. The cascade-safety gate rejects ANY goto-tail arm with a leading block
   statement (the `vpa_sig_params` shape). A finer gate — reject only when
   a SIBLING is actually consumed before the tail — would recover a few
   move opportunities (a leading `let` that touches no sibling is safe).
   Left coarse-but-safe for this lane; the recovered moves are minor.
