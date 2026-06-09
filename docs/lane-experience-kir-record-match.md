# Lane experience — KIR record-pattern match lowering

## The gap

A record-pattern discriminated `match` lowered to a malformed KIR that the
native backend aborted on. Repro:

```kaikai
type Point = { x : Int, y : Int }
fn classify(p : Point) : Int = match p {
  { x: 0, y } -> y
  { x, y: 0 } -> x
  { x, y }    -> x + y
}
```

The `--emit=kir` dump showed the shape: an EMPTY `KSwitch` (zero cases),
only the FIRST arm surviving, and `y` a FREE register never projected. The
native backend aborted `unbound register y`; the C-direct oracle (which
emits straight from the AST, never the KIR) was correct (`5 / 3 / 6`).

Root cause: same class as list-match (PR #792) and literal-match (PR #794).
`lower_match` assumes discrimination is ONLY by variant tag. A `PRecord` is
not a variant, so `arm_tag(PRecord)` returns -1 → no switch case (empty
switch), `bind_pattern_fields` only binds `PVariant` sub-binders → record
field binds never emitted as a projection, and the literal-field
discrimination (`{ x: 0 }`) was dropped entirely.

## The lowering shape

A record pattern discriminates on TWO things at once, both dropped by the
variant path:

1. **Literal-valued fields** (`{ x: 0 }`): each is an equality TEST over
   that field — `field_read(scr, "x") == 0`. Multiple literal fields in one
   arm form a CONJUNCTION (all must match); successive arms form a fail-chain
   decision tree.
2. **Binding fields** (`{ y }` shorthand, `{ y: name }`): each must be
   PROJECTED — `KLet(name, field_read(scr, "y"))` — so the body's
   `EVar(name)` resolves. `PWild` fields are ignored.

So a record-pattern match IS structurally the literal-match decision tree,
but (a) each equality test reads a FIELD of the scrutinee rather than the
scrutinee itself, and (b) each arm binds its non-literal fields by projection
on the success path before lowering the body.

The pure half (one arm's field-test conjunction + the bind projections) lives
in a new module `kir_lower_rec.kai` (mirroring `kir_lower_lit.kai`); the
driver (`lower_record_match` + `lower_record_arms`) is a thin addition to the
Expr-SCC `kir_lower_walk.kai`, exactly as `lower_lit_match`/`lower_lit_arms`
sit there — the drivers call `lower_branch_into` (recursive into
`lower_expr`), so they cannot leave the SCC.

Generated KIR for `classify` after the fix:

```
entry:  t1 = field_borrow(p, x); t2 = eq_raw(t1, 0); condbr t2 ? L1 : L2
L1:     y = field(p, y); dup y; join <- y; br Lcont      # { x: 0, y } -> y
L2:     t3 = field_borrow(p, y); t4 = eq_raw(t3, 0); condbr t4 ? L3 : L4
L3:     x = field(p, x); dup x; join <- x; br Lcont       # { x, y: 0 } -> x
L4:     br L5                                             # catch-all { x, y }
L5:     x = field(p, x); y = field(p, y); join <- x + y; br Lcont
```

## How I read a record field — by NAME, not positional index

The brief flagged the open question: by-name field-read op vs positional
`KProj`. **By NAME** — and no new KIR node was needed.

Oracle evidence (`emit_c.kai`, the byte-id reference emitter):
- TEST path `emit_pat_test_record_fields` reads `kai_op_field_borrow(scr,
  "name")` — a NON-consuming borrow (no incref).
- BIND path `emit_pat_binds_record` reads `kai_op_field(scr, "name")` — the
  OWNING read (incref'd), so a matched arm's binding owns its ref.

Both are keyed by field NAME via `strcmp` (runtime.h `kai_op_field` /
`kai_op_field_borrow`); a record's storage is name-tagged, NOT positional. So
a `KProj`-by-index would have been wrong — the foreseen `KProjField` node was
unnecessary.

The KIR already plants `KPrim("kai_op_field", [base, KStrV(fname)])` for an
ordinary `e.field` read (`kir_lower_walk.kai` `EField` arm), and the native
backend's `nemit_field_read` already lowers it to `kaix_field(ptr rec, i8*
name)` with the field name as a global string. I reused that path:

- The TEST emits `KPrim("kai_op_field_borrow", [scrv, KStrV(name)])` (raw
  field name — `nemit_field_read` materialises it verbatim as the `i8*`
  global, so NO quotes; only string LITERAL values keep their quoted span).
- The BIND emits `KPrim("kai_op_field", [scrv, KStrV(name)])` (owned).
- The equality is the same non-consuming `kai_eq_raw` shim the literal-match
  lane uses (`kai_bool(kai_op_eq(a, b))` — consumes neither operand, so no
  field/scrutinee ref leaks across a failing test).

The one backend change: `emit_native_ops.kai` special-cased `kai_op_field` →
`kaix_field`; I generalised `nemit_field_read(ctx, sym, cargs)` to take the
runtime symbol and added the `kai_op_field_borrow` → `kaix_field_borrow` arm.
Both `kaix_field` and `kaix_field_borrow` already existed in
`runtime_llvm.c`, so no runtime change.

This is AST→KIR + one native-backend prim rename. The C-direct selfhost
byte-id path does NOT consume the KIR, so byte-id is untouched.

## Structural surprises

- **No new node, no runtime change.** The brief allowed adding a `KProjField`
  node wired through all backends if none fit. None was needed — the existing
  `kai_op_field` prim path + the already-present borrow shim covered it. The
  smaller fix is the right one.
- **Field name must be RAW in `KStrV`, not quoted.** `KStrV` nominally
  "includes quotes" (string literals carry their source span). But the native
  field-read uses the `KStrV` payload verbatim as the global-string content,
  so the field NAME must be unquoted (`KStrV("x")`, not `KStrV("\"x\"")`).
  The existing `EField` lowering confirmed this (it passes the bare `fname`).
  First draft quoted both; the KIR dump caught it before any build.
- **Conjunction of tests for a multi-literal arm.** `{ x: 0, y: 0 }` needs
  TWO chained equality tests (a fresh continue block between them); a single
  literal field needs just one `condbr` straight to the body. `rec_emit_tests`
  / `rec_emit_one_test` handle the "more literals follow?" branch to avoid an
  empty trailing block.

## Fixtures

`examples/native/record_match.kai` (mirrors `literal_match.kai`'s
exit-code-observable shape). Exercises:
- `{ x: 0, y } -> y` — literal field + binding field.
- `{ x, y: 0 } -> x` — binding field + literal field (test reads a different
  field than the first arm).
- `{ x, y } -> x + y` — all-binds catch-all.
- `{ x: 0, y: 0 } -> 99` — TWO literal fields (conjunction).
- Exit code = 5 + 3 + 6 + 99 + 3 = 116 (< 256, carried by the exit code).

Native rc == C-direct rc == 116. The native-parity harness goes 12/13 → 13/13.

## Gates run (all green)

- **Native parity** (`tools/test-native-parity.sh`): 13 passed, 0 failed
  (`record_match (rc=116)`).
- **Selfhost byte-id** (`make -C stage2 selfhost`): OK (kaic2b.c == kaic2c.c).
- **tier0** (`make tier0`): OK.
- **C↔LLVM backend parity** (`tools/test-backend-parity.sh`): green (the
  change is native-only; C/LLVM-text emitters untouched).
- **ASAN+UBSan** on the native object of the fixture (object +
  `runtime_llvm.c`, `-fsanitize=address,undefined`): 0 errors, rc=116.
- **`km score`**: `kir_lower_rec.kai` A+ (93.8), 92 LOC, cogcom avg 1.8 /
  max 3, no duplicate groups. `emit_native_ops.kai` A (92.7). `kir_lower_walk.kai`
  stayed B+ (84.2 → 83.1, project band unchanged; per-file B → B- from +18 LOC
  of driver — the established list/literal-match precedent; the new module
  carries the pure logic and is A+).

## Follow-ups for next lanes

- **Nested patterns inside a record field** (`{ x: Some(n) }`, `{ x: [h,
  ...t] }` — a sub-pattern beyond a bare `PLit`/`PBind`/`PWild`) are out of
  scope: `rec_emit_tests` only treats a `PLit` sub-pattern as a test and
  `rec_bind_fields` only projects a `PBind`. A `PVariant`/`PList` sub-pattern
  would silently be ignored (no test, no bind) — the next gap of this family.
  The C-direct `emit_pat_test_record_fields` recurses (`emit_pat_test(sub,
  ...)`); the KIR analogue would recurse a per-field decision tree. The
  in-scope fixtures don't need it.
- **Guards** (`{ x, y } if x > y -> ...`) are still dropped by every KIR match
  path (the `Arm(p, _, body)` destructure discards the `Option[Expr]` guard
  slot) — same as the variant/list/literal paths. First thing a
  guard-supporting lane touches.
- **A record arm whose body owns a borrowed field test temporary** — none
  arises here (`kai_eq_raw` + `kai_op_field_borrow` are both non-consuming, so
  the test chain leaks nothing), but worth re-checking when nested tests land.
