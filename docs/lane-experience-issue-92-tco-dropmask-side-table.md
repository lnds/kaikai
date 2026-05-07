# Lane experience report — issue #92 TCO rule-3 via side table

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

This lane re-lands TCO rule 3 (precise per-call-site dropmask
distinguishing `LUAt + count==1 AND p NOT in args` from
`LUAt + count==1 AND p in args`) after three previous attempts
(PR #41, PR #48 commits 522b9df + bd54356) hit the same Linux glibc
tcache abort. The brief specified strategy #3 from issue #92's path
forward: precompute a side table in a pass that runs BEFORE the
rewrite and never consults `[Expr]` values during the dropmask
computation itself.

## Objective metrics

- Start: 2026-05-05T22:47:44-04:00
- End:   2026-05-05T23:23:16-04:00
- Wall-clock: ~35 min agent time.
- Build/test invocations (from instrumentation TSV):
  - `make tier0`: 2 (1 fail, 1 OK after fixing DM/TcrDm name
    collision).
  - `make tier1`: 2 (both OK).
  - `make tier1-asan`: 1 (OK).
  - `make -C stage2 selfhost`: 1 (byte-identical fixed point).
  - `make -C stage2 selfhost-llvm`: 2 (one OK, one segfault under
    `ulimit -Hs 8192` — see Linux simulation note).
  - Stage 1 ASAN compile of stage 2 source (manual): 1 (clean —
    the strongest local signal that the previous lanes' refcount
    imbalance is not present).
  - Manual third-iter selfhost: 1 (byte-identical iter2 == iter3).

### Empirical evidence — rule 3 fires correctly

Inspecting the emitted C from `examples/tco/list_nth_shape.kai`:

```c
/* nth_loop's goto block, from the side-table-driven dropmask: */
KaiValue *_t0 = kai_t;
KaiValue *_t1 = kai_op_sub(kai_internal_dup(kai_i), kai_int(1LL));
{ KaiValue *_ = kai_internal_drop(kai_xs); (void) _; }   /* rule 3 */
{ KaiValue *_ = kai_internal_drop(kai_i);  (void) _; }   /* rule 2 */
kai_xs = _t0; kai_i = _t1;
goto _kai_nth_loop_entry;
```

The `kai_internal_drop(kai_xs)` line is the rule-3 contribution. Under
the conservative dropmask shipped on `main` it is absent, leaving one
cons cell leaked per iteration. Under this lane the side table at
`(line, col)` of `nth_loop(t, i-1)`'s `f` EVar yields the bit set for
`xs` (count==1, not in args), the dropmask OR-combines it with the
rule-2 mask for `i`, and the goto block emits both drops.

`examples/tco/rule3_no_misfire.kai`'s `id_walk(xs, depth)` call site
goto block contains `kai_internal_drop(kai_depth)` (rule 2: count==2)
but NOT `kai_internal_drop(kai_xs)` — `xs` count==1, IS in args, rule
3 correctly skips. The Makefile target `test-tco-rule3-no-misfire`
asserts the absence by `awk` + `grep -F -q`.

## The side-table approach (avoiding [Expr] threading)

`tcrec_compute_site_dropmask`'s signature is unchanged from `main`;
its body still encodes only rules 1+2 (per-fn). Rule 3 contribution
arrives as a separate `Int` mask looked up by `(line, col)` in a
pre-computed `[TcrDm]` table.

The pre-pass `tcrec_collect_dm_table` walks the body once at the
start of `tcrec_rewrite_decl` (alongside the existing
`pcs_collect_uses_expr` walk) and produces `[TcrDm(line, col, mask)]`.
Inside the walker, `tcrec_rule3_mask` calls `tcrec_args_have_evar`
on the call's args — this is the ONLY place any `[Expr]`-shaped
value flows near rule-3 logic, and it's a self-contained function
returning `Bool` (same shape as `pcs_collect_uses_expr` returning
`[Use]` from a similar walk; that shape works fine in stage 1's
emit).

The side table threads through `tcrec_rewrite_body`,
`tcrec_rewrite_kind`, and `tcrec_rewrite_arms` as `dm_table: [TcrDm]`,
identical in shape to how `uses: [Use]` already threads. The dropmask
compute at each call site is `base_mask + rule3_mask` (disjoint by
construction — rule 1+2 fire on count==0 or count>=2; rule 3 fires
only on count==1 — so OR == ADD).

Total compiler delta: ~250 lines added in `stage2/compiler.kai`
between `tcrec_pow2` and `tcrec_make_sentinel`, plus signature
extensions to three existing rewrite fns. No deletions.

## Pre-pass shape (single walk, computed once)

```kaikai
let dm_table = tcrec_collect_dm_table(body, name, arity, params,
                                       pnames, uses, [], [])
let new_body = tcrec_rewrite_body(body, name, arity, c_sym_str,
                                   params, pnames, uses, dm_table, [])
```

Walk cost: O(body size) once per recursive fn. Lookup cost: O(call
sites per fn) per call site, linear scan over `[TcrDm]`. Both bounded
by the same fn-level analysis already performed by `tcrec_has_tail_self_call`
+ `pcs_collect_uses_expr`; no asymptotic regression.

## Empirical verification

- `examples/tco/list_nth_shape.kai` — pre-existing canonical fixture;
  rule 3 now fires (verified by inspecting emitted C as above).
  Conservative-dropmask test target `test-tco-regression` still passes
  (output unchanged).
- `examples/tco/rule3_basic.kai` (NEW) — minimal `walk(xs, acc)` shape,
  proves rule 3 fires. C-side assertion via `awk` + `grep -F -q`.
- `examples/tco/rule3_no_misfire.kai` (NEW) — `id_walk(xs, depth)`
  with `xs` IS in args, proves rule 3 does NOT misfire (would
  double-free under glibc strict if it did).
- All 4 TCO targets green: `test-tco`, `test-tco-regression`,
  `test-tco-zero-arg`, `test-tco-perceus-wrap`, `test-tco-rule3-basic`,
  `test-tco-rule3-no-misfire`.
- `make selfhost` byte-identical (and manually verified iter2 ==
  iter3 to exclude two-step convergence false-positives).
- `make selfhost-llvm` byte-identical without ulimit constraint.
- `make tier0` + `make tier1` + `make tier1-asan` all green on macOS.
- **Stage 1 ASAN compile of stage 2 source**: clean. This is the
  strongest local signal — the previous lanes' Linux glibc tcache
  aborts came from a refcount imbalance in stage 1's emitted C; if
  ASAN catches no heap errors when stage 1 (compiled with
  `-fsanitize=address,undefined`) compiles stage 2's source, the
  shape is not in the bug class. macOS-only `make tier1` could not
  have given this signal under previous lanes — running stage 1
  itself under ASAN does.

## Linux simulation (ulimit -Hs 8192)

`ulimit -Hs 8192 && make -C stage2 selfhost-llvm` segfaults during
the LLVM emit phase (`build/kaic2-llvm-s1 --emit=llvm compiler.kai`).
**This segfault reproduces on `main` (verified by cloning a fresh
worktree at `main` and running the same command)** — it is a
pre-existing recursion-depth issue in the LLVM backend, not
introduced by this lane. Logging the failure here so the integrator
isn't surprised.

The C backend's `selfhost` (no LLVM emit) succeeds under `ulimit
-Hs 8192` and converges byte-identical (the LLVM emitter has deeper
stacks than the C emitter; out of scope for this lane).

## Friction points

1. **DM name collision** (one tier0 fail). My initial type was
   `type DM = DM(Int, Int, Int)` — collided with the existing
   `type DupMark = DM(Int, Int)` variant constructor at line 33091.
   Stage 2's typer reported it as "expected 3, found 2" sub-pattern
   counts and "wrong arg count" on the `DM` constructor at the
   pre-existing `DupMark` use sites. Renamed to `TcrDm` and fixed
   the one straggling `[DM]` annotation I missed in the second
   pass. ~3 min lost.

2. **Instrumentation TSV outcome bug**. My TSV-update pattern
   captures `$?` AFTER `tail`, so failures bubble through `tail`'s
   exit 0 and get logged as OK. The `selfhost-llvm-ulimit-8192`
   line shows OK in the TSV but actually segfaulted. Caught when
   reading the actual stderr output. Future lanes should pipe
   through `set -o pipefail` or split the make/tail invocation.

3. **Rule 3 misfire fixture v1 wasn't actually a misfire test**.
   First version had `xs` count==2 (match scrutinee + arg use), so
   rule 2 covered the drop and the test would have passed
   regardless of rule 3. Rewrote to `id_walk(xs, depth)` shape
   where `xs` count==1 AND `xs` IS the recursive call's first arg
   — exactly the case rule 3 reasons about with the opposite
   answer.

## Subjective summary

- Confidence in correctness: **high for the implementation
  shape**. Three structural shapes for rule 3 had been tried and
  abandoned in PR #41 + #48; this is the fourth and the brief's
  pre-specified path. The pre-pass returns plain int triples and
  the dropmask compute touches no `[Expr]`. Stage 1 ASAN green
  during compile of stage 2 source is the strongest signal short
  of Linux CI itself.
- Confidence in Linux CI passing: **medium-high**. macOS local
  green didn't gate the previous attempts (their failure pattern
  was Linux-only). But: the failure mechanism in those lanes was
  a refcount imbalance in stage-1-emitted C, which ASAN catches
  on macOS. Local stage 1 ASAN clean compile of stage 2's source
  is the closest test we have. Still: if Linux CI fails, this
  lane should NOT auto-merge. Treat the auto-merge as a "let CI
  decide" rather than a "this is definitely fine".
- Hardest sub-task: deciding what the no-misfire fixture should
  test. The `walk_with_witness` v1 was a false-positive test; the
  `id_walk` v2 actually exercises the count==1 + p-in-args case.
- Easiest sub-task: implementing the side table. The pre-pass is
  a near-copy of `tcrec_walk_tail`'s shape, the lookup is a 4-line
  linear scan, and the dropmask combine is a single `+`.
- Did the compiler help? Yes — the `DM`/`TcrDm` collision was
  caught at typecheck, in the first build, with a precise error
  message. The renaming was mechanical.
- Did the integrator's process help? Yes — the brief's strategy
  #3 specification was extremely specific ("the side table key is
  the call site's source location; the value is the per-param
  flag bitmask"), and the explicit "do NOT thread `[Expr]`
  through `tcrec_compute_site_dropmask`" prevented retrying any
  of the three known-bad shapes.

## Limitations

- Self-report bias acknowledged.
- Single agent (Claude Opus 4.7), not generalisable across LLMs.
- Linux CI is the actual gate. macOS-local + macOS-ASAN green is
  necessary but historically not sufficient (PR #41 + #48 both
  hit Linux-only aborts).
- The pre-existing LLVM-emit segfault under `ulimit -Hs 8192` is
  out of scope and tracked separately by the m4c/LLVM-emit
  honesty targets.
- Stage 1 perceus emit's underlying refcount imbalance (the root
  cause that crashed PR #41 + #48) is NOT fixed by this lane.
  The side table is the workaround — option #3 from issue #92's
  three-option path forward. Options #1 (audit stage 1 perceus
  emit) and #2 (visible alloc tracing in `runtime.h`) remain
  open.

## Raw build log

See `/tmp/lane-issue-92-tco-dropmask-side-table-builds.tsv`. Note
the instrumentation bug above — `selfhost-llvm-ulimit-8192` row
shows OK but the underlying make actually exited 2.
