# Front A — kill the InferState RC churn (plan, not yet implemented)

Status: **designed + validated by asu and linus, deferred for fresh-session
implementation** (2026-05-27 overnight). The two prerequisite lanes shipped
this night: the Mutable effect-leak fix and the first-class `KAI_REF` cell.
Front A is the third and largest; linus advised against coding the delicate
Subst-aliasing analysis late at night. This doc captures everything needed
to execute it cold.

## The measured target

Self-compile of the ~77.6k-LOC compiler bundle, instrumented with
`-DKAI_PROFILE_RC` (kaic2 at -O2, post-KAI_REF):

```
incref = 10,278,869,434   decref = 5,770,239,329
alloc  =    293,406,231   free   =   202,268,882
rc_traffic = 45.0% of wall   alloc/free = 1.8%   other = 53.2%
```

**~10.3 BILLION increfs to compile 77k LOC. RC traffic is 45% of the wall.**

> Correction to the earlier `docs/perf-frontend-plan-2026-05-27.md`: that
> doc cited "1.4B increfs" and "~9s wall". Both were mismeasured (the wall
> is ~31s at -O2 / ~74s at -O0; the increfs are ~10.3B). The **45% RC
> share** is the figure that reproduced across every measurement and is the
> load-bearing diagnostic. Verified KAI_REF did NOT change these numbers
> (identical incref count with the runtime before and after the KAI_REF
> merge — the compiler does not use `Ref` in its own source).

## Root cause (located)

`st_unify` (infer.kai:7305) calls `st_set_sub(st, s2)` on every successful
unification — millions of times. `st_set_sub` (and the other `st_*`
reconstructors) rebuild the entire 18-field `InferState` record to change
one field, incref'ing the ~17 surviving pointer fields each time. ~28
`InferState { ... }` reconstruction sites in the file. That wrapper rebuild
is the bulk of the 10.3B increfs.

`Subst` (the `sub` field) is *already* mutable internally: `subst_extend`
(infer.kai:5246) does `array_set` in place on `s.slots`, but rebuilds the
5-field Subst wrapper and returns it. So the Array payload is shared and
mutated in place today; only the wrappers (Subst's 5 fields, InferState's
18) are rebuilt.

## The validated approach (asu)

1. `Subst.slots` / `row_slots` / `unit_slots` → `Ref[Array[...]]` (now that
   `Ref` is an honest one-slot `KAI_REF`). Counters `row_fresh` /
   `unit_fresh` stay by value.
2. `subst_extend` / `subst_row_extend` / `subst_unit_extend` mutate the Ref
   in place and return the **same** `s` (on `array_grow`, `ref_set(s.slots,
   grown)`). `ref_get` is pure (no Mutable on reads); `ref_set` carries
   Mutable but `subst_*_extend` already declare `/ Mutable`.
3. `st_unify`: `Some(_) -> st` — drop the 18-field InferState rebuild from
   the hot path entirely.
4. Do **not** touch the 132 `.slots` reads (they become `ref_get(s.slots)`,
   pure). Do not touch `st_set_env` / `st_bump_err` / `st_push_*` (cold;
   the profile says `st_set_sub` is the hot one).

## The risk linus flagged (MUST verify first, with a fresh head)

There are 6+ literal `Subst { ... }` construction sites. Some intentionally
**share** the slot Arrays (e.g. `subst_fresh_row` infer.kai:5286 rebuilds
the wrapper to bump a counter, sharing `slots`); others create **fresh**
Arrays (`subst_empty` infer.kai:5233). The correctness invariant: no
`subst_empty()` / `s_seed` that must be independent may start sharing the
`Ref` with the live state Subst.

The concrete trap: `mk_fresh_row_subst` (infer.kai:6531) returns
`FreshRowSubst { temp, sub }` where the caller (infer.kai ~7783) uses
**both simultaneously** — `temp` for `apply_ty`, `sub` for `st_set_sub`.
Today `temp` is born from `subst_empty()` (fresh Arrays) and `sub` shares
the live Arrays. Under the Ref scheme, this stays correct ONLY IF `temp`'s
fresh `subst_empty()` keeps its own independent `Ref`. Same for
`mk_fresh_unit_subst` (6563). **Before coding: grep every `Subst {` site,
classify share-Ref vs fresh-Ref, confirm the simultaneous-use sites
(`temp`/`sub`) do not alias the slot Ref.** linus: "15 minutes of analysis
that, at 11pm after two lanes, has non-trivial odds you skip one."

The aliasing of the slot Arrays already exists today (array_set mutates the
shared Array); the change does not introduce new Array aliasing — it only
stops rebuilding the wrappers. The risk is purely in the construction-site
classification.

## Gate (NOT byte-identical — codegen shape changes)

- incref count must fall materially with `-DKAI_PROFILE_RC` (from 10.3B;
  target a large fraction of the wrapper-rebuild increfs gone).
- selfhost fixed point: kaic2(new) compiles kaic2 → C1; that binary
  compiles kaic2 → C2; require C1 == C2 (self-consistency, not equality
  vs main).
- tier1 + ASAN green (mutation = potential aliasing/UAF; ASAN is the gate).
- Canary fixture: a `match` with backtracking (branches that unify
  differently), to catch a broken branch-isolation in `st_restore_entries`
  (infer.kai:7811) — `sub` is inner-side (propagates), so mutate-and-share
  is what restore wants, but verify.

## Files

infer.kai: Subst (5222), subst_empty (5233), subst_extend (5246),
subst_fresh_row (5286), subst_unit_extend (5300), mk_fresh_subst (6481),
mk_fresh_row_subst (6531), mk_fresh_unit_subst (6563), InferState (6831),
st_set_sub (6974), st_unify (7305), st_restore_entries (7811), and the
~132 `.slots` read sites (left as `ref_get`).
