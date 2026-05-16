# Edition flip: tongariki → hanga-roa (2026-05-16)

## Decision

The default edition flips from `tongariki` to `hanga-roa` ahead of the
nominal 2026-05-21 target date. Effective with the commit landing this
decision doc and the `EDITION` file change.

## Context

The `EDITION` file at repo root names the default edition the compiler
assumes when a project's `kai.toml` does not declare one. Until this
decision it read `tongariki`. The plan documented in `docs/editions.md`
called for the flip to `hanga-roa` to coincide with the
2026-05-21 nominal release date.

Two observations made the early flip the right call:

1. **The user-visible surface is already stable.** Every load-bearing
   Hanga Roa feature shipped between v0.65.0 and v0.69.0:
   - #594 convention-based pipe dispatch (the *only* compiler branch
     gated on `edition >= hanga-roa`).
   - #603 multi-edition compiler dispatch + `kai.toml` field
     enforcement.
   - #605 + #357 HTTP server-side + redirect-following.
   - #611 + #620 + #630 reactor R1+R2+R3.
   - #602 `#unstable` annotation.
   - #643 + #647 + #648 private-type leak fixes.
   - #644 constructor overloading at use sites.
   - #645 pure-named-function auto-generalization over effect rows.
   - #649 release pipeline mirror fix.
   - #604 docs honesty audit.

   The surface a user writes against is what an `hanga-roa` package
   would write against starting today; no breaking change is planned
   in the next five days.

2. **The flip is monotonic.** Searching `stage2/compiler.kai` for
   `edition_at_least` produced exactly one branch — #594's pipe
   dispatch enrichment. Hanga-Roa code paths *add* capability over
   tongariki paths; they do not reinterpret valid programs. Code that
   compiled under tongariki keeps compiling under hanga-roa.

   `ahu` declares `edition = "hanga-roa"` in its manifest already, so
   the flip leaves it unchanged. `kaikai-book`'s package examples
   (cap 17 + cap 18 in both EN and ES) carry no `edition = …` line,
   so they migrate to `hanga-roa` automatically with the flip and
   gain access to #594-style pipes without further work.

## Migration

- **kaikai work repo**: `EDITION` rewritten to `hanga-roa`; this doc
  recorded; `docs/editions.md` "Current edition timeline" row for
  Tongariki marked closed, row for Hanga Roa marked active.

- **Downstream packages**:
  - `ahu` — no change (already declares `hanga-roa`).
  - `kaikai-book` example packages — no change required; they pick
    up the new default automatically.
  - Anyone with an existing `kai.toml` declaring `edition = "tongariki"`
    keeps working without alteration (the compiler still accepts
    tongariki via `KNOWN_EDITIONS`).

- **Release**: the next `cz bump` will produce v0.70.0 (MINOR
  because the flip is a `feat` not a breaking change — within Hanga
  Roa, no edition contract is violated by gaining a default).

## What this is not

- This is not a 1.0 release. Hanga Roa edition runs on the v0.x
  series; 1.0 versioning is a separate decision.
- This is not a feature freeze. Bug fixes and refactors continue;
  only the **user-visible surface contract** is now stable per the
  `hanga-roa` row in `docs/editions.md`.
- This is not Orongo. Items deferred to Orongo (Phase A.1+ cache
  layers, struct-by-value FFI, advanced numeric primitives, LLVM
  direct emit, compilation daemon, RFC #638 timeout-bounded
  `Actor.receive`, `kai migrate`, NetDns/NetUdp, m14 full migration)
  remain deferred.

## Rollback

If a breaking change to the Hanga Roa surface becomes necessary
before public adoption, the flip can be reverted with a single
commit setting `EDITION` back to `tongariki` and a corresponding
update to `docs/editions.md`. Downstream packages declaring
`edition = "hanga-roa"` would continue to compile against the
preserved hanga-roa rule set under the multi-edition dispatch path
(#603) — the rollback only affects the default for packages that
do not declare an edition.

## Related

- `docs/editions.md` — the canonical edition policy doc.
- `docs/decisions/editions-stability-without-stagnation-2026-05-15.md`
  — the prior decision establishing the edition mechanism.
- #594, #603, #644, #645, #643, #647, #648 — the Hanga Roa
  contract items.
