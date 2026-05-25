# Lane experience — issue #677 phase 1n0: sink the protocol-registry types

## Scope as planned

Move the five protocol-registry types — `ProtoOpReg`, `ProtoTPReg`,
`ProtoImplReg`, `ProtoOriginReg`, `ProtocolReg` — out of the protocols
block in `stage2/main.kai` and into `stage2/compiler/ast.kai`, adding
`pub` and a single-line `#[doc(...)]` to each. No function moves; no
signature changes. Pure type relocation, byte-identical selfhost.

## Scope as shipped

Exactly as planned. ~40 LOC of `type` declarations (with their
explanatory comment banners) moved from `main.kai:39202–39228` to the
tail of `ast.kai`, after `collect_records_loop`. The five fns and
records that produce/consume the types (`proto_reg_empty`,
`collect_proto_decls`, `register_proto_ops`, `monomorphise`, the
dispatcher generators) stay in `main.kai` and now reference the types
through the already-present `import compiler.ast` (line 27).

A four-line tombstone comment replaces the moved block in `main.kai`,
pointing readers to ast.kai and naming the issue.

## Why a *preventive* sink (cite #692)

This is the same shape as the `RecInfo` sink in phase 1m, but done
**before** the cut rather than mid-cut. The cross-section analysis
landed in #692 (`docs/lane-infer-cross-section-analysis.md`) identified
that these five types sit on a lateral arrow: they appear in
`infer_program_with_protos_cached`'s signature (the infer public
surface, `main.kai:26647`) **and** are produced by the protocols block
(`main.kai:39167–42630`). When the infer lane (N1) and the protocols
lane (post-N1) each extract their half, neither would have a clean home
for the shared types — they would need a lateral `infer ↔ protocols`
import, which the module discipline forbids.

Sinking them into ast.kai (the shared downward sink, per the
"ast sink rule" memory) lets both future lanes import the types
downward, with zero lateral coupling. Doing it now, as a standalone
byte-identical PR, keeps the big N1 extraction free of incidental type
churn.

## Design decisions / alternatives considered

- **ast.kai vs a new `compiler/proto_types.kai`.** The analysis offered
  a dedicated module as an alternative if ast.kai felt overloaded. At
  784 LOC pre-change, ast.kai is comfortably below the other compiler
  modules and already hosts the analogous `RecInfo`/`SumInfo` registry
  types and their `collect_*` projectors. A separate 40-LOC module would
  add a Makefile/import edge for no structural gain. Chose ast.kai.
- **Constructor visibility.** kaikai constructors inherit `pub` from
  their `type`, so `POR`/`PTPR`/`PIR`/`PrOR` and the `ProtocolReg`
  record fields stay visible to the producing fns in main.kai without
  any extra annotation. Verified: selfhost is byte-identical, which it
  could not be if any constructor had gone private.

## Structural surprises

- The hand-off brief's grep (`^type Proto(Op|TP|Impl|Origin)?Reg`)
  matches only four of the five types — `ProtocolReg` is a record type
  whose name doesn't fit the `Proto…Reg` alternation. Confirmed the
  fifth by reading the block directly. Worth noting for the N1/N2 lanes
  that will grep for these.
- Dependency check was clean: the types reference only `String`, `Int`,
  `Option`, plus `Param` and `TypeExpr`, both already `pub` in ast.kai
  (lines 190, 243). No blocking back-reference into main.kai-only types,
  so the sink was unobstructed.

## Fixtures / coverage

No new fixtures. The types carry no behaviour; existing protocols and
infer tests exercise every producer and consumer, and the byte-identical
selfhost is itself the strongest possible regression guard for a pure
relocation (any visibility or resolution change would diverge the
emitted C). The optional cache.kai roundtrip check was not added — these
types are not serialized through the AST cache surface, so it would test
nothing new.

## Cost vs estimate

Estimate: short lane, ~40 LOC, 1–2 commits. Actual: matched. The only
real cost was the selfhost run (the mandatory byte-identical gate);
the edits themselves were two mechanical text moves.

## Follow-ups for next lanes

- **Lane N1** (`compiler/infer.kai`, ~10 500 LOC): can now import these
  types downward from ast.kai. The `infer_program_with_protos_cached`
  signature needs no change — it resolves the types transparently.
- **Lane post-N1** (protocols extraction): same — the protocols block's
  producers reference ast.kai, not a sibling module.
