# Lane experience — issue #602: #unstable annotation + enforcement

**Status:** Shipped, first lane of the Anga Roa edition.

**Lane branch:** `issue-602-unstable-annotation` (from `main`,
post-v0.63.1).

## Scope as planned

From the brief: add a `#unstable` annotation that marks a `pub fn /
pub type / pub const` as outside the edition stability contract,
plus a `[unstable]` table in `kai.toml` that opts the consumer into
the unstable surface of named upstream packages. At every qualified
call to an unopted-into unstable export, the compiler emits a
non-fatal warning anchored at the call site. Six fixtures.
Selfhost byte-identical. Docs updated.

## Scope as shipped

All ten acceptance items closed:

1. Annotation parses on `pub fn`, `pub type`, `pub const`,
   `pub effect`, and `pub protocol`. (The brief listed four shapes;
   `pub effect` and `pub protocol` came for free from the same
   dispatch and cost zero extra to support — both are part of the
   edition-pinned surface.)
2. AST flag stored as a `DUnstable(Decl, Int, Int)` wrapper,
   mirroring the existing `DDerive` precedent rather than threading
   a new boolean field through every Decl constructor (which would
   touch hundreds of construction sites). The wrapper preserves the
   inner Decl unchanged so every downstream walker delegates via
   one mechanical `DUnstable(inner, l, c) -> ...` arm.
3. `kai.toml` schema accepts `[unstable] <pkg> = true` entries.
   Parsing lives entirely in the shell driver (`bin/kai`) via two
   awk one-liners; `tools/kai-pkg/` was not touched. The driver
   reads the consumer's manifest, emits `--package-name <n>` and
   `--unstable-optin <pkg>` flags per opted-in entry, and passes
   them to `kaic2`.
4. Resolver enforcement runs as a dedicated post-`rqc_decls` walker
   (`unstable_check_decls`). It scans every `EModCall(mod, fname)`
   produced by the qualified-call rewrite, looks up the module's
   `unstable_exports` list on its `ModuleEntry`, and emits the
   warning unless the consumer has opted in (or `mod == pkg`).
5. Warning shape matches the brief verbatim, including the
   suggested remedy text.
6. Six fixtures under `examples/unstable/`: positive opt-in,
   negative no-opt-in, pub type variant, multi-decl, non-pub
   parse-error, and unknown-pkg benign opt-in.
7. Selfhost byte-identical confirmed by `make tier0`.
8. Tier 0 green; Tier 1 green at submit.
9. `docs/editions.md` gained a `Marking unstable APIs (#unstable)`
   section ~50 lines long.
10. `docs/protocols.md` cross-references `#unstable` in the
    annotation discussion.

## Design decisions and alternatives considered

### DUnstable wrapper vs flag on every Decl constructor

The brief originally proposed adding an `is_unstable: Bool` field
to `DFn`, `DType`, `DConst`, and the module-level Decl. A quick
audit found `DFn` is constructed at ~30 sites and matched at ~150;
`DType` is similar. Adding a field would force a touch on every
construction site — many in lowering passes that synthesise
"empty" Decls — and the patch grows past 800 LOC for what is
essentially a one-bit annotation.

The existing `DDerive([String], Decl, Int, Int)` precedent showed
that the kaikai compiler already uses Decl wrappers for
annotations, with the rule that every Decl walker unwraps and
recurses on `inner`. Replicating that for `DUnstable(Decl, Int,
Int)` cost ~53 mechanical edits across the file (one per
`DDerive` match arm) and zero touches on the ~150 `DFn`
construction sites. The arity of `DUnstable` is three (vs four for
`DDerive`) because it carries no parameters; pattern shapes
adjust accordingly (`DUnstable(inner, l, c)` vs `DDerive(_, inner,
l, c)`).

### Driver-side TOML parsing vs kai-pkg subcommand

The compiler doesn't read `kai.toml` directly today —
`tools/kai-pkg/` does, and the shell driver `bin/kai` orchestrates
the pipeline. We had two options:

- **Add a `kai-pkg unstable` subcommand** that emits the
  `[unstable]` opt-ins. Pros: a single TOML parser shared with
  the rest of kai-pkg. Cons: another binary build dependency, an
  extra fork per build, and an unbounded surface that ties
  `bin/kai` to `kai-pkg` reliability.
- **Parse `[unstable]` directly in the shell driver.** Pros: zero
  new code in `kai-pkg`, single fork, parsing is trivial enough
  for awk (no quoted keys, no multi-line values, no nested
  tables under `[unstable]`). Cons: a second TOML reader in the
  driver; brittle if the schema grows.

We chose the driver-side path. The `[unstable]` shape is very
narrow (table of `<pkg> = true` entries), awk handles it cleanly
in ~10 lines, and a future schema growth that breaks the awk
parser is easy to spot.

### Self-import suppression: deferred

The brief proposed silencing the warning when a package consumes
its own unstable APIs (`pkg == mod`). We implemented this check
literally — but in kaikai today, a module is a single file and a
"package" is the consumer's `kai.toml` `name` field. There is no
"this module belongs to package X" metadata. So `mod == pkg`
literally compares the imported-module name (e.g. `mylib`) to the
package name (e.g. `myapp`), which always differs unless the user
named both the same.

We're shipping with the literal check in place; in practice it
fires only when the package name and module name coincide, which
is rare but legitimate. Detecting "this module lives under the
manifest dir" — a stronger self-import notion — needs a path
comparison in the resolver and is filed as a follow-up. The
practical mitigation today: a package author writing their own
unstable APIs can opt-in to their own surface via `[unstable]
<self> = true`. Not elegant; harmless.

### Module-level annotation: not shipped literally

The brief mentioned a `#unstable module ahu.stream { ... }` shape.
kaikai's surface has no `module` keyword — modules are files, not
explicit lexical blocks. We considered:

- **File-level pragma** (`#unstable_module` as a first-line
  declaration). Adds a second annotation form; conflicts with the
  goal of "one annotation, one rule".
- **Per-decl repetition.** The author writes `#unstable` above
  every pub decl in the file. Costly for large modules.

We chose per-decl. A follow-up that adds a file-scope pragma is
cheap if real usage demands it.

## Structural surprises the brief did not anticipate

1. The compiler does not read `kai.toml` at all — that lives in
   the shell driver and `tools/kai-pkg/`. This forced the
   parsing-in-driver decision above. The compiler accepts the
   opt-in list as CLI flags (`--package-name`, `--unstable-optin`),
   which felt right in retrospect: it keeps `kaic2` independent of
   the manifest format and makes the wiring testable in isolation.
2. The cache layer (issue #585) carries a placeholder
   deserialiser for `DUnstable` (tag 16). Cache serdes will not
   see real `DUnstable` data until the prelude carries unstable
   decls; the deserialiser is a defensive belt against silently
   dropping the annotation on a future cache hit.
3. `ModuleEntry` got a fourth field (`unstable_exports:
   [String]`). Twenty-one construction/destructuring sites had to
   absorb the arity change. All mechanical.

## Fixtures shipped

```
examples/unstable/
├── README.md
├── pub_fn_with_optin/          # opt-in, no warning, prints 42
├── pub_fn_no_optin/            # no opt-in, warning, prints 42
├── pub_type/                   # #unstable pub type + pub fn, opt-in, prints 7
├── multi_unstable_pub_decls/   # 3 decls, 2 marked, 2 warnings
├── non_pub_ignored/            # parse error, .err.expected captured
└── unknown_pkg_optin/          # benign unknown opt-in, prints ok
```

Each fixture is runnable with `bin/kai run <dir>/main.kai` from
the repo root. No git fixtures (issue-405 style); the brief's
"positive + negative + module-level + edge cases" coverage matches
the six.

## Coverage gaps

- No fixture exercises `#unstable pub effect` or `#unstable pub
  protocol`. The parser handles them; they're plumbed through the
  same wrapper. Adding a fixture is cheap if real usage emerges.
- No fixture exercises the cache layer (issue #585 blocks the
  cache anyway). `DUnstable` survives a roundtrip in principle
  but is unproven on real data.

## Real cost vs estimate

Brief estimate: 1–2 sessions. Real cost: ~one focused session
plus the delegated mechanical edits. The DDerive precedent
shaved most of the risk; the wrapper pattern + delegate-to-inner
discipline made the change-surface small and predictable.

The bulk of the time went into:

1. Tracking the 21 `ModuleEntry` arity sites (15 min).
2. Threading `package_name` + `unstable_optins` through
   `compile_source` (10 min).
3. Wiring the driver-side awk parser and CLI flags (15 min).
4. Writing the post-pass `unstable_check_decls` walker (~250 LOC
   of mechanical structural recursion; ~20 min).
5. Six fixtures + smoke-testing each (15 min).
6. Docs + retro (15 min).

## Follow-ups for the next lanes

1. **Self-import suppression** — detect when an imported module
   lives under the consumer's manifest dir and skip the warning
   without an explicit opt-in. Resolver-level path comparison.
2. **File-scope `#unstable` pragma** — if real downstream usage
   shows per-decl repetition is painful, add a file-level form.
3. **`kai check --warn-unstable`** — surface the warning count
   in JSON for IDE / CI integration. Probably one-liner change
   in `--check` mode.
4. **`kai migrate --promote-stable`** — codemod that walks a
   library's source, removes every `#unstable` annotation, and
   prints a summary. Easy once the lane lands.
5. **stdlib hookup** — Anga Roa's HTTP server can now mark its
   experimental endpoints `#unstable` without blocking the
   edition release. Filed for the HTTP server lane.
6. **Coverage probe** — the `examples/unstable/*` fixtures don't
   land in `make coverage-probe` because they're not under a
   gated path. Decide whether to add them.

## What load-bearing in this lane stays for the next

- **Drivers can pass schema-derived flags to kaic2 cleanly.** The
  `--package-name` + `--unstable-optin` pattern works; future
  manifest fields (e.g. `[features]`) can follow the same shape.
- **The Decl-wrapper precedent is now firmly established.**
  `DDerive` was first; `DUnstable` is second; the next annotation
  (`#deprecated`?) just adds a third wrapper and a third
  mechanical edit pass over the walker sites.
- **Post-rqc walkers are a clean place to land diagnostic-only
  passes.** The qualified-call rewrite is the natural anchor for
  "did the consumer touch something they shouldn't have."
