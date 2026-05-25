# Driver cross-section analysis — issue #677 phase 1k pre-flight

Status: **analysis only, no extraction.** This lane investigated the
`# cli + driver` section of `stage2/main.kai` for extraction into
`compiler/driver.kai` and concluded it is **not safely extractable as a
single module in this PR.** The reasoning, the verified dependency data,
and a recommended pivot are below. The decision is left to the
integrator, following the precedent of `docs/lane-parse-cross-section-analysis.md`
(PR #684, which likewise shipped analysis-only and deferred the cut).

## TL;DR

- The brief scoped phase 1k as "extract lines ~52789–EOF into
  `compiler/driver.kai`". The section is real and heterogeneous
  (~7 450 LOC, 399 fns, 79 types), exactly as the brief warned.
- **The driver is the top-of-pipeline orchestrator.** It calls ~143
  symbols still defined in `main.kai` — the typer, resolver, emitter,
  every desugar, perceus/unbox, protocol lowering, the validators, and
  every dump path. Extracting it would force `main.kai` to `pub`-export
  ~140 functions to a sibling module, and would invert the natural
  extraction order: the driver should be the *last* thing extracted,
  after the pipeline stages it sits on top of, not the third "easy" one.
- The five sub-sections (core cli / library mode / `compile_source` /
  cache codec / AST serde) are **mutually recursive**, so the block also
  cannot be cleanly sub-split along its own seams without surgery.
- `fn main()` lives at the very end of the range and stays in `main.kai`
  regardless.
- **There IS a clean, self-contained cut inside this range: the cache
  codec + AST serde (the `Cache*Pos` types and `cache_*_to_hex` /
  `cache_hex_to_*` functions).** It has *zero* inbound references from
  the rest of the compiler and depends only on AST types + itself.
  ~3 360 LOC extract cleanly into `compiler/cache.kai` with a 2-function
  public surface. This is the recommended phase-1k pivot if a code-moving
  lane (not just analysis) is wanted now.

## Boundaries (line numbers in `stage2/main.kai` after rebasing onto
`origin/main` @ 7c8f893, PR #685 resolve-extract; file is 59 244 LOC)

> Note for future readers: the original brief cited line numbers ~994
> higher (header at 52788, EOF 60238). Those predate the resolve-extract
> merge. The numbers below are post-rebase and authoritative for this
> branch. The *topology* is identical either way.

| Sub | Description | Lines | fns | types |
|-----|-------------|-------|-----|-------|
| A | core cli + driver (`Mode`/`BuildMode`/`CliOptions`, arg parsing, prelude load, builtin-effect injection) | 51794–53511 | 99 | 8 |
| B | #454 library mode (typed-AST queries) | 53512–55126 | 100 | 9 |
| C | `compile_source` + `run` orchestration | 55127–55879 | 2 | 0 |
| D | #592 `.kab` cache codec (hex primitives) | 55880–56246 | 28 | 9 |
| E | #452 AST serde (`Cache*Pos` types + serialize/deserialize) | 56247–59244 | 162 + `main` | 53 |

Totals: **399 fns, 79 types** (matches the brief). `fn main()` is at
line 59241, inside sub-section E's tail.

Sub-section headers verified with:

```sh
grep -nE "^# =" stage2/main.kai | awk -F: '$1 > 50000'
```

## Why the whole driver is not extractable now

### 1. Outbound coupling: the driver imports almost all of `main.kai`

The range calls **143 head-defined symbols** (lines 1–51793) plus 44
module-defined symbols. The head-defined ones are the entire compiler
pipeline the driver orchestrates:

- **Lex/report:** `report_errors`, `dump_tokens`
- **Resolve/alias:** `resolve_ty`, `expand_aliases_in_decls`,
  `expand_ta_decls`, `build_ty_env`
- **Typer:** `check_program`, `infer_program`,
  `infer_program_with_protos_cached`, `monomorphise`
- **Desugars/lowering:** `desugar_*_decls` (index / pos-records / use /
  var / interp), `lower_axioms`, `lower_consts`,
  `synthesize_refine_pred_fns`, `lower_protocols`,
  `resolve_protocol_calls`
- **Memory/codegen passes:** `unbox_pass`, `perceus_pass`,
  `tcrec_rewrite_decls`
- **Emit entry points:** `emit_program`, `emit_program_llvm`,
  `emit_main_wrapper`
- **Imports:** `expand_imports`, `process_imports`, `collect_pub_exports`
  (+ `ModuleEntry`)
- **Validation:** `validate_resolved_protocols`,
  `validate_typer_invariants`, the `validate_*_collisions_decls` family,
  `validate_pub_access`
- **Dumps:** `dump_program`, `dump_types`, `dump_typed`, `dump_mono`,
  `dump_holes(_json)`, `dump_effects_json`, `strict_holes_check`
- **fmt:** `fmt_program`, `collect_comments` (see §"fmt adjacency")

Module deps it already shares with `main.kai` (re-imported cleanly):
`compiler.ast` (the whole AST type surface + `mk_expr`/`mk_ty`),
`compiler.lex` (`tokenize`), `compiler.parse` (`parse_program`,
`parse_optional_arm_refine`), `compiler.util`, `compiler.diag`.

Extracting the driver means every one of those ~140 head symbols becomes
`pub`. That is one of the largest public-surface expansions in the whole
modularization effort, and it is exactly backwards: a `compiler/driver.kai`
would be a thin shell importing nearly all of `main.kai`, while `main.kai`
would retain the bodies but be forced to export them. The dependency
arrow points the wrong way for this to be phase "1k of the 3 easy ones".

### 2. Inbound coupling: only 7 symbols are needed by head

Conversely, only **7** range-defined symbols are referenced from outside
the range — these would be the `pub` surface *if* the driver were
extracted:

| Symbol | Kind | Def | External callers |
|--------|------|-----|------------------|
| `BuildMode` | type | 52879→51885 | `emit_main_wrapper`, `emit_program`, `emit_program_llvm` |
| `PreludeSegment` | type | 53503→52509 | `mangle_prelude_segments`, `validate_pub_access`, `collect_pub_access_table`, `cpa_segs` |
| `ty_name_zero` | fn | 53829→52835 | EVar table @ 2786–2787 |
| `edition_at_least` | fn | 53017→52023 | @ 25393 |
| `builtin_effect_names` | fn | 53884→52890 | @ 31360 |
| `prelude_module_name` | fn | 53235→52241 | @ 30658, 31817 |
| `tag_decls_module_origin` | fn | 53391→52397 | @ 44366 |

(Arrows show the agent's pre-rebase line → this branch's line.)

Two of these are a latent design smell, not a blocker: `BuildMode` and
`PreludeSegment` are *types* defined deep in the driver but consumed by
the emitter and the prelude-mangling / pub-access passes that live up in
`main.kai`. They arguably belong in `compiler.ast` (or a shared
`compiler.types` module). A future lane that lifts those two types to
`ast` would shrink the driver's eventual pub surface and untangle the
emitter from the cli layer. Flagged for sequencing, below.

### 3. Intra-range cohesion: the sub-sections are mutually recursive

A–E do not form a DAG that would let us sub-split the block along its own
section headers. The cross-calls:

- **A → E:** `load_prelude_cached` (A, line 52616) calls
  `cache_deserialize_module` (E, line 59114).
- **C → E:** `run` (C, line 55837) calls `cache_roundtrip_self_test` and
  `emit_prelude_cache_for` (both E).
- **E-entry → A:** `emit_prelude_cache_for` (E, line 59211) calls A's
  `prelude_module_name`, `strip_prelude_tests`,
  `tag_decls_module_origin`, plus E's own `cache_serialize_module`.

So you cannot, e.g., extract "library mode" (B) alone without dragging
`compile_source` machinery, and you cannot extract the serde block (E)
naïvely because its *entry fns* reach back into A.

## The clean cut that DOES exist: `compiler/cache.kai` (D + E-core)

Despite the entry-fn entanglement, the **codec itself is pristine**.
Every `Cache*Pos` type and every `cache_*_to_hex` / `cache_hex_to_*`
function (D, 55880–56246, and E-core, 56247–~59100) calls **only AST
types and each other**. Verified:

```sh
# zero hits for any of these from outside the range:
for sym in cache_decl_to_hex cache_hex_to_decl cache_expr_to_hex \
           CacheDeclPos CacheExprPos cache_int_to_hex; do
  grep -n "\b$sym\b" stage2/main.kai | awk -F: '$1 < 51794'
done
```

The only things tying serde to the driver are the four bridge fns in
E's tail (~59100–59244):

- `cache_serialize_module` (59110)
- `cache_deserialize_module` (59114)
- `cache_roundtrip_self_test` (59120-ish)
- `emit_prelude_cache_for` (59211)

These mix pure serde with prelude helpers (`prelude_module_name`,
`strip_prelude_tests`, `tag_decls_module_origin`) and the `main` entry.

**Recommended cut for a code-moving phase 1k:** move D + E-core (the
codec) into `compiler/cache.kai`, and *leave the four bridge fns in
`main.kai`*. The bridges then call `cache.kai`'s two public entry points:

- `pub fn cache_serialize_module(decls, source_sha) : String`
- `pub fn cache_deserialize_module(buf) : Option[CacheDeserialized]`

That keeps the prelude glue where the prelude helpers already live, and
the entire ~3 360-LOC codec leaves `main.kai` with a **2-function public
surface** — versus the driver's ~140. `compiler/cache.kai` imports only
`compiler.ast` (every AST node type the codec serializes), and the
`CacheDeserialized` type moves with it (also `pub`, since the bridge
matches on it). `cache_roundtrip_self_test` can either move with the
codec (it tests the codec) or stay in the bridge calling the public API;
moving it is cleaner.

This is a genuinely easy extraction with a small blast radius — a far
better fit for "the 3 easy ones" than the driver, which should be the
*hard last* one.

## fmt adjacency (coordination with the parallel fmt-extract lane)

The driver range calls exactly **two** fmt functions, both defined in
the fmt brother's range (~50029–51792 on this branch):

- `fmt_program` — called inside `compile_source`'s `MFmt` path.
- `collect_comments` — called alongside it.

It does **not** call `fmt_decls`, `run_fmt`, or `fmt_expr`. **Action for
the fmt lane:** `fmt_program` and `collect_comments` must be exported
`pub` from `compiler/fmt.kai`. Whenever the driver is eventually
extracted (after the pipeline modules land), it will add
`import compiler.fmt` and rely on those two being public. This is the
only interaction between the two lanes beyond the trivial
Makefile/`main.kai` import-line merge conflicts the brief already
anticipated.

## Recommended sequencing

1. **Now (phase 1k, if a code lane is wanted):** extract the cache codec
   + serde into `compiler/cache.kai` (the clean cut above). Small pub
   surface, self-contained, exercises the `Cache*Pos` machinery in a
   dedicated `test_cache.kai`. Leave the four bridge fns in `main.kai`.
2. **Next phases:** extract the remaining pipeline stages the driver
   orchestrates — the typer (`infer_*`/`check_program`/`monomorphise`),
   the emitter (`emit_program*`), the desugars, perceus/unbox, protocol
   lowering, validators. Each is a larger lane than the "3 easy ones".
3. **Lift `BuildMode` and `PreludeSegment` into `compiler.ast`** (or a
   shared types module) in whatever lane touches the emitter, removing
   two of the driver's eventual `pub` exports.
4. **Last:** extract `compiler/driver.kai` (core cli + library mode +
   `compile_source` + `run`). By then it imports modules, not bodies, and
   its pub surface is the 5 remaining inbound symbols (`ty_name_zero`,
   `edition_at_least`, `builtin_effect_names`, `prelude_module_name`,
   `tag_decls_module_origin`) — small and stable. `fn main()` stays in
   `main.kai`.

### Why this order

The modularization invariant is that a module imports the modules it sits
*above*. The driver sits at the apex of the pipeline, so it must be
extracted after — not before — the stages beneath it, or the import arrow
inverts and `main.kai` becomes a `pub`-export dumping ground. The cache
codec is the exception: it sits *beside* the pipeline (a pure
AST↔bytes function with no upward calls), which is precisely why it cuts
cleanly today.

### What NOT to do

- Do **not** extract the driver as a thin shell that `pub`-imports ~140
  symbols from `main.kai`. It compiles, but it inverts the dependency
  direction and bloats the public surface — the opposite of what
  modularization is for.
- Do **not** try to sub-split A/B/C along the section headers: A↔E and
  C↔E cross-calls mean those seams are not real module boundaries.
- Do **not** move `fn main()` — it is the runtime entry and trivially
  calls `run(parse_cli(args()))`.

## Verification commands a reviewer can run

```sh
# section boundaries
grep -nE "^# =" stage2/main.kai | awk -F: '$1 > 50000'

# main() stays at the tail of the range
grep -n "^fn main(" stage2/main.kai

# the 7 inbound-pub symbols and their external callers
for s in BuildMode PreludeSegment ty_name_zero edition_at_least \
         builtin_effect_names prelude_module_name tag_decls_module_origin; do
  echo "== $s =="; grep -n "\b$s\b" stage2/main.kai | awk -F: '$1 < 51794'
done

# cache codec has ZERO inbound refs from head (clean cut)
for s in cache_decl_to_hex cache_hex_to_decl CacheDeclPos cache_int_to_hex; do
  echo "== $s =="; grep -n "\b$s\b" stage2/main.kai | awk -F: '$1 < 51794'
done

# the bridge fns and their cross-section calls
grep -n "^fn cache_serialize_module\|^fn cache_deserialize_module\|\
^fn cache_roundtrip_self_test\|^fn emit_prelude_cache_for\|\
^fn load_prelude_cached" stage2/main.kai
```

## Notes for the integrator

- This PR contains **only this analysis doc** — no code moved, no
  Makefile change, no `main.kai` import line. The `driver-extract`
  branch is otherwise identical to `origin/main` @ 7c8f893.
- The decision you own: (a) accept the analysis and re-scope phase 1k to
  the `compiler/cache.kai` cut (recommended), (b) defer all of it until
  the pipeline-stage lanes land, or (c) override and extract the driver
  shell anyway. The data above supports (a) or (b).
- If you pick (a), it is a fresh code lane on a fresh branch — this
  analysis branch should merge as docs-only or be folded into that lane's
  first commit.
- The fmt brother lane must export `fmt_program` and `collect_comments`
  as `pub` regardless of which option you pick — that is the one hard
  cross-lane dependency.
