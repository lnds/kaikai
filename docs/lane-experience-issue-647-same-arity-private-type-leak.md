# Lane experience report — issue #647 same-arity private type leak

Best-effort retrospective by the implementing agent.

## Goal

Close issue #647 — *typer: same-arity private-type collisions
still leak (follow-up to #643)*. Direct continuation of #643 /
PR #646's arity-aware variant walker: that fix discriminates
collisions where prelude and user differ in arity
(`Tree[k, v]` vs `Tree`) but NOT collisions where they share
arity (private `type SplitN = SplitOk | SplitShort` in
`stdlib/crypto/hash.kai` vs. user `type SplitN = LocalA |
LocalB`). The two stdlib types that fall into this trap
(`SplitN` and `NB_FragR` in `stdlib/regexp.kai`) lived in the
audit script's documented skip-list; this lane retires those
skips.

## Approach picked — Option 1 (mangle in the declaring module)

The issue body offered three options; the brief asked for
Option 1 with a stop-and-escalate gate if it turned out
structurally infeasible. Option 1 held up.

The fix renames every non-`pub` `DType T` of `TBSum` body in a
prelude segment from `T` to `<module>::T` at the AST level, and
rewrites every `TyName(T, args)` inside decls of the same
module to `TyName(<module>::T, args)`. Cross-module references
cannot reach a mangled name (private types are not exported),
so user redeclarations of the bare name now live in a disjoint
variant-table bucket from the prelude's. The same is true the
other way: the prelude's exhaustiveness check resolves against
the mangled key.

Options 2 (per-module name scopes for non-`pub` types) and 3
(reject user redeclaration) were considered and ruled out per
the issue body. Option 2 would touch every `TyEnv` builder and
lookup site (~300+ LOC, multi-week lane); Option 3 has poor UX
because the user has no way to discover that `SplitN` is
reserved by a prelude file.

## Bug site

The fix touches one site:

- `stage2/compiler.kai` `compile_source`, right after
  `qualified_prelude` is built. Insert
  `mangle_prelude_segments(qualified_prelude, prelude_segs,
  list_length(qualified_prelude))` before the
  `list_append(qualified_prelude, qualified_decls)` that
  produces `merged_raw`. Approximately line 61283 pre-fix.

All the helpers live in a new ~440 LOC block in
`stage2/compiler.kai` near the `expand_ta_*` family — modelled
after that pass's TypeExpr-rewriting shape (DType / DFn /
DAxiom / DConst / DEffect / DProtocol / DImpl / DDerive /
DUnstable / DAttribPure / DTest / DBench / DCheck plus walkers
for TypeExpr / TypeBody / RowExpr / RowLabel / Param /
FieldDecl / Variant / EffectOp / ProtoOp / Expr / Pattern /
Stmt / Arm / HClause / HReturn). The boilerplate is structural
— each Decl / Expr / Stmt / Pattern carrier that holds a
TypeExpr needs its own walker arm — but no single arm is
clever. The deciding helper is `priv_ty_lookup(entries,
mod_name, bare)` plus the `mangle_te` arm that swaps the
TyName name when `(home_mod, bare)` is in the entries table.

## What shipped

### Mangling pass

- `PrivTyEntry = PTE(String, String, String)` — one entry per
  private prelude DType: `(home_mod, bare, mangled)` where
  `mangled = "<mod>::<bare>"`.
- `collect_private_segment_types(decls, mod_name, acc)` walks a
  segment and pulls every non-`pub` `DType` with `TBSum`
  body into an entry. `TBRecord`, `TBAlias`, `TBEffectAlias`
  are skipped — see *Decisions made* §1 and §2.
- `mangle_prelude_segments(decls, segs, n_prelude)` splits the
  flat decl stream into prelude head + root tail, walks each
  prelude segment with its known module name (from the
  `PreludeSegment` carrier `load_preludes` populates), and
  reassembles. The root tail is never walked.
- `mangle_one_decl` + a tree of `mangle_te` / `mangle_typebody`
  / `mangle_variants` / `mangle_fields` / `mangle_expr` etc.
  walkers rewrite TypeExpr references in-place, keyed by
  `(home_mod, bare)`.

### Pipeline placement

The pass runs immediately after `qualified_prelude` is built
(post-`rqc_decls` and post-`qualtype_decls` for the prelude)
and before the prelude/root concat. This is the latest point
where the per-file segment split is still trustworthy: every
downstream pre-typer pass (`synthesize_refine_pred_fns`,
`lower_pattern_narrow_decls`, `extract_pure_names_decls`,
`lower_axioms`, `inject_builtin_effects`,
`expand_aliases_in_decls`, `expand_ta_decls`) may shift counts
in the flat decl stream and would invalidate the segment-based
slice.

By placing the pass before `expand_ta_decls` we deliberately
trade one thing: a transparent alias whose body mentions a
private sum type would have to be expanded against the bare
name first (and then never see the mangled form). In practice
there are none — alias bodies in `stdlib/` reference `pub`
types, not private sums — but the bound is worth recording.

### Fixtures

Two new fixtures land in `examples/shadowing/`:

- `user_redeclares_same_arity_private_splitn.kai` — user
  redeclares `SplitN` with `LocalA` / `LocalB` variants, then
  invokes `sha256([])`. The prelude's `sha_take_n_loop` walks
  `match bs { [] -> SplitShort(...); [b, ...rest] -> ... }`
  whose **result** type is `SplitN`. Pre-#647 the
  exhaustiveness check on consumers of that result reported
  missing `LocalA` / `LocalB`; post-#647 they consult
  `crypto.hash::SplitN`'s variants. Output asserts
  `user=3 sha256_head=227` (227 = 0xe3, the first byte of
  `sha256("")`).

- `user_redeclares_same_arity_private_nbfragr.kai` — user
  redeclares `NB_FragR`, then runs
  `regex_compile("a(b|c)+d")` + `regex_match` on `abccbd`.
  The prelude's `compile_frag` family deconstructs
  `NB_Frag(...)` exhaustively for the regex compilation
  pipeline. Output asserts `user=3 match=1`.

Both fixtures expose `LocalA` and `LocalB` ctors — the
identical pair the audit script synthesises — so the fixture
shape is the canonical repro the audit walks for every other
private prelude type.

### Audit script

`tools/audit-prelude-private-types.sh` previously carried a
documented skip-list for `SplitN` and `NB_FragR`. The skip is
retired; the file now reports `pass=12 fail=0 skip=0`
(previously `pass=10 fail=0 skip=2`). The skip-list comment is
preserved as a placeholder for future opt-outs (with explicit
instructions not to silently re-add entries).

## Decisions made

1. **Records (`TBRecord`) are NOT mangled.** The first iteration
   included them, which broke the prelude. Cause: the typer's
   `rec_find` walks `env.recs` keyed by bare name, so renaming
   `RxRangeParse` to `regexp::RxRangeParse` while leaving
   `rec_find` keyed on `"RxRangeParse"` left every field
   projection (`res_first.st`, etc.) unresolved. The resolver
   fell back to a fn lookup, found one, and reported the
   misleading "UFCS only fires at call sites" diagnostic at
   ~10 sites in `stdlib/regexp.kai`. The fix was to scope the
   mangling to `TBSum` only — the variant-table path is
   arity-aware (#643) and now also module-keyed, but the
   record-table path is unchanged. Issue #643's retrospective
   item 2 already tracks the record-side leak as a separate
   lane; closing it would require a parallel `rec_find`
   rewrite, the same shape as the variant walker. Out of scope
   here.

2. **Effect-aliases (`TBEffectAlias`) and refinement aliases
   (`TBAlias` with `TyRefine`) are NOT mangled.** Effect alias
   names live in the row namespace, not the type namespace the
   variant walker consumes. Refinement aliases expand to a
   `__ref_pred_X` synth fn plus a scalar base type; renaming
   the alias name would orphan the synth fn lookup.

3. **The per-file `PreludeSegment` accumulator drives module
   attribution, not `decl_home_hint_reset`.** First implementation
   reused the streaming `decl_home_hint_reset` algorithm
   `synth_refine_loop` uses. That algorithm only resets `cur_mod`
   on DFn / DAxiom whose `mo` field flips — it has no signal
   to reset on DType (which carries no `mo`). When the root
   file starts with a `type T` decl (no preceding DFn `mo =
   None`), `cur_mod` retains the LAST prelude's home and the
   user's type gets mangled. The visible symptom was
   `error: ...expected SplitN, found mac::SplitN` on the user's
   own match. The segment-driven approach side-steps this: each
   `PSeg(mod_name, decls)` carries an unambiguous module
   attribution by construction.

4. **Pipeline placement before `expand_ta_decls`.** The pass
   runs early, before transparent alias expansion, so the
   per-file segment counts (`list_length(seg_template)`) still
   line up with the decl stream slice. Running later would
   require either a parallel segment-tracker through every
   intervening pass (adds plumbing across ~7 passes) or
   carrying a `n_root_decls`-and-recompute scheme on every
   pass that adds decls. The segment-driven slice is the
   smallest viable plumbing.

5. **`mangle_one_segment` is reused for `DImpl` sub-decls.**
   The impl's methods inherit the impl's home module; recursing
   `mangle_one_segment(sub_decls, entries, home_mod, [])`
   reuses the same walker without threading a separate
   `decl_home_hint` through impls.

6. **Sentinel marker from #643 still applies.** The arity-aware
   walker's sentinel `T::__type_decl_arity_<N>__` continues
   to discriminate same-arity collisions WITHIN the same
   module (a private DType shadowed by another private DType
   of the same name within the same prelude file). Post-#647
   the sentinel is appended under the mangled name
   (`crypto.hash::SplitN::__type_decl_arity_0__`), but the
   walker's logic is unchanged.

## Limitations and follow-ups

1. **Record-side same-arity collisions remain.** A user
   `type RxRangeParse = { f: Int }` would still collide with
   `stdlib/regexp.kai`'s private record. The fix shape would
   mirror this lane's: extend the same mangling pass to emit
   PTE entries for `TBRecord` bodies AND walk `rec_find` /
   the record-table register path to look up under the
   mangled key. Issue #643 retro item 2 catalogues this; not
   in scope here.

2. **Cross-prelude private type with same name.** Two private
   DTypes of the same name in different prelude files (no
   user redeclaration) — e.g. `stdlib/a.kai: type X` and
   `stdlib/b.kai: type X` — were colliding pre-#647 too, and
   now each lives under its own mangled key. Implicit
   improvement; not exercised by the audit because the audit
   only synthesises USER redeclarations.

3. **Diagnostic ergonomics.** A compiler-internal bug in the
   prelude that surfaces a `crypto.hash::SplitN` in an error
   message now shows the mangled form to the user. This is a
   leak of compiler-internal naming into user-facing output.
   Acceptable today (the user never sees these unless prelude
   code itself is broken), but worth surfacing the bare name
   in error messages if this becomes a recurring complaint.
   The fix would be a `display_type_name(t)` formatter that
   strips the `<mod>::` prefix.

4. **Aliases referencing private prelude types.** A
   transparent alias `type Foo = SplitN` declared in the
   prelude would need to be expanded against the bare name
   BEFORE the mangling pass runs (because the alias body is
   not rewritten by the mangler). Today no such alias exists
   in `stdlib/`. If one is added, the pipeline order
   (`mangle_prelude_segments` runs before `expand_ta_decls`)
   means the alias body would carry the bare name into the
   typer, which would resolve correctly to the mangled DType
   via the entry-table lookup. Acceptable until a contradiction
   surfaces.

5. **No pruning of unreferenced entries.** Every non-`pub`
   prelude `TBSum` becomes a PTE entry, even if no other decl
   in its module references it. The cost is small (a few
   strings per type) and the simplicity is worth it.

## Real cost vs estimate

| Phase                            | Estimate | Actual |
|----------------------------------|----------|--------|
| Pre-reading + design decision     | 15 min   | 30 min |
| First-cut implementation          | 60 min   | 50 min |
| Hit + diagnose root-mangling bug  | —        | 25 min |
| Hit + diagnose record-side breakage | —      | 20 min |
| Selfhost gate                     | 5 min    | 5 min  |
| 2 fixtures + audit unblock        | 20 min   | 20 min |
| Lane retro                        | 20 min   | 25 min |

Total: ~3 hours. Two structural surprises:

- The streaming `decl_home_hint_reset` algorithm cannot reset
  on DType decls (DType has no `mo`), so a root-file `type T`
  inherits the last prelude's home and gets mistakenly
  mangled. Fix was to switch to per-file `PreludeSegment`
  attribution.
- TBRecord renaming broke field projection because `rec_find`
  is bare-name-keyed. Fix was to scope the mangler to TBSum
  only and defer the record-side fix.

The boilerplate volume (~440 LOC of walker arms) is higher
than the brief's "~150-200 LOC" estimate — but it parallels
`expand_ta_*` (~480 LOC) and the deciding logic is small
(`priv_ty_lookup` + the `mangle_te` arm that swaps the
TyName name). Most of the diff is mechanical tree-walking.

## Cross-references

- Issue #643 — the arity-aware walker that closed three quarters
  of the leak shape; this lane closes the remaining quarter.
- PR #646 — the closing PR with the lane retro that catalogued
  this follow-up.
- Issue #643 lane retro `docs/lane-experience-issue-643-private-type-leak.md`
  §Limitations and follow-ups item 1 — explicitly named this
  gap as the immediate next lane.
- `tools/audit-prelude-private-types.sh` — the institutional
  regression gate; this lane retires its two-entry skip-list.

## Verification commands

```sh
# Repro from the issue context
cat > /tmp/repro647.kai <<'EOF'
type SplitN = LocalA | LocalB
fn classify(x: SplitN) : Int = match x {
  LocalA -> 1
  LocalB -> 2
}
fn main() : Unit / Stdout = {
  println("#{int_to_string(classify(LocalA))}")
}
EOF
bin/kai run /tmp/repro647.kai          # prints "1"

# New fixtures
bin/kai run examples/shadowing/user_redeclares_same_arity_private_splitn.kai
bin/kai run examples/shadowing/user_redeclares_same_arity_private_nbfragr.kai

# #643 fixtures (regression check)
for f in examples/shadowing/user_*_does_not_collide_with_private_*.kai \
         examples/shadowing/user_redeclares_private_*.kai ; do
  bin/kai run "$f"
done

# Audit script
make test-private-type-shadow-audit    # pass=12 fail=0 skip=0

# Selfhost
make -C stage2 selfhost                # "self-hosting fixed point: OK"
```
