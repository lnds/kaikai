# Lane experience — issue #1015 L3: collection-construction reference (docs)

Doc-only lane; closes #1015 as the last of three (L1 stdlib, L2 parser,
L3 docs). Short retro by design.

## Scope as planned vs as shipped

**Planned (brief):** one coherent Collections section in
`docs/info/syntax.md` (the `kai info syntax` page) covering the seven
construction surfaces — immutable `%{…}` / `%[…]` literals + empties,
`...` spread, `m[k]` get sugar + the rejected immutable write, runtime
`X.from`, the four `.to_X` conversions, the immutable/mutable split, and
a timeless note on the `%` sigil and `:` separator. Consolidate (not
duplicate) the minimal entries L1 and L2 left in the page. Every kaikai
fence must compile.

**Shipped:** identical. A new `## Collections` section with four
subsections (*Literals*, *Building from a runtime list*, *Immutable and
mutable*, *Why the `%` sigil and the `:` separator*), three full-program
`kaikai` fences, and prose for the non-fenced facts (rejected indexed
write, source-untouched invariant). The L2 entry that had been parked
inside *Literals and operators* was lifted out and replaced by a
one-line cross-reference, so the page holds one coherent section, not
two fragments.

## Design decisions

- **Section, not a new topic.** The brief left `kai info collections` as
  an option. The issue contract said "new Collections section in
  `kai info syntax`", and topics are auto-discovered from
  `docs/info/*.md` (no registry to touch), so a dedicated section in the
  existing page is both what was asked and the lower-surface choice. No
  new file, no registration.
- **Snippets copied from shipped fixtures, not invented.** Every fence
  was assembled from the L1/L2 fixtures already in the tree
  (`examples/sugars/coll_literal_*.kai`,
  `examples/stdlib/collections_to_conversions.kai`,
  `array_from_no_sentinel.kai`) and the real stdlib signatures, then
  compiled before landing — no extrapolation.

## Structural surprises

None for the docs themselves. One worth recording for future doc lanes:
the `array.from` and `.to_X` examples carry `Mutable` in the `main` row
(`array_make`/`hashmap.put`/`hashset.add` write locally), so a doc fence
that uses them must annotate `: Unit / Stdout + Mutable` or `kai build`
rejects it. The literal-only fence stays pure (`Unit / Stdout`). The
immutable `Map` write `m[k] := v` is a hard compile error pointing at
`map.put`, so it is documented in prose, not as a fence.

## Fixtures and coverage

No new `examples/` fixtures — the construction surfaces already have
L1/L2 fixtures wired into their tiers. The doc fences are themselves the
coverage here: `tools/test-info-blocks.sh` compiles and links every
`kaikai` fence in `docs/info/*.md` on the path-gated CI tier, so the
three new fences are regression-guarded. Verified locally: the full
suite passes (100/100 blocks).

## Cost vs estimate

Lower than a code lane, as expected — no `kaic2` rebuild needed beyond
the one the first `kai build` triggers. The work was reading the
shipped fixtures for exact syntax, compiling each fence, and writing
prose.

## Follow-ups

None for #1015 — the uniform construction surface is complete (L1
stdlib + L2 literals + L3 docs). The `m[k] := v` rejection message
references a "v2 design alongside the HAMT carrier"; if that lands, the
*Literals* subsection's note about rebinding via `map.put` is the line
to revisit.
