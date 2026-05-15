# Lane retro — issue #597 boundary tagging (Path A pre-blocker for A.1)

Closes the pre-blocker the #461 lane filed: every synthesised top-
level decl produced by the pre-typer cascade now carries its
originating module's `module_origin` tag, so a future cache loader
can call `partition_decls_by_origin(decls, prelude_mods)` and recover
the prelude/user segments the typer fold (PR #578) consumes. Path A
in `docs/cache-design.md`; Path B (cache post-cascade and accept
format-version churn) was the rejected alternative.

## Scope as briefed vs. as shipped

| Briefed | Shipped |
|---|---|
| `make_dispatcher_fn` propagates `mo` from the originating DProtocol | Shipped via `ProtocolReg.origins: [PrOR(proto_name, mo)]`. DProtocol still has no `mo` field; the home is recovered by `decl_home_hint_reset` streaming during `collect_proto_decls`. |
| `build_refine_pred_fn` propagates `mo` from the originating DType | Shipped via `synth_refine_loop` threading `cur_mod`. DType still has no `mo` field; same streaming algorithm. |
| `lower_const_one` / `lower_axiom_one` preserve `mo` from DConst / DAxiom | **No code change** — verified that `tag_decl_module_origin` already pre-lowers DConst/DAxiom for prelude + import paths (`stage2/compiler.kai:56539`, `:56530`). Root-file DConst / DAxiom legitimately produce `DFn(mo=None)` and stay that way. |
| `lower_impl_methods` + `rename_proto_calls_decls` preserve `mo` | **No code change** — verified `:51338` (`DFn(..., mo)` reconstruction) and `:53374` (rename preserves all 10 DFn fields). |
| Partition helper `partition_decls_by_origin` | Shipped as a pure helper at `:56579+`, plus `decl_partition_home` for the few decl variants that wrap an inner DFn (`DAttribPure`, `DDerive`, `DImpl`). |
| `examples/cache/` regression fixture | Filed under `examples/protocols/boundary_tagging_597.kai` so the existing `test-protocols` rule exercises it on both C and LLVM backends without a new Makefile target. |
| `validate_pub_access` audit | Done — runs **before** `lower_protocols` (`:58849` vs `:59067`), so the synthesis sites never feed into pub-access tables. No regression. |
| `efn_resolve` audit | Done — dispatchers' `__proto_<op>` names are internal and unique, so the lookup falls through the `[one] -> Some(one)` arm regardless of `mo`. No regression. |

## What landed (LOC inventory)

`stage2/compiler.kai` only: +169 / −34 = **135 net LOC**, well under
the 300-500 LOC paper estimate from #597. Three structural moves:

1. **`ProtocolReg.origins: [ProtoOriginReg]`** (`:50705-50709`,
   `:50783-50838`). New `PrOR(proto_name, Option[String])` variant
   recording each protocol's home module. `collect_proto_decls`
   threads `target_mod` + `cur_mod` and registers `(name, mo)` for
   every `DProtocol` it walks; `proto_origin_lookup` (a flat scan)
   feeds the value back into `generate_dispatchers`.

2. **`make_dispatcher_fn` signature gains `mo: Option[String]`**
   (`:51389-51403`) — the dispatcher DFn now mints with that tag.
   `generate_dispatchers_loop` is the only caller and reads `mo`
   from `reg.origins`.

3. **`synthesize_refine_pred_fns(decls, target_mod)`** (`:20407-20460`).
   `synth_refine_loop` threads `cur_mod` via `decl_home_hint_reset`;
   `build_refine_pred_fn` accepts the parent `mo` and mints the
   `__ref_pred_X` DFn with it.

Plus a fourth, externally consumable helper:

4. **`partition_decls_by_origin(decls, prelude_mods)`** (`:56579-56644`)
   and the `PartitionedDecls`/`PartitionAcc` record types. Reads the
   `module_origin` of each decl (recursing through DAttribPure /
   DDerive / DImpl to find the inner DFn's `mo`) and routes by
   membership in `prelude_mods`.

Two `lower_protocols` callers needed signature plumbing: `compile_source`
passes `target_mod_name` (`:59067`), and `synthesize_refine_pred_fns`'s
single call site (`:58955`) passes the same. No other caller exists.

## Design decisions

### Why `ProtocolReg.origins` instead of `Option[String]` on `DProtocol`

`DProtocol`, `DType`, `DConst`, and `DAxiom` v1 do not carry a
`module_origin` field — only `DFn` does (per the comment at
`:1555-1563`). Adding `Option[String]` to four more variants is
strictly more invasive than threading `cur_mod` through the two
walkers that actually need the value, and the streaming recovery
algorithm (`decl_home_hint_reset`) is already the canonical way the
codebase recovers a non-DFn decl's home — `validate_pub_access`
(`:50266`) and `collect_priv_decls_loop` (`:50005`) both use it.
Adding fields to `Decl` is a refactor for the day the parser
authoritatively produces a `mo`; #597 stops short of that.

### Why `decl_home_hint_reset` streams correctly through synth sites

The walker enters `collect_proto_decls` and `synth_refine_loop` with
input that has already been through `tag_decls_module_origin` (for
the prelude path; `:56406`) or `tag_target_decls_module_origin` (for
the root file; `:58774`). Both produce a stream where every DFn
carries its home module's name, and every DProtocol / DType /
DConst / DAxiom sits *positionally adjacent to* the DFns of the same
file. `decl_home_hint_reset`'s rule — "if a DFn lands with
`Some(m)`, switch `cur_mod` to `m`; if it lands with `None`, reset
to `target_mod`" — propagates the home onto the surrounding non-DFn
decls correctly because the input is in file order. The same algorithm
that lets `validate_pub_access` attribute a head-of-file DType to its
prelude is exactly what attributes the same file's DProtocol /
DType-with-refinement to the same prelude.

### Why root-file synthesis stays `mo = None`

The partition's contract is "if `mo` is in `prelude_mods`, route to
prelude segment; otherwise to user segment". A root-file dispatcher
that gets tagged with `Some(target_mod_name)` would still route to
the user segment — by definition the target's name is never in
`prelude_mods`. But it would change the C symbol from
`kai___proto_op` to `kai_<target>____proto_op`, which would alter
emit output on every program with a root-file protocol. Keeping
`None` for root-file synthesis preserves selfhost byte-identical and
isn't load-bearing for the partition: the partition reads the field,
not the C symbol.

The implementation enforces this by mapping
`if next_mod == target_mod { None } else { Some(next_mod) }` inside
both walkers. A root-file protocol's `decl_home_hint_reset` returns
`target_mod` (the reset rule), which the walker normalises to
`None`.

## Selfhost byte-identical verification

Verified at every sub-step:

- After Task 1 (`make_dispatcher_fn` + `ProtocolReg.origins`):
  `make clean && make tier0` → `tier0 OK — selfhost byte-identical,
  demos baseline holds`.
- After Task 2 (`synthesize_refine_pred_fns` threading): `make tier0`
  → same result.
- Task 3 / Task 4 are no-code-change verifications, so no rebuild
  needed.

Full Tier 1 was run before opening the PR; result captured in the
PR body.

`stage2/build/stage2.c` itself does change (the new fields in
`ProtocolReg` and the new helpers add code), so the kaic1-emitted
stage 2 source is *not* identical to pre-lane HEAD. What "byte-
identical" means in the kaikai discipline is the **selfhost fixed
point**: kaic2(compiler.kai) → kaic2', and kaic2'(compiler.kai)
emits the same C source kaic2 did. That holds.

## Risk audit — both clauses from #597 closed

1. **`efn_resolve` shadow risk**: The dispatchers all use the
   `__proto_<op>` name, which is internal-only — no user code can
   shadow it because the lexer doesn't accept `__` prefix in
   user-typed identifiers (and the `lower_protocols` pass synthesises
   them after the parser has run anyway). `efn_collect_matching`
   returns a 1-element list, so `[one] -> Some(one)` fires regardless
   of `mo`. **No regression possible.**

2. **`validate_pub_access` (#510)**: Runs at `compile_source:58849`,
   which is **before** `lower_protocols:59067`. The pub-access
   validator never sees synthesised dispatchers or refine-preds, so
   tagging them with `Some(mod)` instead of `None` is invisible to
   it. **No regression possible.**

## Real cost vs estimate

| Activity | Brief estimate | Actual |
|---|---|---|
| Pre-read (issue + #461 retro + cache-design + 4 synth sites) | ~30 min | ~40 min |
| Audit `validate_pub_access` + `efn_resolve` | ~20 min | ~15 min (both already documented) |
| Task 1 (`make_dispatcher_fn`) | ~90 min | ~50 min |
| Task 2 (`build_refine_pred_fn`) | ~60 min | ~25 min (mechanical after Task 1) |
| Task 3 / Task 4 verifications | ~30 min | ~10 min (no change needed) |
| `partition_decls_by_origin` + fixture | ~45 min | ~30 min |
| Tier 1 + retro + PR | ~60 min | ~45 min |
| **Total** | 5-7 h | ~3.5 h |
| LOC | 300-500 | **135 net** (compiler) + ~40 (fixture) |

The estimate was paper; the actual surface was smaller because
threading `cur_mod` through two existing walkers reuses the
`decl_home_hint_reset` infrastructure, and tasks 3 / 4 turned out
to need zero code change after auditing the existing call paths.

## Follow-ups

- **#461 A.1 cache lane is unblocked**: the partition function is
  in place. The next lane wiring `compile_source` to call
  `partition_decls_by_origin` followed by a two-segment
  `typecheck_program` is mechanical. **Bench-only spike before
  committing** per the #461 retro's recommendation: KAB2 saved
  0.03 s where the design projected 0.41 s, and A.1's 0.59 s
  projection has not been measured against loader cost.
- **`PartitionAcc` is internal**; if a future caller wants a
  streaming partition (no list reverse), refactor it then. Today's
  single use site (the cache loader the A.1 lane writes) doesn't
  need streaming.
- **No DProtocol / DType / DConst / DAxiom field added**. If a
  future lane decides the streaming recovery is too clever, the
  refactor to add `Option[String]` is sized for that lane, not this
  one.

## Bitácora notes for the A.1 implementer

- The single entry point is `partition_decls_by_origin(decls,
  prelude_mods)`. `prelude_mods` is the list of basenames the driver
  already constructs at `compile_source:58783` — pass that list
  through. The output's `prelude_decls` is the cached segment; the
  `user_decls` is the rest.
- The partition is **exhaustive only because every synth site now
  tags**. If a future lane adds a new synthesis site without
  propagating `mo`, this contract breaks. The places to watch are
  `lower_protocols`, `synthesize_refine_pred_fns`,
  `lower_const_one`, `lower_axiom_one`, and any new walker that
  produces a top-level DFn — every one of them is exercised by
  tier 0's `selfhost byte-identical` + `demos-no-regression`.
- The streaming `decl_home_hint_reset` is fragile to **input order**.
  `lower_protocols` and `synthesize_refine_pred_fns` both receive
  `merged_raw = list_append(qualified_prelude, qualified_decls)`,
  so prelude DFns lead and a DProtocol at the head of
  `stdlib/protocols.kai` sees `cur_mod = "protocols"` before any
  user file's decl arrives. If a future lane reorders the merge,
  the partition's source-of-truth shifts; selfhost will catch it
  immediately (dispatchers would carry wrong tags and `c_sym`
  emission would diverge).

## What this lane did NOT do

- **No cache code shipped.** The partition function is unused until
  the A.1 lane wires it into `compile_source`.
- **No DProtocol / DType / DConst / DAxiom variant change.** The
  cleaner long-term shape is for each variant to carry its own
  `Option[String]`, but that's a parser-side refactor with much
  broader churn and is not on #597's path.
- **No prelude qualified-call regression fixture beyond the existing
  `examples/protocols/` and `examples/refinements/` directories.**
  The boundary_tagging_597 fixture covers the root-file synthesis
  arm of both Task 1 and Task 2 (root-file protocol + root-file
  refinement); the prelude arm is covered by every demo that calls
  any `Show.show` / `Eq.eq` / `Ord.cmp` (so the entire
  `demos-no-regression` suite).
