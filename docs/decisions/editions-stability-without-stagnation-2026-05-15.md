# Decision — Adopt "stability without stagnation" via editions

**Date:** 2026-05-15
**Decided by:** Eduardo Díaz, with project context
**Status:** Accepted

## Decision

kaikai adopts **stability without stagnation** as a Tier 1 (load-bearing)
principle, adapted from Rust's
[2014 stability manifesto](https://blog.rust-lang.org/2014/10/30/Stability/).
The mechanism is **editions**: named snapshots of the user-visible
language surface and stdlib API, ordered chronologically with
geographic Rapa Nui names matching the kaikai naming family.

- The current edition is **Tongariki** (v0.x series leading up to the
  21 May 2026 release).
- The 21 May 2026 release will tag the **Hanga Roa** edition. It is
  not v1.0.0 — it is whatever v0.x.y the bump cadence reaches on
  that date, labelled `hanga-roa`.
- The edition after Hanga Roa will be named **Orongo**. It will pull
  in items deferred from Hanga Roa (advanced FFI, new numeric
  primitives, LLVM direct emit, Phase A.2 cache, compilation
  daemon, etc.).

Edition names are Rapa Nui geographic terms. The naming family
matches kaikai itself (Rapa Nui string-figure tradition) and the
ecosystem packages (ahu, kohau, henua, manutara, hopu).

## Why now

Three forcing functions converged in mid-May 2026:

1. **Downstream packages came online.** ahu shipped Tongariki on
   2026-05-03. henua scaffolded 2026-05-14. kohau repo created
   2026-05-14. Three packages now consume kaikai's surface. Once
   they have downstream users of their own, our freedom to break
   their dependency without warning ends.
2. **The 21 May release commitment.** A public-target release
   needs a name and a stability contract. Without editions, the
   contract is implicit and gets eroded by every refactor lane.
3. **Pre-1.0 has been moving fast.** 10+ version bumps in the
   week of 2026-05-13 → 2026-05-15. Without editions to mark the
   boundaries, downstream consumers cannot tell which kaikai
   versions are interchangeable from their perspective.

The pre-existing `CLAUDE.md` "Not principles" line said
"Backward compatibility — not promised until post-MVP." That stance
is incompatible with shipping packages like ahu that depend on
specific kaikai behaviour. The new principle reverses it
deliberately: backward compatibility is **the** commitment to the
user, scoped via editions to keep the team's freedom intact.

## What changes in this decision

### Tier 1 principle added

`CLAUDE.md` and `docs/design.md` now carry a Tier 1 principle #4
"Stability without stagnation". The Tier 2/3 numbering shifts
accordingly. The old "Backward compatibility — not promised" line
is removed; replaced with a more precise statement about
within-edition stability and cross-edition opt-in.

### New file: `EDITION`

A single-line plain-text file at repo root containing the current
edition name in lowercase. Today: `tongariki`. The integrator
edits this file by hand when an edition transition is approved;
`cz bump` does not touch it.

### `kai --version` surfaces the edition

The wrapper now reads `EDITION` and emits e.g.:

```
kaikai 0.62.0 - tongariki (stage 2, self-hosted)
demos baseline: 27
home:           https://kaikai-lang.org
```

Fallback chain in `bin/kai`: `$ROOT/EDITION` →
`$ROOT/share/kaikai/EDITION` → literal `unknown`.

### New doc: `docs/editions.md`

The canonical policy. Defines what an edition fixes (language
surface, type semantics, effects, protocols, pipe dispatch,
stdlib `pub` surface, kai CLI, kai.toml schema), what it does
NOT fix (compiler internals, runtime layout, cache wire formats,
diagnostic exact text, performance), how editions advance, how
users select an edition in their `kai.toml`, and the migration
policy.

### `kai.toml` edition field

The doc names an `edition = "tongariki"` field that future
versions will read. Pre-Hanga-Roa compilers parse it but do not
yet apply edition-specific behaviour selectively — full
enforcement is targeted at the Hanga Roa release. The field is
forward-compatible.

## Alternatives considered

### Alternative 1 — Keep "no backward compatibility until post-MVP"

This was the existing stance. Rejected because:

- ahu (and soon henua/kohau) cannot live with that contract once
  they have users.
- The 21 May release needs a public stability message.
- Without editions, every refactor risks breaking downstream
  silently.

### Alternative 2 — Adopt SemVer strictly without editions

Use MAJOR.MINOR.PATCH with the SemVer contract: PATCH never
breaks, MINOR adds, MAJOR breaks. Pre-1.0 is exempt; post-1.0
breaking changes go to MAJOR.

Rejected because:

- Pre-1.0 the contract is empty (MAJOR.MINOR.PATCH all allow
  breakage), which is what we have today.
- Post-1.0 SemVer is too coarse: every breaking change becomes
  a major-version-bump event. Rust learned this and moved to
  editions on top of SemVer for exactly the same reason.
- Editions decouple "the team wants to evolve the language" from
  "the user has to act". With pure SemVer, every MAJOR forces
  every user to act. With editions, MAJOR is rare and
  pre-announced; routine MINOR/PATCH is silently safe.

### Alternative 3 — A single feature-flag system instead of named editions

Let users opt into individual breaking changes via flags:
`[features] new-pipe-dispatch = true`.

Rejected because:

- Combinatorial explosion. With N flags, 2^N feature sets, each
  needing testing.
- Diffuse responsibility. No single team member can describe
  "what does my package compile against?".
- Editions are exactly the consolidation: a named bundle of
  related breaking changes that travel together.

### Alternative 4 — Rust editions verbatim (numeric: "2024")

Use numeric year-based edition names like Rust does (`2024`,
`2027`).

Rejected because:

- We already have a strong naming culture: Rapa Nui geographic
  names for the language family. Numbering would clash.
- Numbers are less memorable in conversation. "Hanga Roa" reads;
  "edition 2026" doesn't.
- Rapa Nui place names also signal that kaikai is its own
  project, not a Rust clone.

## What stays the same

- Conventional Commits (`feat:`, `fix:`, etc.) continue to drive
  PATCH/MINOR bumps via `cz bump`.
- The release process described in `CLAUDE.md` "Integrator
  workflow (post-CI)" is unchanged.
- `VERSION` continues to be a plain semver string written by `cz`.
- Pre-1.0 freedom to ship `feat:` rapidly is preserved; the
  protection is only at edition boundaries.

## What needs to follow (open items)

These are not blockers for this decision; they are the
implementation work this decision unblocks.

1. **Hanga Roa edition declaration.** A separate decision-doc on
   21 May 2026 will record what the Hanga Roa edition fixes,
   what changed since Tongariki, and the migration guidance.
2. **kai migrate command.** The `editions.md` policy mentions
   `kai migrate --from <old> --to <new>`. Not implemented yet;
   may not be needed for Tongariki → Hanga Roa if changes are
   small. Required for Hanga Roa → Orongo.
3. **Edition-aware kaic2.** The compiler today does not change
   its behaviour based on the `edition` field in `kai.toml`.
   Implementing selective edition behaviour is the lane that
   makes the contract real. Targeted at the Hanga Roa release
   week.
4. **CHANGELOG annotation.** Releases that cross an edition
   boundary should be visually distinct in CHANGELOG.md. Not
   yet automated by `cz`.

## References

- Rust 2014 manifesto:
  <https://blog.rust-lang.org/2014/10/30/Stability/>
- `CLAUDE.md` Tier 1 #4 — short version of the principle.
- `docs/design.md` Tier 1 #4 — long-form discussion.
- `docs/editions.md` — full edition policy.
- `EDITION` at repo root — current edition name.
- `bin/kai` `read_edition()` + `print_version()` — runtime surface.
