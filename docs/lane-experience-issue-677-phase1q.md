# Lane experience — issue #677 phase 1q: extract compiler/emit_llvm.kai

The LLVM-backend extraction. Planned as a mechanical relocation in the
mould of N1/N3/N4 (move a coherent block, mirror the downward glue with
a prefix). It shipped as planned in *shape* but the cross-section was an
order of magnitude more coupled than the brief assumed, and three
boundary/renaming surprises ate the whole lane. The headline number:
**138 mirror fns + 14 mirror types**, vs. the brief's "~5 imports, no
typer types." This retro exists mostly to warn the emit_c lane.

## Scope as planned (brief + the #677 phase-1n analysis)

Move the `# LLVM backend (M3a + M3b — skeleton)` block (brief range
13559–18686, ~5127 LOC) into a new `compiler/emit_llvm.kai`. Assumed
imports: ast + diag + util + infer + fnreg. Assumed coupling: "LLVM
emits LLVM IR types, doesn't touch typer types except reading the
already-embedded `Expr.ty`." Public-surface estimate 5–15, entry point
`emit_program_llvm`. TCO confirmed out of range (the analysis pins it to
the emit_c lane; 0 `tcrec_*`, 0 `resolve_protocol_calls` in the range).

## Scope as shipped

One new module, one entry point, selfhost byte-identical. But the block
boundary, the block *location*, and the mirror set were all wrong in the
brief, and the renamer hit two collision classes the grep-level plan
could not see.

### 1. The real lower boundary is 18354, not 18686

Lines 18356–18686 (`ResolveState` / `rs_*` / `collect_pub_exports` /
`resolve_module` / `process_imports` / `expand_imports` / `canon_paths`)
are **module-resolution driver code**, not the LLVM backend. They are
called from the driver (`expand_imports` at ~26191, `collect_pub_exports`
at ~23292/26214) and belong to the eventual driver/modules lane. The
brief's 18686 figure would have pulled ~330 LOC of import machinery into
emit_llvm. The real backend ends at `lookup_op_offset` (18354), right
before the `ResolveState` banner.

### 2. The backend is physically fragmented — 2 detached islands

8 unambiguously-LLVM fns (`llvm_emit_clause_body`, `llvm_clause_arg_sig`,
`llvm_clause_locals`, `llvm_emit_clause_bodies`, `llvm_emit_effect_struct`,
`llvm_emit_op_fn_ptrs`, `llvm_op_param_args`, `llvm_effect_struct_decls`,
main.kai ~4893–4983 and ~6108–6145) live *inside the emit_c region*,
placed there originally by type proximity (`ClauseInfo` / `LamInfo` /
`EffectOp`). They are called only by each other and by the backend core
(`emit_program_llvm` at 18141). Zero emit_c callers. Moved them with the
core — logically the backend, physically misplaced. (First boundary-cut
slip: my initial detached-A range stopped at 4970 and truncated
`llvm_emit_clause_bodies` mid-`match`; the bundle compiled but with a
dangling brace. Always cut on `(fn|type)`-marker boundaries, never on a
hand-counted line.)

### 3. The mirror set is 138, not ~5 — and the brief's coupling model was wrong

The backend reuses the **entire emit_c / driver shared helper layer**,
none of which has been extracted yet: `c_sym`, the `evar_*` / `ffi_*`
families, `build_globals`, `collect_decls`, the proto-dispatch decoder
(`proto_dispatch_*` / `pimpl_*`), the free-variable analysis (`fv_*`),
the alias-rewrite family (`rewrite_alias_*` / `alias_map_*`), and a
partial AST-functor family. Of the 138, 43 are *genuinely shared* with
the C emitter (called from `emit_fn_body` / `emit_call_expr_default` /
`emit_program` / the `dump_*` queries). They cannot move; they are
mirrored with an `lvm_` prefix, leaving main's originals untouched. Plus
**14 main-resident helper types** (`EVar`, `BuildMode`, `ClauseInfo`,
`LamInfo`, `LamCollect`, `FfiRetKind`, `FvScoped`, `PimplRow`,
`ProtoDispatchParts`, `PR2`, `CUVResult`, `CUVStep`, `AliasMap`,
`StmtsRewrite`) re-spelled `Lvm*` with their constructors.

The user, given the choice (mirror-138 / pre-sink emit_shared /
pause-and-report), chose **mirror**, consistent with the standing
"don't block a lane on a future sink" rule. The cost is ~37% duplication
and a divergence debt the emit_c lane must reconcile — flagged below.

### The closure was wrong twice before it was right

This is the load-bearing lesson. The first closure scan used
`\bname\s*\(` (call position only) and produced **107**. That missed
every helper passed as a *value* to a higher-order fn —
`map(fns, efn_name)`, `map(ds, rewrite_alias_decl)`. Those value-position
refs are exactly the ones that open new sub-graphs (`rewrite_alias_*` +
`alias_map_*`). A naive "all lowercase idents" closure then **exploded
to 1121** (≈ the whole compiler) because local variable names collide
with fn names. The correct closure counts an ident as a callee iff it is
`ident(` **or** a bare argument to a known higher-order fn
(`map`/`filter`/`fold`/`each`/…). That saturates cleanly at **138**.
N1's "flat bundle hides upward-refs" lesson held: the bundle compiled
green with the 107-closure; only the **modular** `kaic2 main.kai`
(import-respecting) surfaced the 28 missing-mirror / privacy errors that
drove the closure to 138.

### Two renamer collision classes

- **`EVar` is overloaded.** The emit type `EVar = EV(...)` collides with
  the AST `ExprKind` constructor `EVar(String)`. A blanket `EVar -> LvmEVar`
  rewrote both, producing `kai_apply(kai_LvmEVar, …)` where an AST
  `EVar(...)` node was meant. Fix: the emit type is never *constructed*
  (it uses `EV(...)`), so any `EVar(` is the AST ctor and stays; only
  type-position `EVar` (negative lookahead `(?!\s*\()`) becomes `LvmEVar`.
  `EV` / `PR` "collisions" were false — only present in imported-module
  *comments*.
- **The renamer corrupts string literals.** `expr_kind_name`'s body has
  `EVar(_) -> "EVar"`; the `EVar` lookahead rewrote the *string content*
  to `"LvmEVar"`, silently changing what the typed-hole walker reports.
  Also `$extern_handler("c_sym")` in a comment became `"lvm_c_sym"`. Only
  2 real corruptions (1 functional, 1 comment), fixed by hand, but the
  general lesson: **a token-level renamer must skip string and comment
  spans.** Audit after every prefix-rename pass with
  `grep '"…<renamed-token>…"'`.

### Public surface: 5 fns, 0 pub types

`emit_program_llvm` (entry, driver-called) and `expr_kind_name` (the
typed-hole enclosure walker in main consults it) are mandatory pub. The
other 3 (`stdlib_head_tag_int`, `llvm_header`, `lvm_find_substring`) are
pure helpers exposed for unit testing. The 14 `Lvm*` mirror types and
the rest of the 138 mirrors stay private (no external signature). The
unused `_bmode: LvmBuildMode` param on `emit_program_llvm` was *dropped*
(both the sig and the single driver callsite) rather than make
`LvmBuildMode` pub to satisfy the "pub fn can't expose a private type in
its signature" rule — the param was already `_`-prefixed dead weight,
and dropping it removed `LvmBuildMode` entirely.

## Design decisions / alternatives considered

- **Mirror vs pre-sink (the load-bearing call).** A `compiler/emit_shared.kai`
  pre-sink for the 43 genuinely-shared helpers would avoid the
  duplication, but it reaches deep into the emit_c region (the future
  emit_c lane's territory) and would invert this clean single-module diff
  into a cross-module one. Per the N3/N4 "don't block on a future sink"
  precedent and the user's explicit choice, we mirrored. The divergence
  debt is real and named for the emit_c lane.
- **Move the 8 detached fns or leave them.** Moved — they are `llvm_`
  fns with zero emit_c callers, so they are the backend, not the C
  emitter. This does touch the emit_c region (lines 4893/6108), so the
  emit_c lane will not find them there; that is documented here so it
  doesn't hunt for them.
- **Drop `_bmode` vs pub `LvmBuildMode`.** Dropped (see above).

## Structural surprises

- The brief's whole coupling premise ("doesn't touch typer types") was
  inverted: the backend shares the *emit_c* helper layer wholesale. The
  reason the previous vertical lanes (infer/unbox/perceus) were clean is
  that they sit *above* emit in the pipeline and consume typed `[Decl]`;
  the backend sits *at* emit and shares emit_c's plumbing. Emit-layer
  extraction is a different shape from pipeline-pass extraction.
- `AliasBinding` (ctor `AB`) — a dependency of the mirrored `AliasMap` —
  turned out already-pub in infer.kai (#693 era), so it imports, no new
  mirror. The transitive *type* closure (not just the fn closure) had to
  be computed to catch `AliasMap`/`StmtsRewrite`.

## Fixtures / coverage

`stage2/tests/test_emit_llvm.kai`: **13 unit tests + 2 property checks**.
- `expr_kind_name` (EInt/EBool/ECall tags + the EVar="EVar" regression
  guard that locks in the string-corruption fix).
- `stdlib_head_tag_int` (Int=3, Bool=2, unknown=-1).
- `llvm_header` (prologue contains `%KaiValue = type opaque` + banner).
- `lvm_find_substring` (start / mid / absent-sentinel).
- `emit_program_llvm` end-to-end on a trivial fn (non-empty .ll +
  prologue present), exercising the private emit walk transitively.
- Property checks (Int generator, per the N4 String-generator caveat):
  `stdlib_head_tag_int` returns -1 for any decimal name; `llvm_header`
  is a stable constant.

Not tier-wired (consistent with every `tests/test_*.kai` — the per-module
runner is project-wide #452/#677 Phase 2). Exercised on every selfhost
regardless.

## Cost vs estimate

Estimate: ~5127 LOC moved, ~5 imports, 5–15 pub. Actual: ~4912 LOC left
main (26928 → 22004), ~7300 LOC in emit_llvm.kai (core + 8 detached + 138
mirror fns + 14 mirror types + header), 10 imports, **5 pub**. The
relocation itself was an afternoon; the closure-correctness iteration
(107 → 1121-noise → 138) and the two renamer collision classes were the
real cost. Far from the closest-to-estimate lane — the brief's coupling
model was simply wrong for an emit-layer block.

## Follow-ups for next lanes

- **emit_c lane (the big one).** It will find 138 of its helpers mirrored
  with an `lvm_` prefix in emit_llvm.kai, 43 of them genuine duplicates of
  emit_c originals. When emit_c extracts, the clean move is to **sink the
  shared 43 into a `compiler/emit_shared.kai`** and have both backends
  import it, deleting the `lvm_` copies. The 8 detached `llvm_*` fns now
  live in emit_llvm.kai, not at main:4893/6108 — do not hunt for them in
  the emit_c region. TCO (`tcrec_*`) is still resident in main and must
  extract *with* emit_c (it mints C goto strings + calls `emit_expr`).
- **Token-renamer hygiene.** Any future prefix-rename extraction must
  skip string/comment spans and audit with
  `grep '"…<renamed-token>…"'`. This lane lost time to a silently-rewritten
  string literal that selfhost happily reproduced byte-identically.
- **Closure computation must count value-position callees.** Higher-order
  args (`map(xs, fn_name)`) are callees; a call-position-only scan
  undercounts and the modular compile then rejects. The saturating
  closure (calls + HO-fn bare args) is the correct algorithm.
