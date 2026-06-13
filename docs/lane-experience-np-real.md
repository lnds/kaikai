# Lane experience — native-parity Real box/unbox (np-real)

**Scope:** close the "Real box/unbox" family of the native-parity baseline
(KIR Lane 1.5): the `9.88131e-324` output-mismatch on Real-heavy fixtures
under `--backend=native`. Branch `np-real`, off `main` at 576cd3fb.

## Scope as planned vs as shipped

**Planned:** the brief framed three sub-causes (complex/unbox_* fixtures)
as "a Real (`SReal`) slot reaching an op raw where boxed is expected, or
vice-versa" — i.e. a local slot-discipline bug, same class as the
burn-down-2 box discipline on the Int slot.

**Shipped:** the three sub-causes turned out to be ONE root cause, and it
was NOT a slot-shape bug. Diagnosis (verified against the C-direct oracle
and `KAI_TRACE_RC`):

- The KIR was deliberately **all-boxed** (`slot_of_ty` → always `SBoxed`,
  "§4.6 later milestone") and IGNORED the unbox pass's `.mode` field.
- The unbox pass (`unbox.kai`) marks a Real node `MUnboxed` when it is a
  raw C scalar; the C-direct oracle emits it as a raw `double kair_x` with
  native arithmetic. Perceus, seeing a raw binding, SKIPS its RC
  (`prc_pat_bindings_skip_raw`): a raw `double` has no refcount.
- The KIR re-boxed every Real and lowered `a * 7.0` to the CONSUMING
  `kaix_mul` (decref both operands) — but it inherited perceus's RC-plan,
  which OMITTED the dup/drop of the unbox-promoted-raw vars. So a multi-use
  raw Real (`a` read 3×, the `rhs` param of `complex.mul` read 2×) was
  double-freed: the slab was reused by the next `kaix_real` and read back
  as a `double` → `9.88131e-324`.

The four listed fixtures + the `^`-on-Real fixture `math_real_basic` (5
total) all share this root; closing it closed all five.

## Design decision and alternatives

Consulted `asu` (decision saved in `project-kaikai-kir-unbox-mode-slave`).
Three paths:

- **A — KIR mode-slave** (CHOSEN): the KIR honors `.mode` exactly like the
  C emitter. A `MUnboxed` Real lowers to a raw `f64` register + native
  `fadd`/`fmul`/`fcmp`, boxing only at the raw↔boxed border. Raw values
  never enter RC, so perceus's RC-plan (which assumed raw) fits by
  construction.
- **B — re-box + re-derive RC** (REJECTED): keep all-boxed, re-insert the
  dup/drop perceus omitted. The load-bearing reframe: B is NOT smaller than
  A — it is A inverted and worse. To re-derive the missing RC, B must read
  `.mode` anyway; once you read `.mode` you're at A's fork, and the only
  difference is B builds a SECOND RC counter that must match perceus
  forever. That second source of truth desyncs silently the moment
  perceus's last-use analysis changes (live code) — a bug-class, the same
  "two live paths" anti-pattern as the two-runtimes rule.
- **C — run unbox post-fork / pre-unbox AST for KIR** (REJECTED): unbox
  INFORMS perceus (perceus reads `.mode` to skip raw RC). Running unbox
  post-fork loses the raw knowledge in the KIR branch; a pre-unbox AST
  makes the KIR re-implement unbox = a third source of truth.

A's real trade-off is NOT RC (that disappears) — it is EDGE COVERAGE: every
raw↔boxed transition you miss is a new mismatch. But the edges are a CLOSED
set the C-direct oracle already solves; you port the map, not explore it.

## Structural surprises the brief did not anticipate

1. **The brief's framing was wrong.** "A slot reaches an op raw where boxed
   is expected" describes a symptom, not the cause. The cause is the KIR
   ignoring `.mode` + inheriting perceus's raw-aware RC-plan. The
   `nslot_type_of_tag` SReal→i64t bug (Paso 0, fixed) was real but
   secondary — inert until SReal was actually emitted.

2. **Bool is in scope, but only as a Real comparison result.** A Real-heavy
   body has `a < b` (raw `fcmp` → raw i1) and `big and neq` (raw i1
   logical). Extending naively to "any `MUnboxed` Bool" was WRONG: a UFn
   call returning Bool (`char_is_lower(c)`) is also `MUnboxed`, but it
   returns a BOXED Bool in the KIR — treating it raw emitted
   `fcmp ptr, double` garbage. The gate had to be the OPERAND type for a
   comparison/logical, never the result type (`kir_kind_is_raw` inspects
   children). And the raw path gates on the FORM (literal / var / arith /
   cmp / logical / neg), not just the type: a Real-typed `if` / call / field
   stays boxed (lowering it raw + unbox→rebox at the border just adds a
   redundant round-trip — initially shipped, then removed).

3. **The box↔raw border decision lives in the EMITTER, not the lowering.**
   The lowering plants `KVar(nm)` uniformly; only the emitter knows a
   register's slot. A raw load of a boxed param unbox-borrows
   (`kaix_real_field`, no decref); a boxed use of a raw binder boxes on read
   (`kaix_real`). Deciding this in `nemit_atom_raw` /
   `nemit_load_reg_boxed` (where `native_ctx_reg_slot` is available) avoided
   threading a raw-name set through `LowerSt` — which would have touched the
   9 `LowerSt` reconstructions that the 4 parallel kir lanes also edit.

4. **stage1 carries its own `rprelude_table`.** New native prims must be
   added in BOTH `stage1/compiler.kai` (the `AHandle`/`RHandle` form kaic1
   resolves the bundle against) and `stage2/compiler/native_prims.kai` — and
   stage1 changes force a full kaic0→kaic1→kaic2 rebuild.

## Fixtures added and coverage

- `examples/kir/raw_real.kai` + `.kir.fns` + `.kir.expected` (test-kir
  golden): the minimal match→scalar fixture asu's note demands — a multi-use
  raw Real + a raw comparison + the box border. byte-id is FALSE-GREEN
  without it (the compiler's own matches never promote a Real to raw, so
  selfhost never exercises the path).
- The 5 closed fixtures are themselves the regression fixtures (in the
  ratchet). The baseline drops 67 → 62.

## Cost vs estimate

Larger than the brief implied (a slot fix). The real shape: a new lowering
module (`kir_lower_raw.kai`, A++), a raw-op dispatch in the emitter, 6 new
LLVM-builder prims (`fbinop`/`fcmp`/`fneg`/`logical`/`lnot`/`zext`) + 2 box
borrow shims (`kaix_real_field`/`kaix_bool_field`), wired in two runtimes
and two prelude tables. All additive: only `MUnboxed` Real nodes take the
raw path; every boxed family (Int / Bool-from-calls / records) is untouched
— zero parity regressions across 515 fixtures.

## Follow-ups left for next lanes

- **RC leak on `complex_*` native (NOT this lane).** `KAI_TRACE_RC` shows
  the native path leaks more than C-direct on record-heavy fixtures: the
  `kai_op_field` box is borrowed (`real.unbox`) but not dropped — the same
  leak the C oracle has (its `(op_field)->as.r` leaks too), PLUS the
  pre-existing native arg-pass leak (#817). Output is byte-id and ASAN is
  clean (no UAF); the leak is the #817 class, a separate family. On
  arithmetic-only fixtures (`cond_real`) the raw path actually leaks LESS
  than C (no boxed intermediates).
- **Int raw is still boxed.** Scope was Real only (boxed Int rides the
  tagged-immediate cache, immortal, no double-free). An Int-raw lane would
  reuse `kir_lower_raw` + the `ibinop` analogue.
- **The redundant `real.box`→`real_to_string` on every raw use.** Each boxed
  use of a raw register mints a fresh `kaix_real`; LLVM -O folds the
  obvious cases, but an unbox-pass-aware KIR could thread rawness into the
  call ABI (the oracle's UFn raw calling convention) to skip the box.
