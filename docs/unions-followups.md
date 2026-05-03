# Union types — follow-ups (post-#187)

Catalogues follow-up work from the unions milestone (#187, closed
2026-05-03). Tier 1 items have GitHub issues; Tier 2 and Tier 3
are documented here without issues to keep the tracker focused.

## Tier classification

| Tier | Meaning | Open issue when |
|---|---|---|
| **1** | Cleanup técnico inmediato. Code that left visible debt (dead code, model ambiguities, generic diagnostics). Feature works without it; next reader trips. | Open as GitHub issue right after milestone closes. |
| **2** | Improvements waiting on real usage. Decisions whose right answer depends on how people actually use the feature. No data today. | Open only if and when a real use case requests it. |
| **3** | Blocked by other issues. Features that depend on other pending work landing first (#180, #174). |  Don't open until blocker resolves. |

## Tier 1 — open issues

| Issue | Topic | Cost |
|---|---|---|
| [#197](https://github.com/lnds/kaikai/issues/197) | Resolver cleanup: skip ctor registration when variant name shadows existing type | ~30-50 lines |
| [#198](https://github.com/lnds/kaikai/issues/198) | Fold `[UnionInfo]` into typer (currently dead code) | ~50 lines |
| [#199](https://github.com/lnds/kaikai/issues/199) | D3-aware diagnostic phrasing on union mismatches | ~20-40 lines |

These three are bundlable into a single follow-up lane
`unions-cleanup-post-187` (~100-150 lines total). Recommended
sequencing: #198 first (refactor that the others can build on),
then #197 (uses the cleaner typer state), then #199 (uses both).

## Tier 2 — open when needed

### Option Y wrapper variants (`__From_C`)

**What**: an alternative codegen approach where union construction
wraps each component in a `__From_C(value)` variant tag, giving
unions a single-tag runtime layout.

**Why deferred**: Phase 4 chose Option X (structural narrow, no
new runtime layout). The current dual-rep approach + D3 upcast
covers all canonical end-to-end cases without Option Y. Adding
Option Y reshapes the runtime — high cost, no clear benefit
today.

**When to revisit**: a future feature requests a single-tag layout
(e.g., FFI interop with C structs, custom serialization that
needs uniform type tagging, or a hypothetical generic-union
implementation that requires it).

**Origin**: Phase 4 PR #193 "Out of scope" + lane retro
`docs/lane-experience-issue-187-phase4-codegen.md`.

### Migrate stdlib sum types to use unions where appropriate

**What**: opportunistically refactor stdlib `type T = A | B`
declarations to express composition via unions instead of wrapper
sum types, where the composition shape applies (rare in current
stdlib).

**Why deferred**: phase-0 audit found 0 in-tree wrappers that
would benefit. stdlib is already minimal and prefixed; no DRY
violation to fix. Migration would be cosmetic.

**When to revisit**: when ahu / kohau / manutara / hopu start
generating real bounded-context error types that compose, the
migration pattern can be applied to their stdlib touchpoints.

**Origin**: design doc `docs/unions-design.md` §"What's discarded
from #184" — stdlib migration explicitly out of v1.

## Tier 3 — blocked by other issues

### Generic unions (`type F[T] = Option[T] | Result[String, T]`)

**What**: union types parameterized by type variables.

**Blocker**: depends on #180 (`protocol P[A]` parametrized
protocols). Generic-union semantics interact with how generic
dispatch happens at type-application time.

**When unblocks**: after #180 lands. May still defer further if
the interaction proves complex.

**Origin**: design doc `docs/unions-design.md` §"Phases NOT in
v1".

### `impl P for U` where U is a union

**What**: protocol implementations that target union types
directly (instead of requiring per-component impls).

**Blocker**:
1. #180 (`protocol P[A]`) — needed for the dispatch model.
2. #174 (polymorphic-impl runtime panic) — affects any feature
   that dispatches at runtime.

**Why forbidden in v1**: protocols dispatch by the head type
tag; a union has no single head type. Today users implement `P`
for each component; the call site narrows before dispatch.

**When unblocks**: after both #180 and #174 resolve.

**Origin**: design doc `docs/unions-design.md` §"Interaction with
protocols (m12.8)".

## Cross-references

- Milestone #187 — closed 2026-05-03. Phases 0–5 in PRs
  #189–#194.
- Design doc: `docs/unions-design.md`.
- User-facing reference: `docs/unions.md`.
- Phase 0 audit: `docs/unions-phase0-audit.md`.
- Lane retros: `docs/lane-experience-issue-187-phase{2,3,4,5}-*.md`.
- Milestone rollup retro:
  `docs/lane-experience-issue-187-milestone.md`.
