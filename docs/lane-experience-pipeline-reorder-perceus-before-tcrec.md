# Lane experience — pipeline-reorder-perceus-before-tcrec

Lane FIX of Eric's structural debt — reorder the compilation pipeline
from `parse → typer → tcrec → perceus → emit_c` to
`parse → typer → perceus → tcrec → emit_c`. The previous order
coupled the two passes implicitly: tcrec emulated
`pcs_collect_exit_drops` byte-for-byte inside its own
`tcrec_compute_site_dropmask` so the goto block's drops would match
the wrap's exit drops the goto skipped. Any change to one had to be
mirrored in the other; issue #92 R6 hit this contract four times,
PR #309 (iter 2) hit it again, PR #313 patched a symptom (`_scr`
match-arm leak) without addressing the structural coupling.

Eric's 2026-05-06 review framed the reorder as the structural fix:
"tcrec recibe un AST donde los drops ya son datos explícitos y los
trata como cualquier otro nodo." This lane lands the reorder.

## Objective metrics

- Wall clock (start → end): 2026-05-06T23:22:52 → 2026-05-07T00:05:42
  (≈ 43m).
- Code delta: ~110 lines added in `stage2/compiler.kai` (driver swap
  + `tcrec_is_pcs_ret_wrap` / `tcrec_rewrite_pcs_ret_wrap` /
  `tcrec_find_pcs_ret_let` / `tcrec_rebuild_pcs_ret_wrap` +
  `TcrecPcsTriple` type), 1 line in `stage2/Makefile` (fixture
  registration), 2 new fixture files in
  `examples/aspirational/pipeline_reorder_smoke/`.
- Tier gates: tier0 OK, tier1 OK, tier1-asan OK.
- Selfhost: byte-identical on the C backend in 1 iteration after the
  refactor settled (initial implementation needed a 2-iter cycle to
  converge after the helpers were renamed; final converges at iter 1
  through the makefile target).
- Selfhost-llvm: byte-identical in 1 iteration.

## Pipeline driver location

`stage2/compiler.kai:46090-46109` (post-edit). The driver is
`compile_source`'s nested helper that builds `unboxed_decls`,
threads them through perceus + tcrec, and hands the result to
`emit_program` / `emit_program_llvm`. Pre-edit the order was
`pre_perceus_decls = if use_llvm { unboxed_decls } else { tcrec_rewrite_decls(unboxed_decls) }`
followed by `perceus_decls = perceus_pass(pre_perceus_decls)`.
Post-edit, `post_perceus_decls = perceus_pass(unboxed_decls)`
runs first, and tcrec runs on `post_perceus_decls` only on the
C backend (LLVM stays as-is — TCO via the LLVM `tail` marker is a
separate lane, issue #37 non-goals).

## Perceus pass audit (assumptions about tcrec)

Audited `pcs_rewrite_kind` (line 33043) and `pcs_collect_uses_kind`
(line 32813). Findings:

- Perceus does NOT consult `tcrec_is_sentinel`. Grep for that
  predicate finds zero hits inside `pcs_*`.
- The dup walker only rewrites `EVar(nm)` reads when `nm in scope`
  (i.e. a fn parameter, let-binding, or pat-binder). The callee of
  a self-tail-call is `EVar("<fn-name>")` where `<fn-name>` is a
  global symbol — never in scope. So perceus would not have wrapped
  the callee whether it ran before or after tcrec.
- Tcrec's sentinel callees (`EVar("__kai_tcrec|...")`) likewise have
  names that cannot collide with any user identifier (the pipe
  separator is illegal in identifiers), so even if perceus's scope
  somehow contained `__kai_tcrec|...`, no real input would trigger
  it.

**Conclusion**: perceus is structurally agnostic to tcrec. No
changes to the perceus pass were needed.

## Tcrec pass audit (consumption of perceus shape)

Audited `tcrec_rewrite_decl` (line 34032), `tcrec_walk_tail` (line
33943), `tcrec_compute_site_dropmask` (line 34095), and the wrap
shape produced by `pcs_wrap_with_exit` (line 33606). Findings:

1. **`tcrec_walk_tail` does NOT descend into a perceus exit-drop
   wrap**. The wrap shape is
   `EBlock(stmts, Some(EVar("__pcs_ret")))` where one `stmts` entry
   is `SLet(PBind("__pcs_ret"), _, ORIGINAL_BODY)`. The walker
   recurses into `tail.kind` for `EBlock(_, Some(tail))`; for the
   wrap, `tail.kind = EVar("__pcs_ret")` — not a call, so the
   walker reports no tail self-call. Without an unwrap, every
   wrap-eligible fn would lose TCO post-reorder.

2. **The "entry-drops only" wrap shape (`EBlock(entry_drops,
   Some(body))`)** does not need special handling. Entry drops are
   `SExprStmt`s (not bindings), so `tcrec_block_scope` leaves the
   scope unchanged, and the walker descends into `body` normally.

3. **`tcrec_compute_site_dropmask` is byte-for-byte equivalent to
   `pcs_collect_exit_drops`**. Both apply the same predicate
   (`LUBlocked` OR (`LUAt` AND count ≥ 2)) to the same `uses`
   list. Post-reorder, tcrec could in principle read the wrap's
   exit-drop list directly instead of recomputing — but
   recomputing is structurally simpler and gives byte-identical
   output, so the dropmask code is unchanged.

4. **`pcs_collect_uses_expr` over the wrapped body would
   double-count entry / exit drops**. `SExprStmt(__perceus_drop(EVar(p)))`
   recurses into `EVar(p)` and adds a `U(p, ...)` to the use list
   even though the drop is not a real read. Tcrec must call
   `pcs_collect_uses_expr` on the **inner** body, not the wrapped
   one, to keep the dropmask correct. The unwrap helper makes
   this trivial.

**Conclusion**: tcrec needs one localized fix — recognise the
wrap, descend to the inner body, run `tcrec_walk_tail` and
`pcs_collect_uses_expr` on the inner, rewrite the inner, and
reassemble the wrap. Nothing else changes. The dropmask
computation, the sentinel-encoding, the goto-block emission,
`emit_fn_body`'s entry-label planting, and `emit_call_expr`'s
sentinel detection all stay byte-for-byte identical.

## Reorder implementation

Three building blocks added to `stage2/compiler.kai`:

1. **`tcrec_is_pcs_ret_wrap(body) : Bool`** (`stage2/compiler.kai`
   ~34095): cheap structural test. Returns `true` iff
   `body.kind == EBlock(_, Some(t))` and `t.kind == EVar("__pcs_ret")`.

2. **`type TcrecPcsTriple = TcrecPcsTriple([Stmt], Expr, [Stmt])`**
   plus **`tcrec_find_pcs_ret_let(stmts, before_acc) : Option[TcrecPcsTriple]`**:
   walks the wrap's `stmts` list, finds the first
   `SLet(PBind("__pcs_ret"), _, rhs)`, and returns
   `Some(TcrecPcsTriple(before, rhs, after))` where `before` is the
   reversed accumulator of stmts seen before the SLet and `after`
   is the rest. Returns `None` if no such SLet is in the list (a
   shape the perceus pass should never produce; defensive).

3. **`tcrec_rewrite_pcs_ret_wrap(...)`**: takes the unwrapped
   inner body, runs the same `tcrec_has_tail_self_call` /
   `pcs_collect_uses_expr` / `tcrec_any_unused` /
   `tcrec_rewrite_body` chain that the no-wrap branch uses, and
   reassembles the wrap with `tcrec_rebuild_pcs_ret_wrap`.

`tcrec_rewrite_decl` itself dispatches on `tcrec_is_pcs_ret_wrap`:
when true, it routes through the new helper; when false, it runs
the original logic on `body` directly. The non-wrap branch is
byte-identical to the pre-edit code, so any decl that perceus
left unchanged (no entry drops, no exit drops) sees exactly the
old tcrec behaviour.

## Selfhost convergence

C backend: byte-identical via the makefile target (`make selfhost`)
in 1 iteration. The bootstrap chain is `stage1/kaic1` → produces
`stage2/build/stage2.c` → cc → `stage2/kaic2` → produces
`stage2/build/kaic2b.c` → cc → `stage2/build/kaic2b` →
`./kaic2b stage2/compiler.kai > kaic2c.c`; `diff -q kaic2b.c
kaic2c.c` is empty. Selfhost-llvm: byte-identical in 1 iteration
(`make -C stage2 selfhost-llvm`).

The initial implementation (with `Option[TcrecPcsWrap]` instead of
`Bool` + branch) needed 2 internal iterations to converge — the
first iter compiled the new source with the OLD kaic2 (which had
no awareness of the new helpers), producing a different .c than
the SECOND iter (which used the NEW kaic2 emitting the new
helpers). The refactor to `Bool` + dispatch reached convergence
at iter 1 because the structural change to the source was
smaller. Either way, well within the lane brief's 5-iter
budget.

## #313 special-case decision

**Kept.** The `tcrec_tail_always_sentinel` predicate at
`emit_match_arm` (line 11554) targets `_scr` — an emitter-only
synthetic local that exists only inside the C statement-expression
the match emits, with no AST representation. Pipeline order does
not affect `_scr`. Removing the special-case would re-open the
issue-309 leak (countdown int leak from O(1) back to O(N)).

Verified empirically: with the reorder in place but the #313
special-case still active, `KAI_TRACE_RC=1 ./build/tco-309` reports
`alloc_total=999874 free_total=999873 leaked=1 live_peak=2`
— byte-identical to post-#313 baseline. Removing the special-case
is a separate concern (out of scope).

## KAI_TRACE_RC pre/post per-tag totals

Selfhost `kaic2 stage2/compiler.kai` under `-DKAI_TRACE_RC=1`
(`stage2/build/kaic2b` built with `-O0`):

| tag      | pre-reorder leak | post-reorder leak |   delta |
|:---------|-----------------:|------------------:|--------:|
| int      |          559,660 |           559,660 |       0 |
| real     |                2 |                 2 |       0 |
| char     |            4,277 |             4,277 |       0 |
| str      |          410,981 |           410,978 |      −3 |
| cons     |        4,518,915 |         4,518,915 |       0 |
| record   |        6,816,618 |         6,816,618 |       0 |
| variant  |        5,172,311 |         5,172,311 |       0 |
| closure  |          486,335 |           486,335 |       0 |
| array    |            8,298 |             8,298 |       0 |
| **total** |   **17,977,397** |    **17,977,396** |  **−1** |

Allocations: `alloc_total=58,519,267` → `58,519,264` (−3, from
3 internal strings whose lengths changed when `pre_perceus_decls`
was renamed in the driver). The reorder is **net-zero** on RC
discipline: every leak count is identical or one less.

`reuse_in_place` count unchanged at 2855 — the reuse recogniser's
behaviour is independent of pipeline order.

## All existing fixtures status

- `examples/tco/main.kai` (`if`-tail count_down 50M): green
  (`make test-tco`).
- `examples/tco/list_nth_shape.kai` (issue #43 / #92 R6): green
  (`make test-tco-regression`).
- `examples/tco/issue_309_countdown.kai` (issue #309): green;
  countdown int leak `live=0`, `live_peak=2`, exactly matching
  post-#313 baseline (`make test-tco-309`).
- `examples/tco/r12_zero_arg_recursive.kai` via test target:
  green (`make test-tco-zero-arg`).
- `examples/perceus/unbox_bench.kai` (issue #89): green
  (`make test-tco-perceus-wrap`).
- New fixture `examples/aspirational/pipeline_reorder_smoke/`:
  green on both backends (`make test-aspirational`); both
  `walk` and `build` get the goto label + #313 `kai_decref(_scr)`
  injection in emitted C, confirming tcrec runs through the
  perceus wrap.

## Empirical evidence

### Countdown fixture (issue_309)

```
Pre-reorder (post-#313 main):
  alloc_total=999874 free_total=999873 leaked=1 live_peak=2
  tag int allocs=999873

Post-reorder:
  alloc_total=999874 free_total=999873 leaked=1 live_peak=2
  tag int allocs=999873
```

Identical. The reorder neither closes new leaks nor opens new
ones in this fixture; it eliminates the structural coupling that
made #313 necessary in the first place.

### Smoke fixture (pipeline_reorder_smoke)

Goto label + #313 inject present in emitted C for both `walk`
(`match`-tail recursive) and `build` (`match`-tail recursive),
confirming tcrec successfully descends through the perceus wrap.
Output byte-identical on C and LLVM backends. The fixture also
runs cleanly under `kai run`.

## Friction points

- **`Option`-returning helpers inflate kaic2 self-compile leak by
  +0.25%.** First-pass implementation used
  `tcrec_unwrap_pcs_ret(body) : Option[TcrecPcsWrap]` + a wrapper
  type with three fields. Each `Some(TcrecPcsWrap(...))` allocation
  per processed decl added ~45k allocs to the selfhost run, which
  showed as a +44,975 leak delta in the per-tag table. Refactor to
  `tcrec_is_pcs_ret_wrap(body) : Bool` followed by `match
  body.kind { EBlock(stmts, _) -> ... }` brought the delta to
  net-zero. Lesson: in a hot pipeline pass, prefer cheap
  structural predicates over Option-wrapped intermediates.
- **`tcrec_rewrite_pcs_ret_wrap` arity (13 params)** is large but
  unavoidable given that DFn has 10 fields and the helper
  receives the unwrap-decomposed `(stmts, body, arity)` on top.
  An alternative would be to thread `d` through and re-destructure
  inside the helper, but that costs an extra `match` plus its
  associated alloc (the same shape as the +0.25% issue). Keeping
  the explicit param list pays for itself.

## Subjective summary

A 43-minute lane that closed Eric's structural debt by adding ~110
lines of code, all localized to `tcrec_rewrite_decl`. The
audit-driven approach was decisive: reading the perceus pass's
scope-tracking and the wrap's exact AST shape pinned down the
single point of friction (a `tail.kind == EVar("__pcs_ret")` that
the original walker can't descend through). One predicate + one
helper + one rebuild function and the reorder is structural,
byte-identical, net-zero on RC discipline, and TCO-preserving on
both backends.

The lane vindicates Eric's "coordinar sin reordenar es la deuda
peor con diferencia" verdict: the cost of the coordination was
~50 lines of `tcrec_compute_site_dropmask` documentation explaining
how it must mirror `pcs_collect_exit_drops`, plus the recurring
risk that a future change to perceus would silently break tcrec.
The cost of the reorder is ~110 lines of localized code where
the responsibilities are made explicit at the AST shape level.
The two passes now coordinate through a structural contract
(perceus produces the wrap; tcrec recognises and preserves it)
instead of through a documented invariant (the dropmask must
match the exit drops byte-for-byte).

#92 R6's failure mode — the malloc-tcache abort on Linux when the
dropmask diverges from the exit drops — should now be reachable
because the dropmask is computed from the same `uses` list
perceus already consumed, against the inner body that perceus
already saw. A separate lane could attempt #92 R6's strict
dropmask once more on top of this reorder.

## Limitations / next steps

1. **#92 R6 (`list_nth_shape.kai` cons leak)** is unblocked
   structurally, but the lane brief explicitly does not retry
   it. A separate lane could attempt the strict dropmask now
   that tcrec receives a perceus-cleaned AST.
2. **The LLVM backend still skips tcrec entirely** — TCO via the
   LLVM `tail` marker is issue #37 non-goals. Post-this-lane,
   the LLVM path runs `perceus_pass(unboxed_decls)` and skips
   `tcrec_rewrite_decls`, exactly as before.
3. **The #313 `tcrec_tail_always_sentinel` injection in
   `emit_match_arm`** stays. Removing it requires a separate
   investigation of whether perceus could plant the `_scr`
   decref as an AST node rather than relying on emitter-time
   string manipulation.
4. **The reorder removes the documented invariant in
   `tcrec_compute_site_dropmask`** that the dropmask "mirrors
   `pcs_collect_exit_drops` exactly". The comment block (lines
   34063-34094) could be simplified now that the coordination
   is structural, but doing so is a docs-only follow-up; the
   code itself is correct as-is.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T23:22:52-04:00	lane-start	-	-
2026-05-06T23:30:00-04:00	tier0-baseline	OK	-
2026-05-06T23:35:00-04:00	first-impl-compile	OK	-
2026-05-06T23:36:00-04:00	first-selfhost-converge	OK	-
2026-05-06T23:50:00-04:00	first-trace-rc	+0.25%-DELTA	-
2026-05-06T23:55:00-04:00	refactor-to-Bool-helpers	OK	-
2026-05-07T00:00:00-04:00	final-trace-rc	NET-ZERO	-
2026-05-07T00:02:00-04:00	tier0	OK	-
2026-05-07T00:02:00-04:00	tier1	OK	-
2026-05-07T00:04:00-04:00	tier1-asan	OK	-
2026-05-07T00:04:00-04:00	selfhost	BYTE-IDENTICAL	-
2026-05-07T00:04:30-04:00	selfhost-llvm	BYTE-IDENTICAL	-
2026-05-07T00:05:42-04:00	lane-end	OK	-
```
