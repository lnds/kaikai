# Lane experience — multi-arg-match duplicate `default:` (issue #129 follow-up)

## Scope as planned vs. as shipped

**Planned (from the 2026-06-03 bugfix handoff):** close the CI red
`multiple default labels in one switch`, which fired on three fixtures
(`sugars/case_block_multi_arg`, `match/multi_arg_basic`,
`unions/issue_430_multiclause_upcast_first_arm_narrower`). The handoff
hypothesised the root was in the multi-arg `match a, b { ... }` desugar
(parser-only sugar, issue #129) chaining arms wrongly so that two
sibling `default:` cases landed in the same emitted C `switch`.

**Shipped:** exactly that. A two-function change in
`stage2/compiler/parse.kai`'s `build_marm_columns`, plus two small
helper predicates. No backend change — the C / LLVM emitters were
faithfully rendering whatever arm list the desugar handed them; the
duplicate `default:` was upstream, in the AST the desugar built.

## Root cause

`build_marm_columns` lowers one multi-arg arm into a nested chain of
single-scrutinee `EMatch`es, one per column. At each column it emitted
**two** arms unconditionally:

```
match sv { <column-pattern> -> <inner> | _ -> fall }
```

When the column pattern was itself irrefutable — a `PWild` (`_`) or a
`PBind` (a name) — the first arm already captured every value, so the
`_ -> fall` sibling was dead. The C backend renders an irrefutable
`PWild`/`PBind` arm as a `default:` (issue #91 fast path), so an
irrefutable column followed by the dead wildcard produced **two
`default:` in one `switch`** — a clang hard error.

`classify2` in `match/multi_arg_basic.kai` triggered it directly: arm
`(0, _)` has a wildcard in the inner column, so the inner match became
`match s1 { _ -> "first-zero" | _ -> fall }` — two defaults.

## Fix

Drop the dead wildcard sibling when the column pattern is irrefutable
**and** the arm in that position carries no guard:

- A column is irrefutable when it is `PWild`, `PBind(_)`, or
  `PAs(_, inner)` with an irrefutable inner (`pat_is_irrefutable`).
- The guard only attaches to the innermost column (last pattern);
  interior columns always pass `None`, so they always collapse when
  irrefutable. The innermost column keeps both arms when a guard is
  present, since a false guard genuinely re-routes to `fall`
  (`guard_cannot_fail_here`).

When both conditions hold the match collapses to a single irrefutable
arm `EMatch(sv, [inner])`. The fall-through subtree stays reachable
because the recursion still threads `fall` into the inner columns'
bodies — we only removed an arm that could never be reached, never a
live path.

## Design decisions / alternatives considered

- **Fix in the desugar, not the backend.** The backend emitting a
  `default:` per irrefutable arm is correct in isolation; the bug was
  feeding it two such arms. Patching the backend to dedup sibling
  defaults would have masked the same dead-code shape elsewhere
  (hand-written nested matches) and added a backend special case for an
  AST that should never have been built. The desugar is the single
  producer of this shape, so the fix belongs there.
- **Collapse to a single-arm `EMatch`, not to the bare body.** A
  `PBind` column binds a name the arm body may read; dropping the
  `EMatch` entirely would lose the binding. Keeping a one-arm `EMatch`
  preserves the bind and routes through the existing fast path, which
  already handles a lone `PBind`/`PWild` arm as a `default:` block.
- **Guard handling kept minimal.** Rather than reason about whether a
  guard is statically true, treat any present guard as fallible and
  keep the wildcard. Conservative, sound, and matches single-arg match
  semantics (a guard failure falls to the next arm).

## Structural surprises

- The compiler is now modularised (`stage2/compiler/*.kai`); the
  desugar lives in `parse.kai`, not a monolithic `compiler.kai`. The
  handoff's line references were to the *emitted C*, not the source.
- The emitted-C read was the fastest diagnosis: `grep -n "default:"`
  on the output showed two consecutive defaults at the same nesting,
  which pinned the column where the desugar over-generated.

## Fixtures

No new fixtures needed — the three pre-existing ones
(`sugars/case_block_multi_arg`, `match/multi_arg_basic`,
`unions/issue_430_multiclause_upcast_first_arm_narrower`) already
encode the bug shape (interior wildcard column, shared-outer-pattern
interleaving, guard-on-multi-arg). All three now pass on **both**
backends (C and LLVM), verified end-to-end (emit → cc → run → diff
golden). `match/multi_arg_basic` is the load-bearing one: its
`classify2` interleaving case is exactly the wildcard-inner-column
shape that produced the duplicate default.

## Coverage gaps

The fixtures cover wildcard and guard columns. Not covered: a `PAs`
column in a multi-arg arm (`x @ _, y -> ...`) — `pat_is_irrefutable`
handles it recursively but no fixture exercises it. Low priority: `@`
in a multi-arg column is an unusual shape and the recursion is a
straightforward structural reading.

## Follow-ups

None opened by this lane. The fix is confined to the desugar and does
not touch the typer, the backends, or the runtime.
