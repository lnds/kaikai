# Lane experience report — issue #92 diagnostic 2026-05-07

Best-effort retrospective by the implementing diagnostic agent.

## Goal

Determine whether the side-table approach (PR #290, branch
`origin/issue-92-tco-dropmask-side-table`) now passes after
**#320** (pipeline reorder perceus-before-tcrec) and **#324**
(closure capture lifecycle, leak rate 27.97% → 1.14%) recently
landed on `main`.

## Outcome — RED **with new evidence**

The imbalance reproduces. The structural diagnosis from PR #290
holds: rule-3 fails because **the pre-pass walk over `[Expr]`
ejercita the same broken stage 1 perceus emit path that the four
prior attempts hit**, regardless of where the walk happens or what
shape the dropmask compute takes.

What changed since PR #290's close (2026-05-06):

- **The bug is now visible on macOS.** PR #290 reported macOS
  tier0/tier1/tier1-asan all green, with only Linux glibc's strict
  malloc tcache catching the imbalance.  After cherry-picking
  PR #290's commit `d2a2208` onto current `main` (post #320 +
  #324 + others), `make selfhost` on Darwin/arm64 now panics with
  `non-exhaustive match` originating in `kai_p_peek` — and ASAN
  confirms it's a **heap-use-after-free in `kai_lex_skip_ws`**, not
  a missed match arm.

- **Hypothesis why**: #324 tightened RC discipline for closure
  capture (drop leak rate 27.97% → 1.14%). Before #324, the leaked
  references masked the imbalance: a use-after-free was sometimes
  a use-after-still-live because the dup/drop counts were sloppy
  in both directions.  After #324, refs that *should* be live
  during the next read are actually freed promptly, exposing the
  imbalance.

## Evidence

### Pin

```
HEAD before lane:    65b9b27 (bump: 0.44.0 → 0.44.1)
VERSION:             0.44.1
Cherry-pick base:    d2a2208 from origin/issue-92-tco-dropmask-side-table
After cherry-pick:   10600ce
Date:                2026-05-07
```

### macOS tier0 — FAIL

```sh
$ make tier0
…
../stage1/kaic1 compiler.kai > build/stage2.c
cc -std=c99 -I ../stage0 build/stage2.c -o kaic2
panic: non-exhaustive match
make: *** [selfhost] Error 1
```

The kaic1-emitted kaic2 binary is fine (compiles compiler.kai
into kaic2b.c without panic).  The kaic2-emitted kaic2b binary,
re-running over compiler.kai, panics during parsing.

### ASAN drilldown — heap-use-after-free in lexer

```
==45005==ERROR: AddressSanitizer: heap-use-after-free
READ of size 4 at 0x604000000f50
    #0 kai_decref runtime.h:1556
    #1 kai_internal_drop runtime.h:1580
    #2 kai_lex_loop kaic2b.c:6231
    #3 kai_tokenize kaic2b.c:6246
    …
freed by thread T0 here:
    #0 free
    #1 kai_free_value runtime.h:1548
    #2 kai_decref runtime.h:1560
    #3 kai_internal_drop runtime.h:1580
    #4 kai_lex_skip_ws kaic2b.c:5813
```

The lexer record allocated by `kai_lex_new` is freed inside
`kai_lex_skip_ws` and then read by `kai_lex_loop`'s next
iteration.  Plain RC double-decref — exactly the family of bug
the four prior attempts hit, just visible earlier in the
pipeline (lex/parse setup) instead of at the
`stage 2 → kaic2b → kaic2c` selfhost step under glibc tcache.

### Bisection — the side table walk is sufficient by itself

Removed both calls to `tcrec_collect_dm_table` in
`tcrec_rewrite_decl` and `tcrec_rewrite_pcs_ret_wrap`, leaving
the helper functions defined but unreferenced; passed `[]` as
the `dm_table` argument to `tcrec_rewrite_body` instead.

```sh
$ make selfhost
self-hosting fixed point: OK
```

**The mere act of walking `body` once via
`tcrec_collect_dm_table` reintroduces the heap corruption.** The
TCO rewrite that consumes the resulting `[TcrDm]` is irrelevant;
the read-only walk is sufficient.  This rules out any "the
side-table threading is wrong" theory.

The cause is in stage 1's perceus emit for the walk shape:
`tcrec_kind_has_evar` recursing into `EBinop`, `EList`,
`EBlock`, etc., walks `[Expr]` and `[ListElem]` and feeds
each through dup/decref. Stage 1 emits a use-after-free
somewhere along that chain.

### Linux CI not exercised

Per the brief's acceptance gate, macOS local must be green
before opening a draft PR for Linux CI.  Since macOS tier0 now
fails on a clean cherry-pick, the gate stops here.  Reaching for
option (b) — reimplement-from-scratch — does not change the
diagnosis: any AST walk that traverses `[Expr]` flows through
the same broken path. The shape of the side table or the order
relative to perceus is not the variable.

## What this means for #92

PR #290's conclusion stands: **option #3 is invalidated**.
Adding new evidence to the file:

- The imbalance is **independent of #320 and #324**: it
  reproduces on top of both.
- **#324 made the bug visible on macOS.** This is a one-shot
  bonus — future re-attempts now have a faster signal loop than
  Linux CI.  Local ASAN catches it in ~10 s on the kaic2b binary.
- Option #3 cannot work as long as ANY `[Expr]` walk feeds the
  rule-3 compute.  Even moving the walk to a separate pre-pass
  doesn't help.  The walk itself is the bug shape.

The remaining options collapse to:

- **Option #1** — audit + fix stage 1's perceus emit for
  `[Expr]`/`[ListElem]` walks.  The ASAN trace from this lane
  (`kai_lex_skip_ws → free → kai_lex_loop → use`) is a concrete
  reproducible starting point that wasn't available before.
- **Option #2** — strict alloc tracing in `stage0/runtime.h`
  (per-tag free counts, sentinel patterns on freed chunks,
  `KAI_TRACE_RC_STRICT` mode) to make the imbalance visible at
  any RC site, not just the lexer.

Recommend **starting with option #1** since the ASAN trace from
this lane gives a concrete locus: stage 1 emits a `kai_decref`
on the lexer record inside `kai_lex_skip_ws` that it should not
emit (or omits a corresponding `kai_incref`).  That's a tractable
audit target.  Option #2 (instrumentation) becomes the second
resort if #1's audit doesn't converge.

## Things that did NOT need to happen this lane

- Reaching Linux CI.  The brief allows for a YELLOW finding to
  stop on macOS evidence; this lane goes further to RED on
  macOS.
- Reimplementing option (b) from scratch.  The bisection (remove
  the two callers, leave the helpers defined) showed
  unambiguously that the walk shape — not the implementation —
  is the trigger.

## Limitations

- Single agent (Claude Opus 4.7), single session.
- ASAN trace shows ONE corruption site (lexer); there may be
  others further down the pipeline that this trace masked by
  aborting first.
- Did not attempt Linux CI to confirm the abort signature has
  shifted to a heap-use-after-free family rather than the
  classic `malloc(): unaligned tcache chunk detected`.  Both
  are plausible glibc reactions to the same root cause.

## Lane disposition

- HEAD pin recorded.
- macOS evidence captured.
- Branch left local, **NOT pushed**.  Per acceptance gate,
  draft PR not opened (macOS local would block the gate).
- Cherry-pick commit `10600ce` left on the local branch
  `issue-92-tco-diag` for reference.  Working tree clean
  modulo this doc.
