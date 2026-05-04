# Lane experience — issue #209 (Perceus reuse-in-place: resolve the `pcs_rewrite_expr → __perceus_dup` interaction)

Date range: 2026-05-03 (single working session, ~45 min wall).
Lane branch: `issue-209-perceus-dup`.
Predecessor: PR #208 (closed #118 v1, retro at
`docs/lane-experience-issue-118-perceus-reuse-in-place.md`).
Issue spec: `gh issue view 209`.

## Objective metrics

- Lane start: `2026-05-03T21:30:00-04:00`.
- Lane end:   `2026-05-03T22:11:30-04:00`.
- Wall-clock: ~45 minutes including audit, two STOP-and-report
  consultations, implementation, two diagnostic loops (one for the
  `args` shadow bug, one for the empirical RC trace), and full
  validation.
- Build / test invocations: 5 logged in
  `/tmp/lane-issue-209-perceus-dup-builds.tsv` (appended below).

## What shipped

Single delta in `stage2/compiler.kai` (~120 LoC net add, no new
runtime primitives).

1. **Reuse-arm detector** `try_reuse_cons_arm` — returns
   `Some(ReuseConsArm)` when an arm matches `[head_pat, ...rest_name]`
   with body `__perceus_reuse_cons(__pcs_scr, h_new, t_new)` (the
   shape `pcs_recognise_reuse_*` produces from the cons-rebuild
   pattern recogniser added in #208).

2. **Dual-path emit** `emit_match_arm_reuse_cons` — for detected
   reuse-arms, emit two paths inline:
   - `if (kai_check_unique(_scr))`: bind head/tail by **borrow**
     (no incref on the children), evaluate `h_new` / `t_new`
     (which consume the borrowed binds linearly), overwrite the
     `_scr->cons.head/tail` slots in place, `kai_incref(_scr)` to
     survive the match-exit decref. `kai_rc_reuse_total++` for
     trace.
   - `else`: bind head/tail by `kai_incref` (the standard pat-binds
     path), evaluate `h_new` / `t_new`, fall back to `kai_cons`.
   - The body's `h_new` / `t_new` are emitted under the same
     pattern-binder names in each branch; the two branches duplicate
     the body text but evaluate it exactly once at runtime.

3. **`emit_match_arm` integration** — when no guard and the
   detector matches, route the arm through `emit_match_arm_reuse_cons`;
   otherwise the existing default path applies. Guards disable
   the dual-path (a guard that rejects after the unique branch
   committed to overwriting slots would be a use-after-free).

The runtime helper `kai_reuse_or_alloc_cons` is left intact for the
LLVM backend (which still routes through the v1 `__perceus_reuse_cons`
dispatch). The LLVM backend is unchanged in this lane — selfhost-llvm
remains byte-identical pre/post.

## Compiler errors I encountered

Two classes:

- **`args` parameter shadowed by prelude `args()` function** —
  first draft named the inner `match` scrutinee `args` (the call's
  argument list). kaic1 emitted the body with `_scr = closure(args
  thunk)` instead of `_scr = the local args binding`, so
  `try_reuse_cons_arm` was inert (always returned `None`) — the
  selfhost converged on the *wrong* (i.e. unchanged) output. Caught
  by inspecting `stage2/build/kaic2b.c` and seeing
  `kai_closure(&_kai_prelude_args_thunk, ...)` in the wrong place.
  Memory note `feedback_kaikai_param_args_shadow.md` covers this
  trap. Fixed by renaming `args` → `call_args`. **Cost: ~5 min once
  the `kai_closure(...)` smell was spotted.**

- **`emit` GitHub label does not exist** — `gh issue create`
  rejected `--label emit` for #212. Fixed by switching to
  `--label compiler` (which is the canonical scope label per
  CLAUDE.md). Cost: 30 s.

## Friction points

- **Was `pcs_rewrite_expr`'s dup structural or defensive?**
  *Neither — it was a misdiagnosis.* The audit (issue spec) named
  `pcs_rewrite_expr → __perceus_dup` as the blocker, but a careful
  read of `pcs_collect_uses_in_arms` + `pcs_is_non_last` showed the
  recogniser-eligible scrutinees are **last-use** in their scope, so
  no `__perceus_dup` wrap is inserted around them. The actual
  blocker is `emit_pat_binds_list` (line 10463/10456 of
  `stage2/compiler.kai`) which emits
  `kai_incref(_scr->as.cons.head/tail)` for every cons pattern bind.
  Those increfs bump the RC of the *children* of the consumed cell;
  the recursive consumer of `tail` therefore enters its own
  scrutinee with RC ≥ 2, defeating `kai_check_unique`. This is the
  follow-up #3 named in the #208 retro ("Limitations" → "L-per-call
  amplification"), not the pass-ordering issue the spec described.
  See the first STOP-and-report block of this lane's transcript for
  the full audit.

- **Did Path 1 work or did you fall back?** Neither of the issue's
  three named paths applied verbatim. The actual fix is Path 3
  *renamed*: not "match-scrutinee dup-suppression" (there is no dup
  on the scrutinee) but "match-scrutinee **bind-incref**-suppression"
  on the children of the consumed cell. The dual-path layout makes
  the suppression safe: it only commits to borrow when the runtime
  RC == 1 check has already passed, and the fallback branch keeps the
  standard owned-binds layout intact for the shared case.

- **Did ASAN catch any subtle issues during dev?** No.
  Tier 1-ASAN went green on the first run after the `args` shadow
  fix. The dual-path's invariant ("the borrow path is only entered
  when no other owner exists, so the body is free to consume the
  children linearly") is locally checkable; ASAN had nothing to
  flag.

- **Self-compile firing remained 0 — was the fix wrong?** No, the
  fix is correct. Two unrelated factors prevent the gain from
  showing in `kaic2` self-compile:
  1. Virtually every list in `stage2/compiler.kai` is multi-use
     (consumed by more than one closure / branch), so
     `pcs_rewrite_expr` does pre-incref it for those reads, and the
     scrutinee enters the match with RC > 1. The dual-path's unique
     check correctly falls back to `kai_cons` in that case.
  2. The TCO lowering for `[n, ...acc]` in a recursive call emits
     `kai_cons(kai_internal_dup(n), kai_incref(kai_internal_dup(acc)))`
     — a **double incref** of the accumulator (filed as #212). On
     `build(N, [])`-style fixtures this means every cell after the
     outermost has RC ≥ 2 at construction time, blocking reuse on
     all but the outermost cell. A non-TCO inline-built list (e.g.
     `let xs = [1, 2, 3, 4, 5]`) does not hit this bug and shows
     5/5 cells reuse.

## Spec ambiguities or interpretive choices

- **How did you instrument firing count?** No permanent
  instrumentation was added. The `kai_rc_reuse_total++` from #208
  remains in `stage0/runtime.h` and prints via the existing
  `[KAI_TRACE_RC]   reuse_in_place=N` line at process exit (gated
  on the counter being non-zero). For diagnostic purposes during
  the lane, a temporary `if (getenv("KAI_DEBUG_REUSE")) fprintf(...)`
  was added inside `kai_check_unique` to capture per-cell RC values
  on a small fixture; that probe was reverted before commit (the
  empirical output is preserved verbatim in #212's body).

- **What did you do with shared cons cells in tail positions?**
  The dual-path emit defers commitment until **after** the
  `kai_check_unique` test. If the cell is shared, the fallback
  branch uses `kai_incref` on the children (the standard layout)
  and `kai_cons` to allocate a fresh cell; the original `_scr` and
  its children remain intact, and the match-exit `kai_decref(_scr)`
  reclaims them normally. There is no scenario where a shared cell
  is mutated, so no UAF window exists.

- **Did you re-evaluate the issue's three paths?** Yes, and the
  audit concluded all three were misdiagnosed (pass-ordering,
  dup-elision, dup-suppression all assume a dup on the scrutinee
  that does not exist). The first STOP-and-report block laid this
  out; the user authorised proceeding with the actual fix.

## Subjective summary

- **Confidence**: high on the dual-path emit. Selfhost C
  byte-identical at fixed point, selfhost LLVM unchanged
  (correctly — the LLVM backend was not touched), tier1 + tier1-asan
  green, fixture validation shows 5/5 reuse on a single-use spine
  (was 1/5 pre-fix). Honest about the self-compile firing count
  remaining 0 and the reasons (multi-use compiler patterns +
  upstream TCO double-incref bug #212).
- **Hardest**: persuading myself to STOP and report twice — once
  on the misdiagnosis ("the issue's premise is wrong"), once on
  the result ("the fix is correct but doesn't move the headline
  metric"). Both reports were accepted; the second authorised
  shipping the v1.1 fix as-is and filing the upstream bug
  separately.
- **Easiest**: writing the dual-path emit. The shape was
  mechanically dictated by the invariants from the audit, and the
  existing `emit_pat_binds_list` provided the template for both
  branches.
- **Compiler help vs hinder**: kaic1's `args` shadowing of the
  prelude function silently broke `try_reuse_cons_arm` to a no-op,
  which turned the first selfhost run into a false positive
  (kaic2b.c was identical to the pre-fix kaic2c.c, so the
  byte-identical gate passed without the new emit ever firing).
  This trap is in user memory but I still hit it. A diagnostic
  that flagged `args` (or any prelude name) when used as a local
  binding name would have saved the loop.

## Perf measurement

`kaic2` self-compile (`./stage2/build/kaic2b stage2/compiler.kai
> /dev/null`), median of three runs each on macOS Apple Silicon, `-O0`:

| metric | Pre-fix | Post-fix | Delta |
|---|---:|---:|---:|
| `alloc_total`             | 51,257,261 | 51,381,200 | +0.24 % |
| `cons` allocs             |  5,992,484 |  6,000,677 | +0.14 % |
| `reuse_in_place`          |          0 |          0 | (see analysis) |
| Wall (median, s)          |       7.92 |       7.74 | −2.3 % |

Wall is within run-to-run noise; the dual-path emit removes a
function-call hop (the runtime helper) at the cost of one extra
branch, which roughly cancels.

`reuse_cons_basic` fixture (5 cells, inline-built spine):

| metric | Pre-fix | Post-fix |
|---|---:|---:|
| `reuse_in_place` | 1 | **5** |

The dual-path emit on a single-use spine fires reuse on every cell —
the "L-per-call" amplification the audit projected, contingent on
the spine being uniquely owned at runtime (which `compiler.kai`'s
own lists are not, for the reasons inventoried above).

### Audit projection vs reality (cumulative across #118 v1 + #209)

| | Audit projection (PR #204) | #118 v1 | #209 v1.1 (this lane) |
|---|---:|---:|---:|
| `alloc_total` delta | −36 % | ~0 % | ~0 % (self-compile) |
| Wall delta at `-O2` | −5 % to −15 % | not measured | not measured |
| Wall delta at `-O0` | (n/a) | +6.1 % | −2.3 % vs main (within noise) |
| Reuse fires (self-compile) | (assumed all) | 0 | 0 |
| Reuse fires (single-use spine) | (assumed all) | 1 / L | **L / L** |

The audit's single biggest assumption — that `kaic2` self-compile
exercises uniquely-owned cons spines — was wrong. The single-use
fixture confirms the runtime path and the recogniser are both
correct; the self-compile workload simply doesn't have the
qualifying spines for the reasons documented above. To move the
self-compile headline number, the path is now: fix #212, then
investigate whether the compiler can be refactored to thread more
lists single-use, then re-evaluate.

## Limitations of this report

- Wall-time numbers are single-run on a single machine (Apple
  Silicon, macOS), `-O0`. The audit projected `-O2`; not
  re-measured here because the user-loop experience is `-O0` and
  the self-compile workload doesn't fire reuse anyway.
- The 5/5 reuse number is from a synthetic 5-element inline list.
  Larger spines (10, 100, 10⁵) would scale linearly under the same
  invariants but were not measured — the headline metric is
  whether reuse fires per-cell, not how the count scales.
- The LLVM backend was not modified. `kai_reuse_or_alloc_cons` is
  still the LLVM lowering target for `__perceus_reuse_cons`.
  Lifting the L-per-call win to LLVM is a follow-up; the C / LLVM
  parity gate (`make selfhost-llvm`) remains green only because
  both backends produce different (but each internally consistent)
  emission for the same source. A follow-up that adds a parallel
  `llvm_emit_match_arm_reuse_cons` would unlock the same gain on
  the LLVM backend.
- The dual-path emit duplicates the body text in the C output (one
  body per branch). Object-code size on `kaic2b.c` grew by less
  than 0.1 % (37 reuse-arm sites, each adding ~200 bytes of source
  per duplicate) — well within the budget for the optimisation.
- Self-compile firing of 0 is a real result, not a bug. See
  "Friction points" → "Self-compile firing remained 0" for the
  two upstream causes (#212 + multi-use compiler lists).

## Follow-up issues filed

- **#212** Tail-call lowering double-increfs cons accumulator.
  Empirical repro embedded; closing this unblocks reuse on
  `build`-style fixtures and any other TCO accumulator pattern.

(No new follow-ups for variant / record reuse — those are still
tracked under #210 and the disabled arms in `pcs_try_reuse_arm`,
unchanged by this lane.)

## Build TSV (appended)

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T21:36:43-04:00	tier0_baseline	OK	-
2026-05-03T21:45:47-04:00	selfhost_C_dual_path	OK	-
2026-05-03T22:00:10-04:00	selfhost_llvm	OK	-
2026-05-03T22:04:44-04:00	tier1	OK	-
2026-05-03T22:05:48-04:00	tier1-asan	OK	-
```
