# Lane experience — `linus-b-listappend`

**Date:** 2026-05-29
**Origin:** linus km audit (`docs/stage2-quality-km-audit-2026-05-29.md`, Lane B).
**Branch:** `linus-b-listappend` (off `main` @ 6d04b7c).

## Scope as planned

Kill the O(n²) `list_append(p1, list_append(p2, p3))` chaining in the
`inf_scan_uses_*` walkers of `stage2/compiler/infer.kai` — the
last-use-analysis tree walk that feeds `--dump-last-use`. Replace the
append-chaining with a reverse-order accumulator threaded through the
walk plus a single `list_reverse` at the public entry, **without
changing the produced `Use` set** (content identical; observable order
preserved). Optionally fold in two "twin" nested-`list_append` sites
flagged by the audit (`scheme_to_string`, the diagnostic-notes
assembly).

## Scope as shipped

Rewrote all 11 mutually-recursive `inf_scan_uses_*` functions
(`infer.kai` ~1024–1227) to the accumulator-threading style:

- New private worker `inf_scan_uses_expr_acc(e, in_lam, acc)` and an
  `acc: [Use]` parameter on every `inf_scan_uses_*` helper. Each
  `EVar` conses to the front (`[U(...), ...acc]`, O(1)); children are
  visited left-to-right threading `acc`.
- `inf_scan_uses_expr` keeps its original `(Expr, Bool) : [Use]`
  signature and is now the **sole** `list_reverse` site — the front-cons
  accumulator is the reverse of source pre-order, so one reverse at the
  entry restores the exact observable ordering the old code produced.
- The public surface is unchanged: `inf_scan_uses_expr` is the only
  function called from outside the walker cluster
  (`dump_last_use_for_decl`, infer.kai:17353), and its signature/return
  type is identical.

The two twin sites were **left as follow-up**, not folded in — see
below.

## Why accumulator (design decision + alternatives)

The old shape is `list_append(left_uses, right_uses)` at every interior
node. `list_append` copies its left argument, so a list-shaped subtree
(stmts, call args, list elems, match arms — all of which recurse with
`list_append(head_uses, rest_uses)`) is O(n²) in node count: appending
the k-th element's uses copies the k-1 already-accumulated entries.

Alternatives considered:

1. **Front-cons accumulator + single reverse** (chosen). O(n) total,
   O(1) per node. Preserves observable order exactly via the final
   reverse. Idiomatic in the codebase (`uuid.kai`, `crypto/hash.kai`
   all use `[x, ...acc]` + `list_reverse(acc)`).
2. **Front-cons accumulator, NO reverse.** I verified the only consumer,
   `inf_last_use_for_loop` (infer.kai:465), is fully order-independent:
   it takes the max `(line, col)` and ORs `in_lam` (any in-lambda ref →
   `LUBlocked`). So dropping the reverse would still produce identical
   `LU` results per param. **Rejected** anyway: the brief asks to
   preserve observable order, and keeping the reverse (a) makes the
   change defensible against a future order-dependent consumer and (b)
   keeps `--dump-last-use` output byte-stable, which matters for the
   selfhost fixed-point and any golden.
3. **Difference lists / `[Use] -> [Use]` continuations.** Over-engineered
   for a single-consumer diagnostic walker; the accumulator is simpler
   and equally O(n).

`[x, ...acc]` as an *expression* (not just a pattern) is valid kaikai —
`kai info syntax` documents the *pattern* form, but the list-literal
spread in expression position is used across stdlib and round-trips
through selfhost. Confirmed by a micro-build before committing.

## Twin sites: deferred as follow-up

The audit flagged two further nested-`list_append` sites:

- `infer.kai:5177` — `list_append(tv_part, list_append(rv_part, uv_part))`
  in `scheme_to_string`.
- `infer.kai:13894` — `list_append(miss_notes, list_append(cov_notes, help_ri))`
  in the missing/covered diagnostic-notes assembly.

These are **bounded depth-2** concatenations of three *fixed* lists, not
recursive append-chains. The cost is O(|left|) once, linear and trivial
(forall-var lists and diagnostic note lists are tiny). They are **not**
the O(n²) pathology that motivates the lane — folding them in would add
change surface (and a reverse or a 3-arg helper) for no measurable
complexity win. Left as cosmetic follow-up; documented here so the next
reader doesn't re-flag them as the same bug-shape.

## Structural surprises

- The walker cluster is **mutually recursive** across 11 functions
  (`_kind` ↔ `_expr` ↔ the per-collection helpers). The accumulator had
  to thread through every one of them in lockstep — you cannot convert
  one without converting all, because the public `inf_scan_uses_expr`
  is called recursively from inside the cluster and from the entry.
  Solution: split the reverse-at-entry wrapper (`inf_scan_uses_expr`,
  unchanged signature) from the recursive worker
  (`inf_scan_uses_expr_acc`, new).
- The consumer is order-independent (max-wins), so the *correctness*
  bar was trivially met; the *byte-identity* bar (selfhost fixed-point)
  was the real gate, and it passed.

## Fixtures + coverage

No new fixture added: the change is a pure-internal rewrite of a
diagnostic walker with no behavioural delta. Coverage is by:

- The selfhost fixed-point itself (kaic2 compiles its own 18k-LOC
  `infer.kai`, which exercises every `ExprKind` arm of the walker).
- Existing inference/effects tier1 fixtures that run `--dump-last-use`
  indirectly through the typer.

Coverage gap acknowledged: there is no dedicated golden pinning
`--dump-last-use` output ordering. Adding one was out of scope (no
behavioural change to pin), but it would harden the "observable order"
contract for a future refactor.

## Real cost vs estimate

Per project policy, no time estimate was made. The work was: read the
cluster + its single consumer (to prove order-independence), rewrite 11
functions mechanically, build, diff the emitted C (quirúrgico: only the
11 functions changed in 96k lines of `stage2.c`), run the gates.

## Gate evidence

- `make selfhost`: **OK** — `kaic2b.c == kaic2c.c` (byte-identical
  fixed-point with the change applied).
- `make -C stage2 selfhost-llvm`: **OK** — `s1.ll == s2.ll`.
- `make tier0`: **green** (31 OK, 3 no-golden, 0 diff, 0 failed; demos
  baseline 34 holds).
- Emitted-C diff vs `main` baseline: **only** the 11 rewritten
  `inf_scan_uses_*` functions differ; the other ~96k lines of `stage2.c`
  are byte-identical → the change touches no other compiler pass and no
  codegen.

### tier1 — blocked by pre-existing failure (NOT this lane)

`make tier1` fails at `test-issue-318-include` with **Error 139
(segfault)**. This failure is **pre-existing on `main`**, verified two
ways:

1. Rebuilt the baseline `kaic2` from `main`'s `infer.kai` (stashed this
   lane's change) in the same worktree — `test-issue-318-include`
   segfaults identically.
2. CI on `main` itself is red on `tier1` for the same fixture: run
   26612257359 (push of 6c717d8) and the `0.85.1` bump push both show
   `make[1]: *** [test-issue-318-include] Error 139`.

The fixture compiles the *entire* stdlib with `--include-prelude-tests
--test` and segfaults at runtime — unrelated to last-use analysis. No
open tracking issue exists for it (the original #318 feature is closed).

## Follow-ups left for next lanes

1. **Pre-existing**: `test-issue-318-include` segfault on `main` — tier1
   is red independent of this lane. Needs its own lane + tracking issue.
2. Twin bounded-`list_append` sites (`infer.kai:5177`, `13894`) —
   cosmetic, optional.
3. (Optional) a `--dump-last-use` ordering golden to pin the observable
   order contract.

## Merge decision

Per Eduardo (2026-05-29, one-shot authorization for audit Lanes A+B):
open the PR and enable `gh pr merge --auto --merge`. The only red tier1
gate is the pre-existing `issue_318_include` SIGSEGV, verified red on
`main` itself (CI run 26612257359). The refactor is selfhost
byte-identical (fixed-point + LLVM) with a surgical emitted-C diff, so
it cannot have caused or worsened that failure. Lane A (PR #729) took
the same pattern and is awaiting Eduardo's manual merge for the same
reason; this lane uses auto-merge under the same authorization.
