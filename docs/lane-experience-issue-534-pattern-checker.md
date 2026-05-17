# Lane experience report — issue #534 (pattern checker + unbound tyvar)

## Goal

Close #534. Phase-2 negative-space audit (#520) surfaced four
typer-correctness gaps that kaikai accepted silently while the
ML/HM lineage rejects:

1. Duplicate binding inside one pattern — `match p { MkPair(x, x) -> x }`.
2. Duplicate variant arm — `match m { Yes(x) -> 1; Yes(y) -> 2 }`.
3. Duplicate literal arm — `match x { 1 -> 10; 1 -> 20; _ -> 0 }`.
4. Unbound type variable in signature — `fn f(x: a) : b = x`.

All four are Tier 1 #1 (Safe at compile time) regressions: programs
the user almost certainly did not intend, compiling clean and
producing silently-wrong behaviour.

## Scope as planned vs as shipped

**Planned (per brief):**

- Reproduce each of the 4 gaps under the current `bin/kai`.
- Localize the relevant typer site for each.
- Implement 4 fixes (pattern walker bookkeeping for #1, arm
  reachability set for #2 / #3, return-type tyvar scope check
  for #4).
- Relocate 4 fixtures out of `silent_contract/` into final dirs
  with `.err.expected` goldens.
- Selfhost gate **per gap** before moving to the next.
- 120-180 line retro, PR open (no merge).

**Shipped:**

- **Gaps 1, 2, 3 were already closed.** Pre-lane re-run of the
  4 reproducers showed kaikai already rejected the three
  pattern shapes with clear diagnostics. The brief was written
  against a snapshot that pre-dated those three fixes, so the
  per-gap selfhost gate ran exactly once (for gap 4) instead
  of four times. Goldens for gaps 1-3 already live in
  `examples/negative/patterns/` from the prior partial-close
  pass; this lane verifies and does not touch them.
- **Gap 4 (unbound tyvar) closed in this lane.** New checker
  `check_unbound_tyvars` walks every `DFn` / `DAxiom` signature
  after parsing, before inference. Wired into `check_program`
  alongside `check_row_labels`.
- 1 fixture relocated (the other 3 were already in their final
  home).
- Lane retro + PR.

## Decisions and alternatives considered

### Where to place the unbound-tyvar check

Three candidates:

- **Inside `fn_scheme_of_decl` (line 27989).** That is where
  the implicit-tparam promotion happens (R1 fix 2026-04-26).
  Tempting because the data is already there: `declared` +
  `implicit` are computed inline. Rejected because that
  function builds the scheme silently for many callers
  (display, alias rewriting, instantiation prep); rejecting
  here would either break those paths or have to plumb a
  "diagnostics enabled" flag through ~7 call sites.
- **Inside `infer_decl`** (the inferencer entrypoint).
  Rejected because by then the implicit tparams are already
  generalised and the original syntactic shape is gone — we
  would have to reverse-engineer which tyvars came from where.
- **A dedicated pre-inference walker, sibling to
  `check_row_labels`.** Chosen. `check_program` runs
  `check_row_labels` *before* inference for exactly the same
  reason — surface-syntax correctness checks belong before
  type elaboration. The walker only needs to read the AST
  (`Decl`, `TypeExpr`, `RowExpr`); no env / no scheme / no
  unifier state. ~120 LOC including all helpers.

### What counts as "unbound"

The R1 fix (line 27999) makes lowercase TyName names *implicit*
tparams, so `fn id(x: a) : a = x` is valid without `[a]`.
That promotion is too permissive: it also accepts
`fn coerce(x: a) : b = x`, where `b` has nothing on the
left-of-`=` side to constrain it.

The rule shipped:

> A lowercase tyvar that appears in the return type or the
> row labels of a `DFn` / `DAxiom` signature must also appear
> in the explicit `[…]` tparam list OR in at least one
> parameter type. Otherwise it is unbound.

Two carve-outs:

- **Synthetic compiler-generated fns** (name starts with
  `__`) are skipped. Protocol desugar emits `__proto_<op>`
  wrappers whose signatures legitimately carry `proto_self`
  only in return position by construction (e.g. `From[a]`'s
  `from(x: a) : Self`). The soundness of those signatures is
  the desugar pass's responsibility, not the surface checker's.
- **Zero-param fns** (`fn empty() : Map[k, v] = …`) are skipped.
  With no value-level params, *every* tyvar in the return is
  caller-elects: the use site picks the instances, so the
  silent-shadow shape from #534 cannot arise. This matches the
  ML convention for top-level values typed by their signature
  (the caller is the unification source). Removing this carve-out
  would have broken `stdlib/collections/map.kai::empty` and
  similar constructor-style fns; the carve-out is morally
  equivalent to the implicit-`forall` reading of the signature.

### Diagnostic shape

```
error: unbound type variable: `b`
  --> file:line:col
  = note: type variable `b` appears in the return type or
          effect row of `fn coerce` but not in any parameter
  = help: introduce it as an explicit tparam (e.g.
          `fn coerce[b](...) : ...`)
```

Anchored at the fn decl's own line/col (we do not have a
per-tyvar source position handy; the TypeExpr line/col tracks
the outermost `TyName` only). The note disambiguates the
return-or-row provenance; the help offers the literal fix the
user almost always wants.

## Structural surprises the brief did not anticipate

- **3 of 4 gaps already closed.** Brief was clearly written
  against an earlier snapshot. Discovered by running the four
  reproducers from §"Paso 1" verbatim against the current
  `bin/kai`. Saved ~3 hours of redundant pattern-walker work.
- **`fn empty() : Map[k, v]` is idiomatic and ubiquitous.**
  First version of the check rejected this and flooded with
  errors on `stdlib/collections/map.kai`. The carve-out for
  zero-param fns is the principled answer (caller-elects), but
  it took one false-positive scare to see it clearly.
- **`__proto_*` wrappers carry `proto_self` only in return.**
  Same shape as the bug, but legitimate by construction (proto
  desugar). Skip via name prefix — robust and obvious; the
  alternative (threading a "desugared" boolean through
  `DFn`) would have changed the AST surface for ~zero gain.

## Fixtures and coverage

- `examples/negative/type_invariants/unbound_tyvar_in_signature.kai`
  — moved out of `silent_contract/`, new
  `.err.expected` golden anchors `error: unbound type variable: `b``.
- `examples/negative/silent_contract/README.md` row #534 flipped
  from "partial close" to "closed".
- `tools/test-negative.sh` summary now 104 PASS, 0 FAIL (was 103,
  with shape 4 silently passing through the silent_contract skip).

No new fixtures added for gaps 1-3 (already present); the
existing goldens in `examples/negative/patterns/` cover them.

## Selfhost + tier gates

- **Selfhost byte-identical** confirmed (`make -C stage2
  selfhost`: `self-hosting fixed point: OK`).
- **Tier 0** green (33 demos baseline holds; selfhost byte-identical).
- **Tier 1** green (full sweep at end of lane).
- **Test-negative** green (104 PASS).

## Real cost vs estimate

Estimate per brief: ~200-300 LOC compiler, 4 selfhost gates,
4 fixture migrations, ~half-day.

Actual: ~120 LOC compiler (only gap 4 needed compiler work),
1 selfhost gate (only gap 4 changed `stage2/compiler.kai`),
1 fixture migration. Lane converged in ~1.5 hours including
retro and PR prep. Most of the savings came from discovering
gaps 1-3 were already closed.

## Follow-ups

- **Or-pattern semantics.** The current pattern walker rejects
  duplicate bindings; an or-pattern `A | B` legitimately binds
  the same name in both arms. The existing fixture is a
  positive smoke test, not an explicit negative anchor — worth
  adding a fixture once the or-pattern surface stabilises.
- **Multi-position spans for unbound-tyvar diag.** Today the
  caret points at the fn declaration line. A future ergonomic
  pass could thread the TyName(line, col) from inside the
  return TypeExpr to anchor the caret directly at `b`. Low
  priority — the note + help already give the user enough to
  fix.
- **`DAxiom` coverage.** The check fires for axioms too, but
  axioms in the codebase today universally use explicit
  `[T1, T2]` lists, so this branch is currently untested. A
  fixture under `examples/negative/axiom/` would harden it;
  not load-bearing — the carve-out logic is identical to
  `DFn`.
