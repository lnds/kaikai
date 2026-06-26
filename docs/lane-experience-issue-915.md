# Lane experience — issue #915: constant-index record field reads

Goal: replace the `kai_op_field(rec, "name")` strcmp loop that every
record field read ran with a constant-index load, the largest recoverable
slice of the self-compile runtime-prims profile (`op_eq` / `strcmp` /
`op_field` linear name lookups, 22.7% self-time per #911). AST nodes are
records, so `node.kind` / `.ty` / `.line` run this per node.

Outcome: shipped, on BOTH backends, with one shared index oracle.
Throughput +3.5% on the C-direct self-compile. The fix was twice as large
as the brief framed it, because the index is unsound without first fixing a
layout defect the brief did not anticipate.

## The blocker the brief missed: source-order layout

The brief assumed the field index was "trivially derivable" from
`RecInfo`'s declared field order. It is not. Named record literals were
stored in SOURCE order, not declaration order: empirically, for the same
type `P = { x, y }`,

- `P { x: 1, y: 2 }` → `kai_record(2, {1,2}, {"x","y"})` (x at slot 0)
- `P { y: 2, x: 1 }` → `kai_record(2, {2,1}, {"y","x"})` (x at slot 1)

So a field's physical slot was a property of the *producing literal*, not
of the type — no global (type, field) → index map was sound. Positional
(`P{1,2}`) and spread (`P{...s, x:9}`) literals were already normalised to
declaration order in the desugar; the named form was the one un-normalised
case.

Two latent defects fell out of the same source-order layout, both fixed as
a side effect of canonicalising:

- **Structural equality** (`kai_op_eq` KAI_RECORD arm) compares fields
  positionally by slot, so `P{x:1,y:2} != P{y:2,x:1}` held today —
  logically-equal records comparing unequal. Now `==`.
- **Reuse-in-place** (`kai_reuse_or_alloc_record`) overwrites `fields[i]`
  AND `names[i]` positionally. A rebuild written in a different field
  order than its donor would have written values to the wrong slots — a
  silent corruption latent in the source-order regime.

## How the index is computed and the layout canonicalised

One shared oracle in `ast.kai`: `rec_field_index(recs, type, arity, field)
→ Option[Int]` (declaration ordinal via `rec_find_with_field` +
`field_decl_index`), and `field_index_of(recs, opt_ty, field)` wrapping it
for the `Option[Ty]` at a read site. Every site — both backends' field
read AND the layout decision — indexes through this one oracle, so they
cannot drift.

**Canonicalisation lives in the desugar**, the layer before the
C-oracle / KIR fork. This was forced: the C backend burns the typed AST
straight to C (it never sees KIR), so a KIR-only reorder would fix only
native and break native-vs-C parity. A named `ERecordLit` whose fields are
already in declaration order passes through unchanged (the overwhelming
common case — zero AST growth). An out-of-order one becomes a let-block
that binds each field value to a temp IN SOURCE ORDER, then assembles the
record in DECLARATION order. This is the same eval-order-vs-assembly-order
shape the spread desugar already used; the lane finished the third form.

**Eval order is load-bearing and was preserved.** An empirical probe on
main showed `P { y: side("y"), x: side("x") }` prints `y` then `x` —
source order, observable via effectful inits. A pure list-reorder would
have changed that (a Tier-1 stability violation); the let-bind keeps it.

With the layout canonical, an `EField` whose `base.ty` resolves to a known
record lowers to `kai_op_field_at(rec, N)` — a constant-index load with
the IDENTICAL incref-on-read RC contract — in both `emit_c.kai` and the
KIR `EField` lowering. Unknown / polymorphic / non-record base types keep
the `kai_op_field` strcmp fallback. Runtime helper `kai_op_field_at` added
to both runtime.h copies + the `kaix_field_at` native shim (mirrors
`kaix_tag_eq`'s `(ptr, i32)` shape).

## Scope decision: match-bind reads stay on strcmp

Match-bind record-field reads (`kir_lower_bind.kai`) also go through
`kai_op_field`, but the read site there has only the pattern's field
names, not the scrutinee's record type — threading the type down is more
invasive. Left on strcmp: they remain CORRECT against the now-canonical
layout (strcmp finds the name wherever it is, which is always declaration
order), they just do not get the perf win. This is sound for the lane
because correctness depends on construction-side canonicalisation being
universal — which it is — not on the read strategy. Separable follow-up.

**One invariant the follow-up inherits:** any future record-construction
path that bypasses `lower_record_lit` / `lower_reuse_record` / the desugar
must also assemble in declaration order, or it breaks both read strategies.

## RC preservation — the trap, and the verification

`kai_op_field_at` is `kai_incref(rec->as.rec.fields[i])` — byte-identical
ownership to `kai_op_field`; only the slot lookup changed, not whether the
field is increfed or how the base is dropped. Verified: a field-heavy
1000-iteration loop under `KAI_TRACE_RC` shows incref/decref totals
IDENTICAL between this branch and clean main on both backends. The C
backend is fully RC-clean on it (`live_peak=7`, bounded). The native
backend shows the same pre-existing temp-record residual WITH OR WITHOUT
this change (3000 incref / 1000 decref on both), confirming the change is
RC-neutral; that residual is a separate native matter, not this lane.

## Gates

- selfhost byte-id deterministic (`kaic2b.c == kaic2c.c`) on both backends.
- C-backend ASAN clean on the canonical + reversed-order-reuse fixtures.
- Native + C agree on every fixture (canonical, reversed reuse, existing
  `reuse_record_basic`).
- Serial native-vs-C parity (`BACKEND_PARITY_JOBS=1`).
- Fixtures: `examples/records/field_index_canonical.kai` (out-of-order
  literal + reversed-order rebuild + equality flip) and
  `examples/perceus/reuse_record_reversed_order.kai` (the field-order-
  reversed reuse rebuild — the case that would false-green if reuse wrote
  source-order slots).

## Throughput (C-direct, best-of-5, self-compile of `main.kai`, ~93.5k LOC)

Both binaries are the self-compiled `kaic2b` generation, so the
compiler's own field reads reflect each branch's emit.

| | main | #915 |
|---|---|---|
| best | 39.43s | 38.08s |
| LOC/s | 2,370 | 2,454 (+3.5%) |
| own field reads | 3,121 strcmp | 3,123 indexed |

Modest because `op_field` is one strand of the shared 22.7% runtime-prims
bucket (the `op_eq` / other strcmp strands are untouched). The win is the
field-read strand specifically, eliminated outright.

## Cost vs framing

The brief read as a one-file KIR tweak ("lower EField to a constant-index
load") and pointed at `emit_c.kai:6094`. Reality: the field read had moved
through KIR for native, the C oracle stayed AST-direct, the index was
unsound against the live layout, and the fix's centre of gravity was a
desugar canonicalisation touching record equality semantics. The brief's
"the layout exists, the field's offset is a compile-time constant" was the
exact assumption that did not hold. Two architect consults pinned the
forced moves: canonicalise before the fork (not in KIR), and preserve eval
order with the let-bind (don't pure-reorder).

## Follow-ups

- Migrate match-bind record-field reads to `kai_op_field_at` (thread the
  scrutinee record type into `bind_record_subfields`).
- The native temp-record RC residual surfaced by the field-heavy probe is
  pre-existing and unrelated; not opened here.
