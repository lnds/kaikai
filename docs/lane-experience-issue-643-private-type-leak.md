# Lane experience report — issue #643 private-type leak

Best-effort retrospective by the implementing agent.

## Goal

Close issue #643 — *typer: private types in prelude leak into user
scope (cap 5 Tree regression, book ch5 blocked)*. The canonical
repro:

```kai
type Tree
  = Leaf
  | Node(Int, Tree, Tree)

fn height(t: Tree) : Int = match t {
  Leaf            -> 0
  Node(_, l, r)   -> 1 + max_int(height(l), height(r))
}
```

failed at the user's `match t` because the typer's variant table
surfaced the private prelude `type Tree[k, v] = TEmpty | TNode(...)`
from `stdlib/collections/map.kai` and reported "covered: TNode,
TEmpty; missing: Leaf, Node". Companion to PR #510 (which closed
the same shape on the **function** side) and follow-up to PR #625
(removed the `pub` flag from `Tree[k, v]` but did not patch the
resolver leak).

## Bug site

The leak lived in `stage2/compiler.kai`'s variant-table lookup
path. Two functions are load-bearing:

- `variants_of_type_loop` (`stage2/compiler.kai:35464` pre-fix)
  walked every `TyEntry` whose return tycon matched the requested
  name and returned ALL such variants, with no discriminator
  beyond the bare name.
- `extract_return_tycon` returned only the tycon name, discarding
  the type argument list.

The flat name-keyed table is shared between every module compiled
in a single `compile_source` invocation. User-side `match`
exhaustiveness consulted the same table the prelude consumed.

## Approach

**Option A (least invasive, picked):** make the variant walker
arity-aware. Almost every legitimate name collision between user
code and a private prelude type today is an **arity difference**
— the prelude's `Tree[k, v]` (arity 2) vs. the user's `Tree`
(arity 0). Pairing the bare-name match with an arity check
correctly separates the two declarations.

**Option B (deferred):** module-scoped type names — non-`pub`
types live in a separate module namespace and never enter the
shared bare-name table. Cleaner long-term but invasive (record
walker, fn scheme registration, qualifier rewriting). Not
required for the load-bearing book ch5 case and deferred.

## What shipped

### Arity-aware variant lookup

- `TyConArity = TCA(String, Int)` carries the return tycon's name
  and its type-arg arity.
- `extract_return_tycon_arity` parallels `extract_return_tycon`
  and returns this pair instead of just the name.
- `variants_of_type_loop_arity` walks `env.entries` newest-first,
  pinning the arity from either an explicit caller `arity`
  argument or from the first matching variant. Subsequent
  variants whose return tycon arity differs from the pin are
  skipped — that is what keeps `Tree[k, v]` variants out of the
  user's `Tree` (and vice versa).
- `variants_of_type_arity` is the new public entry; callers that
  already hold a full `TyCon(name, args)` pass `list_length(args)`
  as the arity. Two callers updated: `check_exhaustive`
  (top-level `TyCon` scrutinees) and `check_union_component`
  (per-component recursion).
- Legacy `variants_of_type` / `variants_of_type_loop` still
  exist; they pass `pin = -1` so the first matching variant
  defines the canonical arity. Used by callers that don't have
  the scrutinee's full `TyCon` (mostly diagnostic emitters and
  the protocol dispatch helpers).

### Boundary sentinels for same-arity collisions

Arity alone does not separate two arity-0 sum types of the same
name (e.g. private `type SplitN = SplitOk | SplitShort` in
`stdlib/crypto/hash.kai` vs. a user redeclaration `type SplitN
= ...`). For those cases the walker also consumes a sentinel
inserted alongside the variants:

- `ty_env_decl_marker_name(tname, arity)` returns
  `"<tname>::__type_decl_arity_<N>__"`. The `::` separator
  prevents the existing qualified-key suffix scanners from
  treating the marker as a `<mod>::<fname>` candidate (the suffix
  is `__type_decl_arity_<N>__`, never a user-facing target name).
- `ty_env_add_decl_marker` prepends one such marker BEFORE the
  variants are registered. Since `ty_env_add` cons-prepends, the
  marker ends up AFTER the variants in head-first order. The
  walker, walking newest-first, accepts variants until it crosses
  the marker for its current (target, arity) pair — at which
  point it has left the current declaration's region and any
  earlier same-(name, arity) variants belong to a shadowed
  declaration.
- The marker's scheme is `mono(TyAny)` — accidental reads return
  `None` from `extract_return_tycon_arity` and never participate
  in any lookup that walks variants by return tycon.

### Union summary path

`env.unions` (the per-`DType TBSum` summary harvested at env-build
time, issue #198) was non-arity-aware. The arity-aware lookup
either:

- Uses `union_merge_with_summary_filtered` to merge in only those
  `UnionInfo` entries whose components overlap the already-
  selected arity-matched variants (so a `Tree[k, v]` union
  summary does not pollute a user-scope `Tree` query).
- Falls back to the legacy bare-name walk when no arity match
  succeeds (records / opaque tycons whose variant entries are
  absent still surface their union components — preserves the
  byte-identical baseline for legacy single-arity types).

### Fixtures

Four fixtures land under `examples/shadowing/`:

- `user_tree_does_not_collide_with_private_stdlib_tree.kai` — the
  canonical book ch5 example (`type Tree = Leaf | Node(...)`
  vs. private `Tree[k, v]`). Prints `height: 2`.
- `user_state_does_not_collide_with_private_record.kai` — record
  shape with a name that does **not** collide with today's stdlib
  privates. Documents the shape and links to the audit's
  documented limitation for same-arity record names.
- `user_redeclares_private_after_import_other.kai` — explicit
  `import collections.set` plus a local `type Tree`. Verifies
  the arity discriminator survives an unrelated import path.
- `user_redeclares_private_recursive_type.kai` — deeper recursive
  `Tree` traversal (`sum_tree`, `depth`) to exercise the
  match-arm path repeatedly post-fix.

Each ships with a `.out.expected` golden tested by the existing
`make test-stdlib` runner (`examples/shadowing` is picked up by
`tests/runner.sh`'s glob).

### Audit script

`tools/audit-prelude-private-types.sh` walks every non-`pub type`
in `stdlib/`, synthesizes a minimal user program that redeclares
the same name as a sum type, and runs `bin/kai build`. Wired into
the `tier1` chain via the new `test-private-type-shadow-audit`
target. The script ships with a documented skip-list — see
"Limitations" below.

## Load-bearing detail — the `args` shadow trap

The first implementation pinned the arity to 0 for every variant,
producing a spurious "covered" / "missing" inversion. Root cause:
the helper used pattern slot name `args`, which collides with the
prelude's `args : () -> [String]` CLI-arg fn. Stage 0/1's
resolver silently routed `list_length(args)` to the closure
instead of the local binding, so every arity came out as 0. The
slot was renamed `tas` and the fix worked first try. The
prelude-`args` shadow is recorded in
`feedback_kaikai_param_args_shadow.md` and now also flagged
inline in `extract_return_tycon_arity`'s docstring.

This is the second time this lane that the `args` shadow bit a
typer change — the cost of NOT having a "do not name this `args`"
linter is steeper than any new lint would have been. Worth
revisiting as a Tier 0 sanity probe.

## Limitations and follow-ups

1. **Same-arity sum-type collisions** between a non-`pub` prelude
   type and a user redeclaration trip the prelude's own
   exhaustiveness check, because the prelude code (compiled
   later inside the same single-segment `compile_source`) consults
   the same name-keyed table the user does. The arity-aware
   walker correctly returns the USER's variants for the user's
   scrutinee, but the same lookup from the prelude side now
   resolves against the user's variants — the prelude code fails
   to match `SplitOk` / `SplitShort`.

   Two stdlib types fall into this trap today and live in the
   audit script's skip-list:

   - `SplitN` (`stdlib/crypto/hash.kai:88`).
   - `NB_FragR` (`stdlib/regexp.kai:624`).

   Pre-#643 both shapes failed the same way (in BOTH directions,
   not just one), so this is not a new regression — the canonical
   `Tree` case is fixed, the SplitN / NB_FragR cases remain
   broken to the same extent they were before. Documenting them
   in the audit's skip-list keeps the gate stable.

   The clean fix is per-module name scopes for non-`pub` types,
   either by renaming the private types in their declaring module
   (mangle to `<mod>__<tname>` at register time and rewrite every
   internal `TyCon` reference) or by extending `add_decls_loop` to
   carry a module-scoped env and resolving names against the
   user's scope only when the typer is inside the user's decl
   region. Both are larger lanes — track as a follow-up issue
   referencing this retro and the two stdlib renames it would
   unblock.

2. **Record-side shadow** is untouched. `rec_find` walks
   `env.recs` and returns the first match by name, no arity. A
   user declaring `type Thread = ...` whose name collides with a
   non-`pub` record like `stdlib/regexp.kai`'s `Thread` still
   sees the wrong field set. The `user_state_does_not_collide_with_
   private_record.kai` fixture documents the shape that works
   today (no stdlib `State` collision); the audit script's skip
   list covers the records with actual collisions.

   **Closed by issue #648** (lane retro
   `docs/lane-experience-issue-648-record-side-private-leak.md`)
   — `rec_find_with_field` adds a field-set tie-breaker that
   discriminates same-(name, arity) record collisions without
   the prelude-side regression the variant-side sentinel left
   open. The fixture
   `examples/shadowing/user_thread_does_not_collide_with_private_regexp_thread.kai`
   exercises the closed case; `tools/audit-prelude-private-records.sh`
   institutionalises the gate.

3. **The audit script is one-shot.** It compiles each redeclared-
   type test in sequence (no parallelism, ~1.5s per stdlib type
   on macOS). For the ~12 non-`pub type` declarations in
   `stdlib/`, total wall time is ~20s — acceptable for tier1.
   If `stdlib/` grows past ~50 private types, batch into one
   compile per stdlib module.

## Fixture coverage gaps

- No fixture exercises an `import`-bridged collision where the
  user explicitly imports a module that re-exports a type whose
  name matches a private one in the prelude. The closest
  approximation (`user_redeclares_private_after_import_other.kai`)
  imports `collections.set`, which has no `Tree` in its
  surface, so the prelude leak path is the only one being
  exercised. Worth adding a fixture once an actual cross-module
  candidate exists (none today).

- The audit script tests **sum-type** redeclaration only. A
  record-side audit would synthesize `type X = { f: Int }` for
  each non-`pub type X = { ... }` in `stdlib/`. Deferred until
  the record-side fix lands.

## Real cost vs estimate

| Phase                        | Estimate | Actual |
|------------------------------|----------|--------|
| Localize bug                 | 20 min   | 25 min |
| First-cut fix (arity-only)   | 30 min   | 40 min |
| Selfhost gate                | 5 min    | 5 min  |
| Realize same-arity gap       | —        | 15 min |
| Sentinel-augmented fix       | 30 min   | 35 min |
| Fixtures + audit script      | 25 min   | 30 min |
| Hit + diagnose `args` shadow | —        | 20 min |
| Lane retro + docs            | 20 min   | 25 min |

Total real wall time: ~3 hours, dominated by (a) the `args`
prelude-shadow trap and (b) discovering the same-arity collision
edge after the first-cut arity-only fix shipped. The arity
mechanism itself is small (~80 LOC compiler.kai); the sentinel
addition is ~30 LOC.

## Decisions made

- **Sentinel inside `env.entries` instead of a separate side
  table.** Adding a new field to `TyEnv` would touch every
  `TyEnv { ... }` reconstruction (~40 sites); piggybacking on the
  existing entry list keeps the patch surgical.

- **`mono(TyAny)` for the sentinel's scheme.** `TyAny` is the
  existing "not yet resolved" placeholder; using it ensures any
  accidental scheme dereference fails closed (returns `None`
  from `extract_return_tycon_arity`) rather than fabricating a
  variant.

- **Skip-list in the audit script instead of suppressing the
  diagnostic in the compiler.** The compiler's diagnostic is
  honest about what fails; the audit script's skip-list is a
  visible TODO that future lanes can clear by name.

- **No edits to `validate_type_name_collisions_decls`.** That
  validator only fires for `pub`-exported preludes (consulting
  `ModuleEntry.exports` which excludes private types). Issue
  #643 lives in the non-`pub` path; touching the validator would
  blur the boundary between the two failure modes.

## Cross-references

- PR #510 — closes the function-side analog (`pub`-fn enforcement).
- PR #625 — removed `pub` from `Tree[k, v]`; insufficient on its
  own.
- Issue #644 — constructor overloading for `pub` types (book
  ch17). Out of scope here; the two issues are structurally
  distinct.
- `feedback_kaikai_param_args_shadow.md` — the `args` prelude
  shadow that bit this lane.

## Verification commands

```sh
# Repro from the issue body
cat > /tmp/repro_643.kai <<'EOF'
type Tree = Leaf | Node(Int, Tree, Tree)
fn max_int(a: Int, b: Int) : Int = if a > b { a } else { b }
fn height(t: Tree) : Int = match t {
  Leaf -> 0
  Node(_, l, r) -> 1 + max_int(height(l), height(r))
}
fn main() : Unit / Stdout = {
  let t = Node(1, Node(2, Leaf, Leaf), Leaf)
  println("height: #{height(t)}")
}
EOF
bin/kai run /tmp/repro_643.kai      # prints "height: 2"

# All four fixtures
for f in examples/shadowing/*.kai; do
  bin/kai run "$f"
done

# Audit script
make test-private-type-shadow-audit  # pass=10 fail=0 skip=2

# Selfhost byte-identical
make -C stage2 selfhost              # "self-hosting fixed point: OK"
```
