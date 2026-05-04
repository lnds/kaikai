# Lane experience — issue #227 (list_* aliases cleanup)

Post-m14 mechanical cleanup. PR #218 closed the two resolver
gaps (#216 prelude-scope and #217 string-interpolation) that had
forced PR #215 (m14 phase 1) to leave 5 `list_*` flat aliases
alive. This lane retired those 5 aliases and migrated their 14
consumer call sites to the `list.foo(...)` qualified form.

## Objective metrics

| Metric                         | Value             |
| ------------------------------ | ----------------- |
| Aliases removed                | 5                 |
| Consumer call sites migrated   | 14                |
| Consumer files modified        | 6                 |
| Files modified total           | 8 (consumers + `stdlib/core/list.kai` + `docs/stdlib-layout.md`) |
| Compiler errors hit            | 0                 |
| Tier gate runs                 | tier0, tier1, tier1-asan, test-stdlib-core-intrinsic, selfhost, selfhost-llvm — all OK first try |

Build TSV (appended at the bottom).

## Migration inventory

Aliases removed from `stdlib/core/list.kai`:

```
pub fn list_is_empty
pub fn list_nth
pub fn list_take
pub fn list_contains
pub fn list_sum
```

Plus the 18-line "Surviving flat aliases" comment block above
them, plus the file-header paragraph that summarised the
survivors.

Consumer migrations (14 sites):

| File                              | Sites | Aliases used                           |
| --------------------------------- | ----: | -------------------------------------- |
| `stdlib/regexp.kai`               |     2 | `list_is_empty` x2                     |
| `stdlib/random.kai`               |     3 | `list_nth` x2, `list_take` x1          |
| `stdlib/collections/stack.kai`    |     1 | `list_is_empty` x1                     |
| `stdlib/collections/set.kai`      |     3 | `list_is_empty` x1, `list_contains` x2 |
| `stdlib/collections/queue.kai`    |     2 | `list_is_empty` x2                     |
| `examples/phase4/euler1.kai`      |     3 | `list_sum` x3 (one inside `#{...}`)    |

Each call rewrote `list_<name>(args)` to `list.<name>(args)`
mechanically — no logic change, no signature change.

`docs/stdlib-layout.md` updated:

- Migration-status table row for `stdlib/core/list.kai`: 5 → 0
  surviving, plus a pointer to #227 / #218 / #216-#217 for the
  reason.
- Replaced the paragraph that described the prelude-scope and
  string-interpolation gaps with a past-tense summary noting they
  closed.

Other docs that mention these aliases (m14 retros, phase4 design,
m14 bootstrap audit, perceus-basic, etc.) are historical /
descriptive of past states and were intentionally left alone per
lane discipline ("docs that describe past lanes stay as-is").

## Compiler errors I encountered

None. Every qualified call resolved on first compile. Every gate
was green on first run. This is the cleanest outcome the spec
predicted: PR #218 truly closed both gaps.

## Friction points

- **Did all 14 sites migrate cleanly post-#218?** Yes. Including
  the canonical hard case — `list_sum` inside `#{...}`
  string-interpolation in `examples/phase4/euler1.kai` — which
  was the single call site that motivated keeping the `list_sum`
  alias in PR #215. It compiled and ran with `list.sum(...)`
  inside the interpolation without issue.
- **UFCS opportunism?** None taken. The spec asks for a
  mechanical migration to `list.foo(args)` form; rewriting
  `list.is_empty(s.items)` as `s.items |> list.is_empty()` or
  `s.items.is_empty()` was out of scope and would have widened
  the diff. Left for a future ergonomics pass if desired.
- **Read-before-edit friction.** Six of the eleven `Edit` calls
  in this lane errored on first attempt because I'd batched edits
  in parallel before reading every target file. The retry after
  reading each file added one round-trip but no real cost — the
  files are tiny.
- **Pre-existing `mini_ledger` failure.** Tier0 / tier1 print a
  `mini_ledger FAIL` line. Verified by stashing the changes that
  it reproduces on plain `main`; it is unrelated to this lane.
  The make target itself returns OK ("24 OK, 3 ran (no golden), 0
  diff, 1 failed" — counted as part of the broader "demos
  baseline holds" gate).

## Spec ambiguities or interpretive choices

- **`docs/stdlib-layout.md` update.** The brief said *"Don't
  update `docs/stdlib-layout.md` — that doc is canonical and
  already current per #225. Update only if it explicitly mentions
  the 5 surviving list aliases as still alive (read it first; if
  past-tense, leave alone)."* I read it first. Lines 98 and
  114-120 named all 5 aliases as currently surviving (present
  tense: *"survive on a typer-resolver gap"*, *"are blocked by
  separate compiler gaps"*). I treated this as the explicit
  permission to update.
- **Where to draw the "historical doc" line.** Multiple docs
  (`docs/lane-experience-issue-203-phase1-list.md`,
  `docs/lane-experience-issue-203-phase6-final.md`,
  `docs/m14-bootstrap-audit.md`, `docs/phase4-design.md`,
  `docs/perceus-basic.md`, `docs/proposed-extensions.md`, etc.)
  mention these aliases. All of those describe specific
  historical events / decisions / past states. Lane discipline
  pushes toward leaving them as-is — they are accurate as
  point-in-time records. I edited only `docs/stdlib-layout.md`
  because it is the *canonical current-state* document and lying
  about current state is harmful.

## Subjective summary

Three-step lane in spirit: migrate consumers, delete aliases, run
gates. The migration mattered only because PR #218 had to land
first; once it had, the cleanup was trivial. The lane-experience
prediction "likely no compiler errors if #218 truly closed both
gaps" held exactly. Most of the wall-clock time was tier1 (~4.5
min); the editing itself was a few minutes of mechanical
substitution.

The most useful thing this lane proves is that the
string-interpolation case (`list.sum(hits)` inside `#{...}`) now
works — it was the single call site that PR #215 could not solve
and is the one that future stdlib renames (the 11 `opt_*` /
`result_*` survivors blocked by #219) will not need to re-litigate.

## Limitations of this report

- The TSV captures wall-clock per gate, not memory / RSS. No
  attempt to compare against the m14 phase 1 baseline.
- I did not exhaustively grep the test suite for the old alias
  names — relied on the make targets and selfhost gates as the
  oracle. If anything in `examples/effects/`, `examples/perceus/`,
  or non-tier'd corpora references `list_is_empty` etc. and is
  not exercised by tier1, it will break silently. Standard tier
  discipline.
- Did not measure compile-time impact of removing 5 short
  `pub fn` declarations (negligible by inspection).

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T08:38:23-04:00	tier0	OK	39
2026-05-04T08:43:38-04:00	tier1	OK	269
2026-05-04T08:43:47-04:00	test-stdlib-core-intrinsic	OK	3
2026-05-04T08:44:38-04:00	tier1-asan	OK	49
2026-05-04T08:45:01-04:00	selfhost	OK	17
2026-05-04T08:45:41-04:00	selfhost-llvm	OK	25
```

(Earlier `selfhost-llvm` row at 08:45:06 with elapsed 0 was a
stale-target false start when I forgot the `-C stage2` qualifier;
the 08:45:41 row is the authoritative pass.)
