# Lane experience — issue-203-phase0-audit

## Objective metrics

- Start: 2026-05-03T22:10:37-04:00
- End: 2026-05-03T22:17:55-04:00
- Wall-clock: ~438 s (~7 min)
- Build TSV (`/tmp/lane-issue-203-phase0-audit-builds.tsv`):
  empty header only — doc-only lane, no `make tier*` invocations
  to time.
- Output: `docs/m14-bootstrap-audit.md` (~440 lines), single
  doc-only PR.

## Audit findings highlights

The audit recommends **option E (direct rename, no bootstrap
split)**, rejecting all four options A–D enumerated in the issue
body. The recommendation rests on a single empirical finding:

**The premise that motivates option D is wrong.** The issue body
asserts that `stage1/compiler.kai` "contains 205+ references to
flat-prefix names" and "stage 2 source must be parseable by stage
1's parser". Both true on their face — but the bridge claim
"those references are stdlib calls that the rename must
preserve" does not hold.

Walking the actual code:

- `stage1/Makefile` invokes `kaic0 compiler.kai` with **zero
  `--prelude` flags**. Stage 1 is built with no stdlib loaded.
- `stage2/Makefile` likewise: `kaic1 compiler.kai`, no preludes.
- The flat-prefix names appearing in stage 1 / stage 2 source
  resolve to either:
  1. Self-defined helpers at the top of each file
     (`ch_is_digit`, `list_nth`, `list_has`, `string_ends_with`,
     `list_is_empty` — each declared with a `fn` line).
  2. **Stage 0 PRELUDE table entries** (`stage0/emit.c:425–467`):
     37 hardcoded names that the bootstrap C compiler lowers
     directly to `kai_prelude_*` runtime calls. These are
     runtime primitives, not stdlib functions.

Stripping out stage 0 builtins, **stage 1 has 23 stdlib-flat-name
references; stage 2 has 45.** All resolve to self-inlined
helpers. **Both compiler sources have zero genuine stdlib
dependencies.**

The migration surface is `stdlib/` itself + `examples/` +
`demos/` — about 545 references across ~108 files. The
bootstrap chain is structurally insulated from the rename.

This empirical finding collapses options A and D (both predicated
on "stage 1 must learn qualified calls or stdlib must split for
stage 1") into option E ("just do the rename, the bootstrap
chain doesn't care").

## Friction points

**Did the issue body's proposal D survive empirical scrutiny?**
No. Option D is an elaborate solution to a non-problem. Reading
only the issue body, D is plausible — there is a real
chicken-and-egg in *general* between "kaikai-minimal cannot parse
qualified calls" and "stage 2 source uses qualified calls". The
audit's job was checking whether stage 2 source actually has any
qualified calls (or any stdlib calls at all). It does not.

**Did §1 inventory reveal stdlib usage you didn't expect?** Yes —
in the opposite direction. I expected to find a long tail of
genuine stdlib calls in stage 2 (e.g. `list_take`, `opt_map`,
`result_and_then`) based on the issue body's framing. The grep
turned up zero. Every match in stage 2 for those names lived
inside comments / string literals belonging to the m6.2 resolver
documentation. That's how I realised the issue body had
counted self-helpers and stage 0 builtins together with stdlib
calls.

**Did §3 reveal a fifth option?** Yes — option E. It falls
directly out of §1's data. The issue body listed A/B/C/D and the
audit was structured to recommend among them, but once the
empirical case dissolved D's premise, A and C became
unmotivated, B was workable but not optimal, and option E
emerged as the clean answer. The audit names it explicitly so the
integrator can reject the issue body's framing without ambiguity.

## Spec ambiguities or interpretive choices

**How to scope `stdlib_minimal/` — strict subset of stage 1
needs, or copy-everything-flat?** Moot under option E (no
`stdlib_minimal/` is needed). The audit notes this in §3: option
C ("rename only what stage 1 doesn't use") becomes equivalent to
option E once §1's data is in, because "what stage 1 doesn't
use" is "everything in stdlib".

**How to treat the dual-name transition window in §4?** The
audit treats option B's "dual names forever" as
**transitional, per-PR-bounded** — each migration PR adds the
module-relative export, lands the consumer migration, and
removes the flat alias all in one PR. The dual window does not
span PRs. This is more aggressive than the issue body's tentative
"land qualified versions parallel for now" sketch, but matches
how `stdlib/core/list.kai` already evolved (the partial migration
visible in the file today).

**Six-PR phasing.** The audit picks one-module-per-PR over
larger batches. Smaller PRs are easier to review, easier to
revert, and align with the per-module structure of `stdlib/core/`.
The integrator can override (e.g. fold tuple + io into PR 6
trivially) without re-litigating the audit's verdict.

## Subjective summary

**Confidence in the verdict:** high. The empirical findings (§1
and §2 numbers) are mechanically reproducible by anyone with
`grep`, and they unambiguously falsify option D's premise.

**Hardest section:** §3. The challenge wasn't proving option E is
correct — that follows from §1 — but addressing options A–D in
their own terms first, so the integrator could see why each
fails on its own merits before option E is proposed. Option B
in particular is half-right (it's the right transition mechanism)
and needed careful handling so it gets reused under E rather than
rejected outright.

**Easiest section:** §1. Once `stage1/Makefile` was inspected
and shown to pass no preludes, the rest of the empirical case
followed by direct grep counts.

**Compiler help/hinder:** N/A (no compiler invocations on a
doc-only lane).

## Limitations of this report

- The 545-reference figure for the migration surface is an
  alternation-regex grep across the canonical flat-prefix name
  set. False negatives possible: any flat-prefix name that
  doesn't appear in `stdlib/core/*.kai`'s `pub fn` set (e.g. a
  one-off helper in a non-core stdlib module) is missed by the
  count. Spot checks against examples/ suggest the miss rate is
  small (<5%), but the rename will surface stragglers per-PR
  and they should be migrated as found.
- §4's per-PR phasing assumes a clean per-module split.
  `stdlib/core/list.kai` already entangles `length`/`list_length`
  with internal helpers like `length_loop`; migration PRs may
  need to rename internal helpers too for consistency. The audit
  declares internal-name renames out-of-scope but flags the
  question.
- The audit does not benchmark the migration. It assumes that
  m6.2 + UFCS together produce equivalent generated code for
  `string.concat(a, b)` and `string_concat(a, b)` after the
  rename — this is plausible (m6.2 documents the symbol-minting
  pipeline as a name change with identical lowering) but not
  measured here. If a future PR's `make selfhost` *does* shift
  byte output (which would be surprising under §4's argument),
  the audit's "selfhost is unaffected" claim needs revisiting.
- The audit does not evaluate the broader ecosystem impact
  (kaikai apps outside this repo). Risk 2 in §5 names the
  deprecation-cycle question but assumes a pre-1.0 minor bump
  is acceptable. If external users disagree, the milestone may
  need a longer dual-form window than per-PR-bounded.
