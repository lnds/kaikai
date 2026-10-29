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

## Current state

The principle above is the target; Hanga Roa does not meet it yet.

- **Hanga Roa is open to breaking changes.** Until the project closes
  Hanga Roa, a change that breaks user code ships as a `!` commit with
  a `BREAKING CHANGE:` footer and a CHANGELOG entry, with no edition
  bump and no required `kai migrate` rule. Releases since Hanga Roa
  opened carry many such commits (removed method-call syntax `x.f()`,
  changed stdlib signatures and behaviour). The edition promise in the
  principle above starts to bind when Hanga Roa closes.
- **The `edition` field selects almost nothing.** The only gate keyed
  on it is pipe dispatch: under `tongariki`, `|` against a user
  `pub type` does not dispatch (only the built-in `List → list`
  mapping does). Every other language change applies to every
  edition, so a `tongariki` package that uses a removed form fails to
  build on a current compiler.
- **`kai migrate` covers a handful of rules** (listed under *Migration
  policy*), not the breaking changes accumulated so far.
- **There are no deprecation warnings.** `#[deprecated]`, like any
  attribute kaikai does not recognise, is accepted and ignored without
  a diagnostic (the recognised ones are `#[doc]`, `#[derive]`,
  `#[unstable]`, `#[constructor]` and the field-level `#[json(...)]`).

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
- **Protocol contracts.** The core protocols (`Show`, `Eq`, `Ord`,
  `Hash`, `Serialize`, `Default`, the arithmetic protocols, …) and
  their method shapes. Adding a
  new method with a default impl is non-breaking; changing an
  existing method's signature is breaking.
- **Pipe dispatch rules.** What `|`, `|>`, `||`, `|?` resolve to and
  on which types. Adding new heads to the dispatch table is
  non-breaking; removing or remapping existing heads is breaking.
- **stdlib public surface.** `pub` declarations in `stdlib/` —
  function signatures, exposed types, effect rows. Internal
  helpers (non-`pub`) are not part of the edition contract.
- **kai CLI.** `kai build`, `kai run`, `kai test`, `kai bench`,
  `kai check`, `kai typecheck`, `kai fmt` — flags and behaviour.
  New flags non-breaking; flag removal or behaviour change breaking.
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
   one line, lowercase name. Today: `hanga-roa`.
2. **`kai --version` surfaces the edition**: e.g. `kaikai <version> -
   hanga-roa (stage 2, self-hosted)`.
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
| Tongariki | pre-2026-05-15 | v0.x series through v0.68.x | The pre-Hanga-Roa phase. Rapid iteration on internals; user surface stabilising. Closed when `EDITION` flipped to `hanga-roa` on 2026-05-16. |
| **Hanga Roa** | **2026-05-16 (current)** | v0.69.0 onward | First public-target edition. Its opening releases shipped the precompiled stdlib cache; package mode with the `edition` field; the HTTP client and server helpers; the reactor (network, file, sleep, process and stdin park the fiber, never the OS thread); convention-based pipe dispatch (`|` against a downstream `Stream`, `Repository`, …); constructor overloading at use sites; effect-row generalisation of pure named functions; `#[unstable]`; the C/native parity gate; `kai lsp`; the `--diags-json` / `--effects-json` / `--library-mode` / `--effect-holes-json` report flags. Breaking changes have kept landing within it (see *Current state*). |
| Orongo | not cut | — | The stabilization edition; the 1.0 label is postponed indefinitely. Much of the scope first planned for it shipped inside Hanga Roa (cache layers, FFI struct-by-value on both backends, the BigInt/Rational/Decimal/Float32/Int32/UInt32/UInt64/Int128 primitives, direct LLVM emit with a static libLLVM, native self-host, NetDns/NetUdp, per-module compilation of user programs, `kai migrate`). What the cut itself needs is pinning the surface, a `kai migrate` rule for each breaking change, and flipping `EDITION` to `orongo`. A `kai.toml` may already name `orongo`: the compiler warns that it does not implement that edition and builds under Hanga Roa semantics. |

The current edition name is in the `EDITION` file at repo root.
This table is updated when an edition transitions; the historical
record stays.

### Breaking changes accumulated for the Orongo cut

Changes that alter the pinned surface land here as they are made, so
the Orongo release notes and the `kai migrate` rule set can be
assembled from one list rather than reconstructed from git.

- **`Fail` removed from the stdlib** (issue #1535). `pub effect Fail`
  and its runtime default handler are gone from `stdlib/effects.kai`.
  Migration: a `/ Fail` row becomes `Result[T, String]` propagated
  with postfix `!`; a failure whose policy the consumer must choose
  becomes a domain effect whose op returns `Unit` (so a handler can
  resume and skip); a deep non-local exit uses `Cancel.raise()`. Code
  that wants the old shape verbatim declares the effect itself —
  `effect Fail { fail(msg: String) : Nothing }` — with the one
  behavioural difference that a locally declared effect has no default
  handler, so an unhandled `fail` is now the compile error `effect not
  handled: Fail` instead of a runtime abort with an exit-1 banner.
  Not mechanical: `kai migrate` reports it as `manual:`, because the
  right replacement depends on whether the caller can recover.

- **`initially` / `finally` clauses on a handler** (issue #1537).
  Additive to the surface — existing handlers keep parsing and behaving
  unchanged — but recorded here because it changes the *semantics* of
  paths that previously ran no user code: a non-local exit now runs the
  `finally` of every scope it unwinds, where before it truncated the
  evidence chain silently. Programs that leaked a resource on an abort
  or cancellation path now release it. `initially` and the existing
  `with Eff(init)` form fill the same handler-state slot and are
  mutually exclusive; writing both is a parse error. No migration:
  nothing that compiled before stops compiling, and the names are
  contextual, so a user effect may still declare an op called
  `finally`.

- **`kai install` with no argument** (issue #1724). The command now
  installs a package's binary, the `cargo install` / `go install`
  meaning; dependency resolution, which is what it used to do, is
  `kai fetch`. The argument form is additive — it was an error before
  — and the no-argument form still resolves dependencies within
  Hanga Roa, printing a deprecation notice. At the Orongo cut it
  becomes "install the cwd package", matching both precedents.
  Migration is mechanical but not source-level: a script or CI job
  running bare `kai install` to prefetch dependencies changes to
  `kai fetch`. `kai migrate` does not cover it — the affected text is
  in shell scripts and workflow files, not in `.kai` source.

- **Unused imports** (decided with #1845). An
  `import` the file never refers to — neither its written name as a
  qualifier nor any name the module exports (protocol operations
  included) — is a **warning** in Hanga Roa (`warning: unused import
  \`m\``, at the import) and becomes an **error** at the Orongo cut,
  as in Go. A module that declares impls, or declares nothing (a
  documentation stub such as `math.bits`), is used by being loaded and
  is never reported. Migration is mechanical — delete the line — and
  `kai migrate` will carry the rule once the error is switched on. An
  import its file never uses but that is the only way a module another
  file uses enters the unit is not "unused": its warning names that
  file, and the import moves there instead of going away.
  Open before the flip: a program that imports a module only for the
  impls it brings into scope is exempt by that rule, but an import of
  a module that *also* exports names and is still used only for its
  impls would be silent under the exemption and an error without it;
   the Orongo rule needs either a blank-import form or an "impl
   actually used" exemption for that class (`examples/org-pkgs` is
   full of them).

- **Binder case is one table** (`stage2/compiler/parse_naming.kai`), applied by one post-parse pass over every binder position of the AST (`ast_binders.kai`).
  A `var` or `let` binder and every pattern binder is `snake_case`, a
  sum-type variant is `PascalCase`, a type parameter is lowercase
  (`fn id[a](x: a)`), and a `const` is `UPPER_SNAKE_CASE`; each
  violation is a parse error naming the convention. Migration: `kai
  migrate` renames a lowercase `const` to `UPPER_SNAKE_CASE` together
  with every use the resolver binds to it, and never a homonym; a
  renamed `pub const` is a breaking change for the importing package
  (`real_pi` → `REAL_PI`, `owner_only` → `OWNER_ONLY`, …). The same
  rule respells uppercase type parameters (`[T]` → `[t]`) with every use
  in their declaration; a legacy sibling module parses as written during
  the run.

- **Core helpers superseded by surface features** (audit in the
  closed issue #1807; scheduled, not yet removed). Seven core helpers
  remain callable although their replacement shipped:
  `string_concat(a, b)` → `a ++ b`; `string_concat_all(xs)` →
  `string_join(xs, "")`; `int_to_string` / `real_to_string` → string
  interpolation (`"#{n}"`) or `Show`; `string_to_int` /
  `string_to_real` → the parse path (replacement unverified in the
  audit); `array_make(n, v)` → constructor under review (indexed
  access and assignment already cover reads/writes). They keep
  working through Hanga Roa — some are still called inside
  `impl Show` bodies — and at the Orongo cut the ones with verified
  replacements are removed, with `kai migrate` rewriting user call
  sites (all mechanical except `array_make`). Open before the flip:
  verify the parse-path replacement for the two `string_to_*`
  helpers, and decide `array_make`'s fate (retain as the sole
  constructor or replace with a literal form).

## Edition selection in user packages

A `kai.toml` declares which edition its source compiles against:

```toml
name = "my-app"
version = "0.1.0"
edition = "hanga-roa"
entry = "src/main.kai"   # optional — defaults to main.kai

[dependencies]
```

If omitted, the package uses the default edition of the kaikai
installation. Recommendation: set it explicitly once the package
ships a stable release, so that future kaikai upgrades do not
silently retarget the package against a newer edition.

The `entry` field (issue #658) overrides the conventional
`main.kai` entry point used by `kai build` / `kai run` / `kai test`
when run from inside the package directory. Mirrors Cargo's
`[[bin]] path =`. If the manifest declares an `entry` that does
not exist on disk, the driver fails with
`kai.toml declares entry = '<path>' but <path> does not exist`
before invoking the compiler. See `docs/packages.md` for the full
package-aware build shape.

**Edition selection:** `bin/kai` reads `kai.toml`'s `edition`
field, validates it against the known set `{tongariki, hanga-roa,
orongo}`, and forwards `--edition <name>` to `kaic2`; an unknown
value fails with `kai: error: unknown edition "<x>" in <dir>/kai.toml`
before any compilation runs. The one rule keyed on the edition is
convention-based pipe dispatch, on for `hanga-roa` and later; under
`tongariki` only the built-in `List → list` mapping dispatches.
Every other rule is the same for all editions, so declaring
`tongariki` does not preserve forms removed since. `orongo` is
accepted with a warning and compiles under Hanga Roa semantics.

## Marking unstable APIs (#unstable)

The edition contract pins every `pub` declaration in stdlib and in
any downstream package that ships against an edition. That's the
guarantee. But sometimes a package author wants to ship a new
public API alongside a stable one and reserve the right to iterate
on its signatures without an edition bump. The `#[unstable]`
annotation is the escape hatch (issue #602).

### Author side — marking a declaration

```kaikai
#[unstable]
pub fn parse_level(s: String) : Int = string_length(s)

#[unstable]
pub type Window = { width: Int, height: Int }

#[unstable]
pub const DEFAULT_BUFFER_BYTES : Int = 4096

fn main() = {
  let w = Window { width: 3, height: 4 }
  println(int_to_string(parse_level("ab") + w.width + DEFAULT_BUFFER_BYTES))
}
```

`#[unstable]` precedes the `pub` keyword and is permitted on `pub fn`,
`pub type`, `pub const`, `pub effect`, and `pub protocol`. Calls from
the declaring package itself never warn. Marking a
non-`pub` declaration is rejected at parse time — there's no exposed
surface to mark unstable.

### Consumer side — opting in

The consumer's `kai.toml` lists the upstream modules whose unstable
exports they have read and accepted, by the qualifier the call is
written with:

```toml
name = "my-app"
version = "0.1.0"
edition = "hanga-roa"

[unstable]
mylib = true
```

Importing a `#[unstable]` declaration **without** opt-in produces a
non-fatal warning at every call site:

```text
warning: mylib.experimental is #[unstable] — API may change between versions without an edition bump. Add 'mylib = true' under [unstable] in kai.toml to acknowledge.
  --> main.kai:3:61
```

The build succeeds. The diagnostic is the disclosure: the consumer
chose to touch unstable surface and the toolchain made it visible.
Adding the opt-in entry suppresses the warning. The declaration is
consumed exactly as it would be otherwise — no codegen change.

### Why this is not "an edition for one decl"

`#[unstable]` does NOT split the edition. The package still declares
`edition = "hanga-roa"`; only specific decls are excluded from the
contract. Downstream packages can ship with their as-yet-uncommitted
public surface marked, under the same edition as everything else.

When the author commits to an unstable decl, they remove the
`#[unstable]` annotation in a `feat:` release. Downstream consumers
notice a missing warning, drop the `[unstable]` entry on their next
clean-up, and the API is now under the edition contract.

## Migration policy

When edition `N → N+1` introduces breaking changes, the kaikai
toolchain ships migration help:

- **Release notes** list every breaking change with before/after
  examples.
- **`kai migrate --from <old> --to <new>`** rewrites source from
  one edition to the next where the change is mechanical (renames,
  signature shifts, syntax desugars). It is an AST rewrite through
  the parser + the formatter — never a textual pass — so it does
  not corrupt code and never emits source that fails to parse.
  Dry-run by default (prints the migrated source, writes nothing);
  `--write` applies it in place, and a second `--write` run is a
  byte-stable no-op. Non-mechanical changes are reported as
  `manual: <line>:<col> — …` pointers, not silently dropped.
- **Deprecation warnings** pointing at a future change are the
  intended third tool; kaikai has no mechanism for them today
  (`#[deprecated]` is ignored).

The principle: a user planning to upgrade can read the release
notes, run `kai migrate`, and have a working build. Worst case,
the migration leaves diagnostics that explain what to fix.

`kai migrate` supports `tongariki → orongo` and `hanga-roa → orongo`, and
`--write` also stamps the target edition into `kai.toml`. Its rule set today:

- `map.from_pairs`, `hashmap.from_pairs`, `set.from_list`,
  `hashset.from_list` → `.from`;
- `stream.fold`, `map.fold` → `foldl`; `string_builder.len` → `length`;
- the two type arguments of every `Result[_, _]` annotation are
  swapped, only when the source edition is `tongariki`, whose `Result`
  is Err-first. Hanga Roa's `Result` is already Ok-first
  (`Result[T, E]`), so a `hanga-roa` source keeps its annotations;
- `array.from` / `array.copy` with a `default` argument → reported as
  `manual:`.

It does not cover the breaking changes that have landed within Hanga
Roa (see *Current state*).

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
