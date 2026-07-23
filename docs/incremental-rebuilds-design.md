# Incremental rebuilds — per-user-module build avoidance (design)

Design for #1428. Today kaikai's rebuild-after-edit is **flat**: a 41-module
package recompiles all 41 modules regardless of what changed (measured 1.66 s
cold and 1.66 s after a one-line edit, vs GHC's 1.71 s → 0.28 s). Per-module
throughput already beats GHC; the missing piece is exclusively **build
avoidance**. This doc specifies the cache key, delta serialization, propagation
rule, and invalidation for that avoidance. It is **design only** — no compiler
change lands under it.

The lever is to extend the content-addressed pattern that already caches the
**core** (#1322/#1334) and **user post-parse** (#455) to a per-module *typed*
artifact, and to cut downstream recompiles when a module's typed interface is
byte-identical — GHC's `.hi`-hash recompilation avoidance.

## What already exists (the reused foundation)

The infrastructure #988 and #1298 left behind is exactly what this reuses; both
were parked because the return on the **cold-start floor** was small (~67 ms /
~4 % for the typer alone — #1298), which was correct then. The return on the
**incremental** loop is the 6–20× above, so the same foundation is now worth
building on:

- **Content-addressed cache keying** (`stage2/compiler/user_cache.kai`,
  `core_cache.kai`). The user cache already keys each root file by
  `content_hex = uc_content_hex(src)` (FNV-1a 64-bit over the source bytes, 16
  hex chars) **plus** `dep_hex`, a `string_hash` over the concatenated
  `content_hex` of every transitive import in discovery order. The blob filename
  embeds both, so any edit to the source, a direct import, or a transitive
  import changes the filename and the old entry is simply never read —
  invalidation is a property of the key, not a sweep. This is the exact keying
  shape the per-module typed cache needs; only the **payload** differs.
- **KAB2 codec** (`stage2/compiler/cache.kai`). The binary blob format:
  4-byte magic `"KAB2"`, `format_version` (u32 LE, `cache_format_version() = 4`,
  bumped on any payload schema change), `kaikai_version` hash (u32 LE), 64-byte
  ASCII source SHA, then the payload. `cache_read_header` rejects a blob whose
  magic / format / version drifts — so a stale or foreign blob is a miss, never
  a mis-load.
- **Key-version stamps.** `uc_key_version()` / `cc_key_version()` are bumped on
  any change to the respective key derivation, distinct from the codec
  `format_version`. A typed cache adds a third such stamp.

**The one gap the foundation does *not* close (blocking — see open questions):**
the KAB2 codec today serializes **surface `[Decl]` only**. There is no schema for
typed artifacts (`Scheme`s, effect-row solutions, `ModuleEnvDelta`). A new typed
codec is a prerequisite, not an arm on the existing one.

## Cache key

Per user module `M`, the typed-interface blob is keyed on the tuple:

```
typed_key(M) = ( content_hex(source(M)),
                 iface_dep_hex(M),
                 uc_typed_key_version,
                 kaikai_version, edition, flags_hash )
```

- `content_hex(source(M))` — FNV-1a over `M`'s own source bytes, via the existing
  `uc_content_hex`.
- `iface_dep_hex(M)` — `string_hash` over the concatenation, in stable
  discovery order, of each **direct** import's **interface hash** (see below).
  This replaces the post-parse cache's `dep_hex` (which hashes imports'
  *content*): the typed cache must key on imports' *interfaces*, so that a
  content edit to an import that leaves its interface unchanged does **not**
  invalidate `M` — that is the whole point (propagation rule below).
- `uc_typed_key_version` — new stamp, bumped on any change to typed-key
  derivation or typed-blob layout (a third stamp alongside `uc_key_version` /
  `cc_key_version`).
- `kaikai_version, edition, flags_hash` — folded into the key so an upgrade,
  edition bump, or flag change is a global miss (see Invalidation).

**Interface hash of `M`** = `content_hex` over the serialized `ModuleEnvDelta`
of `M` *restricted to its interface-affecting tables* (below), NOT over `M`'s
source. Two sources that produce byte-identical deltas share an interface hash;
that identity is what the propagation cut reads.

## Delta serialization — the `ModuleEnvDelta`

The typed interface a module contributes is `ModuleEnvDelta`
(`stage2/compiler/infer.kai`), the bounded struct the issue names. It has a
`name` field plus **8 tables**:

| # | Table | Interface-affecting? | Note |
|---|---|:---:|---|
| 1 | `ty_entries: [TyEntry]` | **yes** | The inferred `Scheme` of each exported binding. A changed signature ⇒ downstream must re-infer. |
| 2 | `unions: [UnionInfo]` | **yes** | Exported union declarations; a variant added/removed changes what importers resolve. |
| 3 | `op_eff_arities: [OpEffArity]` | **yes** | Effect-op arities visible across the boundary. |
| 4 | `recs: [RecInfo]` | **yes** | Record shapes referenced by importers. |
| 5 | `sums: [SumInfo]` | **yes** | Sum-type shapes referenced by importers. |
| 6 | `op_to_eff: [OpToEff]` | **yes** | Op→effect mapping consulted during importer inference. |
| 7 | `unit_aliases: [UAliasEntry]` | **yes** | `unit` aliases an importer may reference by name. |
| 8 | `constructors: [CtorEntry]` | **yes** | Exported constructors importers pattern-match / build against. |

Under the current scope-A shape **every one of the 8 tables is interface-visible**
— an importing module can, in principle, resolve against any of them
(`build_ty_env_inherited` seeds the importer's typer env from exactly these).
There is no purely-internal table in `ModuleEnvDelta` today: internal-only typer
state (local substitutions, holes, per-module diagnostics) lives *outside* the
delta, in `TypecheckedModule.typed` and the fold accumulators, and is never part
of the interface hash. So the serialization target is the whole delta, and the
interface hash covers all 8 tables.

The blob for `M` therefore carries:

1. `M`'s `ModuleEnvDelta` — the interface, serialized via the new typed KAB2
   payload schema.
2. `M`'s `TypedProgram` contribution (`TypecheckedModule.typed`) — the typed
   decls needed downstream for monomorph. This is internal (does not enter the
   interface hash) but must be persisted so a cache hit reconstructs exactly
   what a fresh infer produced.

The interface hash is computed over (1) only, in a **canonical serialization**
(stable table order, stable within-table order) so that logically-equal
interfaces hash equal regardless of inference order — the fold today does not
guarantee a stable order, so canonicalization is part of the serializer's
contract (open question).

## Propagation rule

Let `dirty(M)` mean `M`'s own source `content_hex` changed, or `M` has no valid
blob. The rebuild proceeds bottom-up over the import DAG:

1. **Recompile front-end only for dirty modules.** For each `M` with
   `dirty(M)` false and every direct import interface-hash-matching its recorded
   `iface_dep_hex(M)`, load `M`'s typed blob and **skip parse/resolve/infer**.
2. **Interface-cut.** When a dirty module `M` is re-inferred, compare its new
   interface hash against the one recorded in its old blob:
   - **unchanged** ⇒ `M`'s importers are **not** made dirty on account of `M`.
     Their `iface_dep_hex` still matches; they load from cache. This is the cut
     that kept 20 downstream modules untouched in the measurement.
   - **changed** ⇒ every direct importer of `M` is marked dirty and re-inferred;
     the rule applies transitively (an importer whose *own* interface then
     changes propagates one hop further, and stops as soon as an interface
     stabilizes).

**Rule, precisely:** *a module is re-inferred iff its own source changed, or at
least one direct import's interface hash changed. Interface equality — not source
equality — gates propagation.* Source-only edits (comments, function bodies that
don't alter a signature, private helpers) stop at the editing module.

## Invalidation (full rebuild)

Mirror the core cache's invalidation ingredients — a full front-end rebuild
(every typed blob a miss) is forced by any of:

- **Compiler version** — `kaikai_version` folded into `typed_key`; the KAB2
  header already validates a `kaikai_version` hash (`cache_kaikai_version_hash`).
- **Edition** — the `EDITION` file (`hanga-roa`) folded into `flags_hash`; an
  edition bump changes the language surface and must not reuse typed blobs.
- **Toolchain identity** — bin/kai already derives `--toolchain-id` from the
  `kaic2` binary's mtime+size and scopes the core cache under it
  (`resolve_core_cache_root()/<tid>`). The typed cache lives under the same
  toolchain-scoped root, so a rebuilt compiler is a clean namespace — never a
  reinterpretation of a foreign blob.
- **Key-version bump** — `uc_typed_key_version` bumped when the derivation or
  layout changes.
- **Flag change** — any flag that alters inferred output (optimization level,
  effect-checking mode, target) folded into `flags_hash`.
- **Stdlib / core change** — a core module edit already changes its
  `core_cache` content hash; because user modules import the core, the core's
  interface participates in each user module's `iface_dep_hex`, so a core
  interface change reinvalidates dependent user modules by the same cut.

Invalidation stays a **property of the key**, never a sweep: a key mismatch is a
miss, and a stale blob is simply never named.

## What is NOT avoided

The avoidance is **front-end only** (parse/resolve/infer, per module, plus the
unchanged-interface downstream cuts). Explicitly **not** avoided:

- **Whole-program monomorphization stays the codegen constraint.** Monomorph
  specializes each generic across the *whole* program; it cannot run per-module
  in isolation and this design does not attempt to make it. Even when every
  module's front-end loads from cache, monomorph re-runs whole-program.
- **Perceus, lowering, emit, and the `cc`/link step** likewise re-run. #1298
  measured the back-half + `cc` as the dominant cost of a warm rebuild; this
  design does **not** touch them (that is #1296's territory — per-module header
  slices / the `.o` cache). The front-end cut removes redundant parse+infer
  work; it does not remove codegen.

So the realistic ceiling here is "skip parse/resolve/infer for unchanged
modules and their unchanged-interface dependents", not "skip the build". The
6–20× the issue cites is the *edit-loop* delta this unlocks against today's flat
recompile — bounded below by the whole-program back-half that always runs.

## Open questions for the implementing lane

The KAB2 codec serializes **surface `[Decl]` only**; a **typed** payload schema
(`ModuleEnvDelta` + `TypedProgram` contribution) is the hard prerequisite and the
first blocking task — without it there is nothing to key an interface on. Three
things the lane must settle before the cut is sound: **(a)** a *canonical*
serialization of `ModuleEnvDelta` so logically-equal interfaces hash equal
regardless of inference order (the fold does not guarantee stable order today);
**(b)** whether the pre-typer cross-module tables threaded *around* the fold —
effect aliases, transparent type aliases, and `proto_impls` (`ProtoImplReg`),
which are **not** part of `ModuleEnvDelta` — must be folded into the interface
hash or a separate whole-program invalidation, since a change to any of them can
alter an importer's inference even when every module's delta is unchanged
(`core_cache.kai` already warns a sound post-typecheck cache "would have to cut
both paths" — the modular fold *and* `efn_resolve`); and **(c)** confirming the
`TypecheckedModule.typed` blob a cache hit restores is byte-identical to a fresh
infer's output, so the whole-program monomorph that always runs downstream sees
no skew (the #1298 byte-identical-C property is the test to preserve).
