# kaikai editions

Editions are kaikai's mechanism for **stability without stagnation** —
the Tier 1 principle adopted from Rust's
[2014 stability manifesto](https://blog.rust-lang.org/2014/10/30/Stability/).
This document defines what an edition is, what stability it guarantees,
how editions advance, and the current edition policy.

## Principle

Within one edition, kaikai's language surface and user-facing stdlib
API are **stable**. A package written against an edition continues to
compile on every subsequent kaikai version *that still supports that
edition*. Breaking changes happen only when crossing an edition
boundary — and the user opts in to the new edition explicitly.

The contract to the user: **upgrading kaikai is painless**. Read
release notes, run `kai upgrade`, recompile.

The contract to the kaikai team: we can iterate fast on internals —
runtime, codegen, RC discipline, cache layers, typer refactors —
without breaking that contract. The protection lives at the **edition
boundary**, not at every minor version.

## What an edition fixes (the stable surface)

An edition pins the user-visible interface:

- **Language syntax.** Reserved keywords, operator precedence, the
  grammar consumed by the parser. New syntax additions that are
  unambiguous extensions (a new keyword nobody uses, a new operator
  in unused slot) may land within an edition; anything that changes
  the meaning of existing source requires an edition bump.
- **Type system semantics.** Inference rules, subtyping behaviour
  (e.g. refinement transparency), unification semantics, monomorph
  rules. Bug fixes that align behaviour with documented rules are
  not breaking; behaviour changes that reinterpret valid programs
  are.
- **Effect system surface.** Effect rows, effect names, default
  handlers, the catalog in `docs/effects-stdlib.md`. Adding a new
  effect or a new operation on an existing effect is non-breaking.
  Renaming or restructuring an existing effect requires an edition
  bump.
- **Protocol contracts.** The five core protocols (`Show`, `Eq`,
  `Ord`, `Hash`, `Serialize`) and their method shapes. Adding a
  new method with a default impl is non-breaking; changing an
  existing method's signature is breaking.
- **Pipe dispatch rules.** What `|`, `|>`, `||`, `|?` resolve to and
  on which types. Adding new heads to the dispatch table is
  non-breaking; removing or remapping existing heads is breaking.
- **stdlib public surface.** `pub` declarations in `stdlib/` —
  function signatures, exposed types, effect rows. Internal
  helpers (non-`pub`) are not part of the edition contract.
- **kai CLI.** `kai build`, `kai run`, `kai test`, `kai bench`,
  `kai check`, `kai fmt` — flags and behaviour. New flags
  non-breaking; flag removal or behaviour change breaking.
- **`kai.toml` schema.** Field names, validation rules. Additions
  non-breaking; removals or semantic changes breaking.

## What an edition does NOT fix (free to change at any release)

The edition contract is about **the user's source code surface**.
These are internals and may change in any release without an
edition bump:

- Compiler internals (typer passes, perceus, monomorphisation,
  emit phase, lower passes).
- Runtime representation (variant cell layout, RC discipline,
  fiber stack format, mailbox internals). Programs see only the
  observable behaviour, not the bytes.
- Wire formats for caches (`.kab*`), object files, debug info.
- Stage 0/1/2 internal interfaces (kaic0 → kaic1 → kaic2 plumbing).
- Build pipeline details (how `bin/kai` shells out to `cc` or
  `clang`, atomic write strategies, sha256 batching).
- Diagnostic message text and format. The *contract* is that a
  diagnostic appears for a given violation; the *exact words* may
  change.
- Performance characteristics. We commit to upgrade-painless, not
  to wall-time identical across releases.

If a change touches only items in this list, no edition bump is
needed and the version increments per Conventional Commits
(`fix:` → PATCH, `feat:` → MINOR pre-1.0).

## How editions advance

1. **Current edition lives in `EDITION` at repo root.** Plain text,
   one line, lowercase name. Today: `tongariki`.
2. **`kai --version` surfaces the edition**: e.g. `kaikai 0.62.0 -
   tongariki (stage 2, self-hosted)`.
3. **Edition changes are deliberate**, decided by the integrator
   (the human), not automated by `cz bump`. The decision is
   recorded in `docs/decisions/edition-<name>-<yyyy-mm-dd>.md` per
   the project's decision-doc convention.
4. **When an edition bumps**, a release follows the same week, and
   release notes list every breaking change relative to the prior
   edition, with migration guidance for each.
5. **Edition names are Rapa Nui geographic names**, matching the
   kaikai naming family (kaikai itself is Rapa Nui for the
   string-figure tradition; ahu, kohau, henua, manutara, hopu
   follow). See `kaikai-docs/framework-naming.md` for the broader
   naming policy.

## Current edition timeline

| Edition  | Active from | Released as     | Notes                                                                        |
|----------|-------------|-----------------|------------------------------------------------------------------------------|
| Tongariki | pre-2026-05-15 | v0.x series through 2026-05-20 | The pre-Hanga-Roa phase. Rapid iteration on internals; user surface stabilising. |
| **Hanga Roa** | **2026-05-21** | v0.x release | First public-target edition. Includes the cache chain, package-mode workflow, HTTP server, stable pipe dispatch including the convention-based extension for downstream types (`|` against `Stream`, `Repository`, etc.). |
| Orongo | post-Hanga-Roa | TBD | Next edition. Pulls in items that did not make Hanga Roa: advanced FFI (struct-by-value), new numeric primitives (BigInt, Rational, Float32, etc.), LLVM direct emit, Phase A.2 cache, compilation daemon. |

The current edition name is in the `EDITION` file at repo root.
This table is updated when an edition transitions; the historical
record stays.

## Edition selection in user packages

A `kai.toml` declares which edition its source compiles against:

```toml
name = "my-app"
version = "0.1.0"
edition = "hanga-roa"

[dependencies]
```

If omitted, the package uses the default edition of the kaikai
installation. Recommendation: set it explicitly once the package
ships a stable release, so that future kaikai upgrades do not
silently retarget the package against a newer edition.

**Multi-edition support:** kaikai's compiler accepts source from
any edition it knows about. When a package declares `edition =
"tongariki"`, the compiler applies the tongariki rules even if the
default edition is hanga-roa. This is how `stability without
stagnation` works in practice: a tongariki package keeps working
on an hanga-roa compiler.

> **v1 status (2026-05-15):** edition selection is **shipped**
> (issue #603). `bin/kai` reads `kai.toml`'s `edition` field,
> validates it against the known set `{tongariki, hanga-roa}`, and
> forwards `--edition <name>` to `kaic2`. The compiler routes the
> #594 convention-based pipe dispatch on `edition >= hanga-roa` and
> falls back to the legacy seeded `List → list` mapping under
> tongariki. The prelude cache is partitioned by edition under
> `~/.cache/kaikai/preludes-v1/<edition>/`, so cross-edition rebuilds
> are clean. An unknown edition value produces
> `kaic2: unknown edition \`<x>\` in kai.toml — known editions:
> tongariki, hanga-roa` before any compilation work runs. The
> `cache_kaikai_version_hash` was bumped from 1 to 2; any `.kab`
> blob predating #603 is rejected at load.

## Marking unstable APIs (#unstable)

The edition contract pins every `pub` declaration in stdlib and in
any downstream package that ships against an edition. That's the
guarantee. But sometimes a package author wants to ship a new
public API alongside a stable one and reserve the right to iterate
on its signatures without an edition bump. The `#[unstable]`
annotation is the escape hatch (issue #602).

### Author side — marking a declaration

```kai
#[unstable]
pub fn from_stdin() : Source[String, Stdin + Spawn] / Spawn = ?from_stdin

#[unstable]
pub type Source[t, e] = { pid: Pid[Demand] }

#[unstable]
pub const DEFAULT_BUFFER_BYTES : Int = 4096
```

`#[unstable]` precedes the `pub` keyword and is permitted on `pub fn`,
`pub type`, `pub const`, `pub effect`, and `pub protocol`. Marking a
non-`pub` declaration is rejected at parse time — there's no exposed
surface to mark unstable.

### Consumer side — opting in

The consumer's `kai.toml` lists upstream packages whose unstable
exports they have read and accepted:

```toml
name = "my-app"
version = "0.1.0"
edition = "hanga-roa"

[unstable]
ahu = true
henua = true
```

Importing a `#[unstable]` declaration **without** opt-in produces a
non-fatal warning at every call site:

```
warning: ahu.stream.from_stdin is #unstable — API may change between
  versions without an edition bump. Add 'ahu = true' under [unstable]
  in kai.toml to acknowledge.
  --> src/main.kai:3:14
```

The build succeeds. The diagnostic is the disclosure: the consumer
chose to touch unstable surface and the toolchain made it visible.
Adding the opt-in entry suppresses the warning. The declaration is
consumed exactly as it would be otherwise — no codegen change.

### Why this is not "an edition for one decl"

`#[unstable]` does NOT split the edition. The package still declares
`edition = "hanga-roa"`; only specific decls are excluded from the
contract. This keeps Hanga Roa shippable on 2026-05-21 with
downstream packages (ahu, kohau, henua, the HTTP server) carrying
their as-yet-uncommitted public surface marked, while the language
surface itself stays stable.

When the author commits to an unstable decl, they remove the
`#[unstable]` annotation in a `feat:` release. Downstream consumers
notice a missing warning, drop the `[unstable]` entry on their next
clean-up, and the API is now under the edition contract.

## Migration policy

When edition `N → N+1` introduces breaking changes, the kaikai
toolchain ships migration help:

- **Release notes** list every breaking change with before/after
  examples.
- **`kai migrate --from <old> --to <new>`** is the goal: a
  command that rewrites source from one edition to the next where
  the change is mechanical (renames, signature shifts, syntax
  desugars). Non-mechanical changes get diagnostics asking the
  user to fix them manually.
- **Deprecation warnings** appear in the older edition where
  possible, pointing at the future change. A user staying on the
  older edition keeps compiling; they get a heads-up about what
  changes if they bump.

The principle: a user planning to upgrade can read the release
notes, run `kai migrate`, and have a working build. Worst case,
the migration leaves diagnostics that explain what to fix. We
never silently change behaviour.

## What is NOT an edition

- **A bug fix.** Aligning runtime behaviour with documented
  semantics is not a breaking change, regardless of how much code
  it might break. The contract was the documented semantics, not
  the bug.
- **A performance change.** Code that compiles slower or faster
  is not breaking. Same for code that runs faster (or, rarely,
  slower in pathological cases).
- **An internal refactor.** Reshuffling `stage2/compiler.kai` into
  smaller modules, changing how the cache is laid out on disk,
  rewriting how perceus handles drops — none of these are
  edition-relevant.
- **A new feature.** Adding `kai test --watch`, a new effect
  primitive, a new stdlib module is non-breaking and lands within
  the current edition.

## Recording the policy

This file is the canonical edition policy. Cross-references:

- `CLAUDE.md` Tier 1 #4 — short version of the principle.
- `docs/design.md` Tier 1 #4 — long-form discussion.
- `EDITION` (repo root) — the current edition's lowercase name.
- `bin/kai --version` — surfaces the edition at runtime.
- `kaikai-docs/framework-naming.md` — Rapa Nui naming family for
  editions, packages, and the language itself.

History of edition decisions lives in
`docs/decisions/edition-<name>-<yyyy-mm-dd>.md`.
