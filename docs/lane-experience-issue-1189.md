# Lane experience — issue #1189: public `Composition` theory + `Layout` kind

## Scope as planned vs. as shipped

**Planned (issue + brief):** a public `Composition` theory (the sixth kind's
algebra), a `Layout` kind over it for declarative binary representation
(`U32<be>`), `encode`/`decode` with real codegen, TLV backward-dependency, nested
Layout records, and a `kai info kinds` page. Waterfall as a doc example only.

**Shipped:**

- `theory Composition = { assoc, measure }` — public, user-declarable
  (`kind Frame : Composition with frame` compiles). Fixture-gated.
- `kind Layout : Composition with layout`, habitants `be`/`le` seeded like a base
  kind (resolve without import). `U32<be>` ≠ `U32<le>` (identity). Formation guard
  rejects `be^2`/`be*le`.
- `encode`/`decode` **executing byte-exact on both backends** (C + native,
  byte-identical), over fixed-width records. `decode` returns `Option[T]` (None on a
  short buffer). Non-Layout records (plain fields, nested records) are rejected
  cleanly at the typer.
- `docs/info/kinds.md` (new topic, auto-registered, code blocks compile under
  test-info-blocks). Catalog docs updated. Waterfall as a `text` example.

**Deferred (declared follow-up, rejected not mis-encoded):** TLV (`Bytes<len>`),
nested Layout records, signed integer heads, sub-byte fields.

## How the Module pattern was traced (and where it diverged)

The brief said "replicate Module: theory decl + engine arm + collector." That was
right for the **theory + kind + collector**, but the engine claim was wrong in two
ways the code made obvious:

1. **No new unifier.** The brief (and the design doc's `TyComp` sketch) imagined a
   sum-a-measure unification engine (~150-200 LOC). But `unify_dim` already delegates
   everything to `unify_abelian`, and Module/Region impose their semantics as a
   *formation guard* (`utable_module_violation`), not a separate unifier. For what
   `Layout` exercises — `be`/`le` incompatible by identity — the abelian solver on
   well-formed inputs already *is* symbol equality. So Composition needed **zero new
   engine**: just its kind names added to the atomic-kind list the existing guard
   reads. The "sum-a-measure" is Waterfall's need (not shipped), and for Layout the
   size sum lives in codegen, not unification. This was the single biggest scope
   reduction — the engine the brief budgeted for does not exist because it was not
   needed.

2. **The `[l: Layout]` signature is pseudocode.** The issue writes
   `decode[l: Layout](bytes) : Option[l]`, but `[x: Kind]` binds a *habitant*
   (a unit-var that goes in `<x>` and is rewritten to `Kind$sym`), not a type usable
   in `Option[l]`. `Layout` classifies habitants (`be`/`le`), not records; `Header`
   is a plain `Type` whose *fields* carry habitants. Forcing a kind bound over a type
   is the constraint-propagation Tier 1 #3 forbids. Shipped signature: `encode[t]` /
   `decode[t]` over ordinary `[t: Type]`, with the emitter checking the concrete
   target is Layout-bearing — the exact shape `unit_name` uses.

## Structural surprises

- **The guard's annotation path needed the base kinds too.** `st_check_module_unit`
  (operator path) reads `s.module_kinds` seeded with `base_module_kind_names()`, but
  `validate_module_unit_shape` (written-annotation path) reads `scope_module_kinds`,
  which only had decl-stream kinds. Layout's habitants ship in the catalog (not the
  user stream), so the annotation guard silently did nothing until `build_kind_ctx`
  seeded `kc_modules` with the base atomic kinds. Two guard entry points, one seeded,
  one not — a real trap.

- **Habitant qualification bit the layout detector.** `layout_synth` runs pre-kind-
  resolution (sees `USym("be")`), but `layout_rewrite` runs post-resolution (sees
  `USym("Layout$be")`). The field detector matched `"be"` exactly and so failed to
  recognise `Header` as Layout-bearing at rewrite time, rejecting a *valid* call.
  Fix: strip the qualifier with `kind_sym_bare` before comparing.

- **Codegen parity came free — via generated kaikai, not a per-backend engine.** The
  cleanest mechanism turned out to be: synthesise `__layout_encode_T` /
  `__layout_decode_T` as ordinary kaikai (a fold of `bin.put_uint_*` /
  `bin.get_uint_*`), inject them pre-typer (same slot as refinement predicates), and
  redirect calls post-inference. Both backends lower the generated fns like any user
  fn, so C and native are byte-identical *by construction* — no risk of two hand-
  written engines diverging. The `bin_*` bricks were verified to run identically on
  both backends before the engine was built.

- **`__detach_unit` / `__attach_unit` bridge the brand.** A field `magic: U32<be>` is
  `Int<be>`; the `bin_*` bricks want plain `Int`. `encode` wraps each field read in
  `__detach_unit` (erase the brand, identity at runtime); `decode` wraps each read
  value in `__attach_unit` (re-apply it) so the plain `Int` fits the branded field.

- **Self-compile caught the missing driver imports.** The bundle build (one
  concatenated TU) linked fine, but the modular self-compile failed with
  `cannot find layout_type_names` — the driver needs explicit
  `import compiler.layout_synth` / `layout_rewrite`. A bundle-only test would have
  missed this; tier0's stage2 selfhost caught it.

- **The bare `encode`/`decode` builtin collides with a module's same-named fn.**
  `encode`/`decode` are global builtins, so `import encoding.toml` brings a
  `decode`/`encode` into scope that the resolver leaves as `EVar("decode")` —
  indistinguishable by name from the Layout builtin. The rewrite must key on the
  *resolved target type*, not the name: redirect only when the concrete target is
  a synthesised Layout record (`list_has(layout_types, t)`); leave every other
  `encode`/`decode` untouched. Keying on the name rejected `toml.decode` as
  "not Layout-bearing" and broke the modular self-compile. This also deleted the
  whole bad-call reporting path (`BadLayoutCall`, `report_bad_layout_calls`): a
  non-Layout `encode`/`decode` is simply not ours to reject.

- **A con-name literal in a list-pattern arm is outside the native subset.**
  `TyCon(_, "Option", [inner])` — testing the con name AND binding the arg list in
  the *same* arm — is a "mixed variant slot" the native backend cannot lower
  (`list-in-multi-slot`, `interleaved test+bind`). The native-safe form is a
  wildcard con name plus a head+spread on the arg list: `TyCon(_, _, [a, ...])`,
  the exact shape `pipe_elem_ty` uses. `decode` always resolves to `Option[T]`, so
  the first type-arg is `T` — the con name never needs testing. (A `[h, ...rest]`
  helper factored out of the arm miscompiled the bundle a different way; the
  inline `[a, ...]` in the arm is the one that lowers on both backends.)

- **The bundle-single-TU miscompile bit twice more, both from short helper names.**
  A `fn lr_first_ty` helper — perfectly exhaustive — made the *self-compiled*
  compiler panic `non-exhaustive match` on any program that hit the layout rewrite.
  Inlining its body into the arm fixed it. The recurring lesson: in the single-TU
  bundle, prefer inlining a two-line pattern over a named helper when the helper
  name is short and generic.

- **The big time-sink: two bundle-namespacing miscompiles that broke an unrelated
  feature.** The stage2 bundle is ONE translation unit with no inter-module
  namespacing, so a name in a new module can collide globally and *silently
  miscompile a feature 100k lines away*. Two collisions in the new module broke
  refinement-type narrowing (`p : Small` → "requires a union scrutinee"), which the
  new code never touches:
  1. **`type LayoutField = LF(...)`** (arity 3) collided with `LintFinding.LF`
     (arity 4). `ctor_add` prepends without dedup or diagnostic, poisoning the
     global constructor table. Fixed: `LF` → `LFld`.
  2. **`fn tyname(...)`** collided with the *parameter* `tyname` in
     `layout_encode_fn_name(tyname: String)` — a fn-vs-binder shadow. Fixed:
     `fn tyname` → `mk_tyname`.
  Neither is caught by any diagnostic; the symptom is a distant, unrelated test
  failing. Debugging took a content-bisection of the module (`head -N` + stubs,
  rebuild, check the distant test) — NOT a bundle bisection. The lesson for future
  compiler-module lanes: **prefix every short helper name in a new bundle module,
  and never reuse a bare AST-ish constructor name.** This is worth a compiler issue
  (`ctor_add` should reject a duplicate constructor with conflicting arity). It also
  forced a detour: the module was briefly merged into one file (C-grade) to rule out
  a "two new modules" theory, then re-split once the real cause was the names.

## Fixtures added

- `examples/sugars/kinds_composition_user_kind.kai` (+`.out.expected`) — public
  theory, user-declarable.
- `examples/sugars/kinds_layout_fields.kai` (+`.out.expected`) — `U32<be>` record.
- `examples/sugars/kinds_layout_endian_mismatch_err.kai` (+`.err.expected`) —
  `U32<be>` ≠ `U32<le>` (test 1).
- `examples/sugars/kinds_composition_guard_err.kai` (+`.err.expected`) — `be^2`
  rejected (formation guard).
- `examples/sugars/kinds_layout_encode_decode.kai` (+`.out.expected`) — the
  byte-exact roundtrip + short-input-None (test 3), run through the C backend by the
  sugars harness.
- `docs/info/kinds.md` code blocks (positive + `kaikai-neg`) under test-info-blocks.

## Coverage gaps / follow-ups

- TLV / nested / signed heads / sub-byte are rejected, not supported — a follow-up
  issue should carry the design (already in `docs/layout-kind-design.md`).
- Native byte-exactness is asserted by the same fixture that runs on C; a dedicated
  native `.out.expected` is not separate (the sugars harness is C-only), but the
  roundtrip was verified on native manually and both print identical bytes.
- The `layout_rewrite` child walk hand-mirrors `dsg_map_expr_kind`'s composite arms;
  if a new `ExprKind` composite is added, both must be updated. Noted at the walk site.

- **The native backend SIGSEGVs when the *self-hosted native* compiler runs the
  layout rewrite over a program that carries a Layout type.** Root cause is in the
  native lowering, not the feature: the identical walker, compiled by the C
  backend, runs flawlessly (all fixtures + decode byte-exact on both backends); the
  native-built `kaic2-native` null-derefs (`EXC_BAD_ACCESS at 0x10`, offset-16 read
  of a NULL node) while emitting C for any `.kai` with a non-empty `layout_types`.
  The `rewrite_layout_calls` early-return (`layout_types == []` → `decls` untouched)
  is correct on its own and covers every layout-free program — so the self-host
  gate (whose sample has no layout) is green, byte-identical to the oracle. The
  residual hole is the intersection *self-host × native × layout-bearing input*.
  Repro is deterministic: build `kaic2-native` (the gate's link path), then
  `kaic2-native --emit=c examples/sugars/kinds_layout_encode_decode.kai` → SIGSEGV;
  the same file through the C-backed `kaic2` is byte-exact. The flat native stack
  (`_kai_lam_28`, no frame pointers) and the offset-16 NULL read match the
  raw-binder-into-boxed-consumer pattern the native boxing border has hit before.
  This is a native-backend follow-up (issue #1201), not a feature defect; a fixture
  that runs the above repro under CI is the gate the follow-up lane should land with.

## Cost vs. estimate

The engine the brief budgeted (~150-200 LOC unifier) was **zero** — the biggest
saving. The cost moved entirely to the codegen half the brief under-weighted:
`layout_synth` (254 LOC, A) synthesises the fns, `layout_rewrite` (143 LOC, A+)
redirects, plus the `bin_*` width/endian bricks in `protocols.kai`. The
"generated kaikai, not per-backend codegen" decision is what kept native parity from
doubling the work.

`layout_rewrite` shrank from 312 to 143 LOC once the reject/report path went away:
keying the rewrite on the resolved target type (redirect iff it is a Layout record)
made a non-Layout `encode`/`decode` a non-event, so the whole `BadLayoutCall`
threading — a tuple `(payload, bad-list)` woven through every walk helper — was dead
weight. Removing it took the file from B- back to A+ and the walk from a
result-variant maze to a plain structural map.
