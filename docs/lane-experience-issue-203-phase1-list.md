# Lane experience report — issue-203-phase1-list

This is the lane retro for PR 1 of the m14 milestone (issue #203):
migration of `stdlib/core/list.kai` flat aliases (`list_*`) to the
module-relative form (`list.*`) across all consumers.

## Objective metrics

| Metric                        | Value                       |
| ----------------------------- | --------------------------- |
| Lane start                    | 2026-05-03T22:28:55-04:00   |
| Lane end                      | 2026-05-03T22:55:46-04:00   |
| Wall clock                    | ~27 minutes                 |
| `make tier0` invocations      | 2 (both OK; 29s, 35s)       |
| `make tier1` invocations      | 2 (both OK; 239s, 262s)     |
| `make tier1-asan` invocations | 1 (OK; 50s)                 |
| `make selfhost` invocations   | 1 (OK; 18s)                 |

The first `tier0` ran on a draft that broke 28 demos. The second
`tier0` ran on the corrected scope (preludes reverted) and held
the demos baseline at 27 passing. The first `tier1` failed on
`examples/phase4/euler1.kai` (string-interpolation gap, see below);
the second `tier1` ran after reverting that file and was green.

The full TSV log is appended at the end of this document.

## Migration inventory

| Quantity                                       | Value |
| ---------------------------------------------- | ----- |
| Flat aliases originally in `stdlib/core/list.kai` | 29 |
| Flat aliases removed                           | 24    |
| Flat aliases surviving (resolver gaps)         | 5     |
| Consumer files modified                        | 12    |
| Consumer call sites migrated to `list.*`       | ~98   |

The 24 aliases removed: `list_any`, `list_all`, `list_concat`,
`list_count`, `list_drop`, `list_drop_while`, `list_find`,
`list_flat_map`, `list_head`, `list_index_of`, `list_map_indexed`,
`list_max`, `list_max_by`, `list_merge_by`, `list_min`,
`list_min_by`, `list_product`, `list_repeat`, `list_sort`,
`list_sort_by`, `list_tail`, `list_take_while`, `list_uniq`,
`list_zip_with`.

The 5 aliases surviving (with the gap that blocks them):

| Alias            | Blocked by                  |
| ---------------- | --------------------------- |
| `list_is_empty`  | prelude-scope resolver gap  |
| `list_nth`       | prelude-scope resolver gap  |
| `list_take`      | prelude-scope resolver gap  |
| `list_contains`  | prelude-scope resolver gap  |
| `list_sum`       | string-interpolation gap    |

## Compiler errors I encountered

### Class 1 — `error: undefined name \`list\`` from prelude files

**Where**: every demo and main-file fixture failed at compile time
when stage 2 tried to resolve `list.foo` in any of these prelude
files: `stdlib/random.kai`, `stdlib/regexp.kai`,
`stdlib/collections/{queue,set,stack}.kai`. The error pointed at
the *main* file (a comment, in some cases), but the actual
unresolved reference lived inside a prelude.

**Diagnosis**: I re-ran the same prelude chain on `main` (no lane
diff) and reproduced the error. Stage 2's qualified-call resolver
only fires for the top-level main compilation unit; prelude files
cannot reach `list.foo`.

**Fix**: reverted my changes to all five prelude files; restored
the four flat aliases they need (`list_is_empty`, `list_nth`,
`list_take`, `list_contains`).

**Attempts**: 1 revert. The class became invisible after the
revert.

### Class 2 — `error: undefined name \`list\`` inside `#{…}` interpolation

**Where**: `examples/phase4/euler1.kai:11`:

```kaikai
println("sum = #{int_to_string(list.sum(hits))}")
```

**Diagnosis**: I extracted a minimal repro and stashed the lane
diff. Same error on `main`. Hoisting `list.sum(hits)` into a
`let` outside the interpolation also fixed it. So the gap is
specific to qualified-call resolution inside `#{…}` segments.

**Fix**: reverted `examples/phase4/euler1.kai`; kept `pub fn
list_sum` alive in `list.kai` for that one call site.

**Attempts**: 1 revert. Confirmed on the second `tier1` run.

## Friction points

- **Did `stdlib/core/list.kai` actually export both forms as audit
  §4 claimed?** Yes, the module-relative exports were already
  present (`pub fn map`, `pub fn filter`, `pub fn length`,
  `pub fn reverse`, `pub fn head`, `pub fn nth`, …). The 29 flat
  aliases at the bottom of the file delegated to those bare-name
  exports. Parity was real; the audit's claim about the source
  surface was correct.

- **Did any consumer break that suggested a semantic difference
  between flat and module-relative?** Not a semantic difference in
  the *implementation* (the aliases were one-line wrappers calling
  the bare name with the same arguments), but two **resolver**
  gaps that the audit did not catch. From the consumer's
  perspective `list_foo(xs)` and `list.foo(xs)` are not
  interchangeable in every context — see the two error classes
  above. The audit assumed they were.

- **Did UFCS get used opportunistically?** No. The lane scope said
  the primary form is `list.foo(xs)` and UFCS (`xs.foo()`) is
  opportunistic; in the consumer files I migrated, the conversion
  was a mechanical name swap and no call site read more clearly
  in the UFCS form. I left every site as `list.foo(xs, …)`.

## Spec ambiguities or interpretive choices

- **All flat aliases removed, or some kept?** Briefing said
  "remove the flat-name `pub fn` exports … if any are
  private-helper-shaped, leave alone (per audit §4 transitional
  rule)." None of the 29 were private-helper-shaped (all `pub fn`).
  But I kept 5 alive because of the two compiler resolver gaps
  uncovered above. The decision tree:

  - 4 (`list_is_empty`, `list_nth`, `list_take`, `list_contains`)
    → kept because their callers live in prelude scope, where the
    qualified-call resolver does not fire today.
  - 1 (`list_sum`) → kept because its only consumer call site is
    inside a `#{…}` interpolation, where the qualified-call
    resolver also does not fire today.

  Both gaps are stage-2 resolver bugs, not language design choices.
  The aliases retire when those bugs land; the comment block at
  the foot of `stdlib/core/list.kai` documents the rationale.

- **How did I handle re-exported functions like `list_concat` if
  they delegate to other modules?** None of the aliases delegate
  cross-module. Every flat alias was a one-liner calling its
  bare-name twin (`pub fn list_concat(xss) = concat(xss)`), all
  defined within `list.kai`. No re-export reasoning needed.

## Subjective summary

- **Confidence**: high on the migration itself; the changes are
  mechanical name swaps with no behavioral surface area. Low on
  the auditor's unbroken inheritance — the audit's premise that
  `list.foo` works wherever `list_foo` works is empirically
  wrong, and the next phases (string, option, result, char,
  tuple, io) will hit the same two gaps unless the resolver is
  fixed first.
- **Hardest**: figuring out that the prelude-scope gap and the
  interpolation gap were pre-existing bugs in `main`, not
  regressions from my diff. Both error messages pointed at the
  consumer file (sometimes at a comment line) instead of at the
  prelude or interpolation segment, which inverted the natural
  bisection direction.
- **Easiest**: the consumer name swap. `perl -i -pe` over the 12
  consumer files was a single command; the longest-name-first
  ordering avoided every prefix collision (`list_max_by` before
  `list_max`, `list_take_while` before `list_take`).
- **Compiler help/hinder**: hindered. The "did you mean …?"
  hints suggested local variable names (`hits`, `xs`, `i`) rather
  than reading like "this looks like a module reference and the
  resolver does not see modules in this scope." Pointing at the
  consumer file's first comment when the failure originated in a
  prelude file is misleading; the column is also off by enough
  bytes that it does not even land on a token. A clearer error
  for unresolved qualified calls would have shaved most of the
  bisection time.

## Limitations of this report

- I did not file follow-up issues for the two resolver gaps; that
  is a comment on issue #203 by the integrator. This report
  describes what I observed, not what should be done about it.
- I did not measure the size of the resolver fixes; I only
  located where in the pipeline they would land (the
  qualified-call resolver in `stage2/compiler.kai`, near
  `rename_proto_calls_kind` and the prefix-fallback hook).
- I did not run the `make daily` (Tier 2) gate. Briefing did not
  require it.
- The retro is written immediately at lane close, so my "what
  felt hardest / easiest" answers are subject to recency bias.

## Build TSV (appended)

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T22:32:15-04:00	tier0	OK	29
2026-05-03T22:36:48-04:00	tier0	OK	35
2026-05-03T22:40:53-04:00	tier1	OK	239
2026-05-03T22:54:07-04:00	tier1	OK	262
2026-05-03T22:54:33-04:00	selfhost	OK	18
2026-05-03T22:55:30-04:00	tier1-asan	OK	50
```
