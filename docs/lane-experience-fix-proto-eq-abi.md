# Lane experience — fix-proto-eq-abi

**Scope:** Fix the tier1 SIGSEGV (`test-issue-318-include`) caused by an ABI
mismatch in the runtime protocol-dispatch path: builtin unboxed scalar
protocol impls (`Eq_Bool/Int/Real/Char`, `Ord_Int/Real/Char` cmp/min/max,
`Hash_*` scalars, and the unboxed `Add/Sub/Mul/Div/Rem` arithmetic family)
were registered in the impl table and then called through a boxed
`KaiValue*(KaiValue*,KaiValue*)` cast.

## The crash chain (verified live with lldb, then by C/LLVM inspection)

1. A `test` block's `assert head([1,2,3]) == Some(1)` lowers `==` to the
   runtime operator `kai_op_eq_v`. The `assert` form drops the operand's
   static type, so the post-monomorph rewrite (`protos.kai`
   `prc_rewrite_cmp_binop`, gated on `l.ty`) to a specialised
   `Eq_Option_eq__mono__Int` never fires. (In a normal program the
   monomorphiser specialises and the bug is invisible — this is why it was
   cold until a recent nested-dispatch change started routing payloads
   through the impl table at run time.)
2. `kai_op_eq_v` → `kai_op_eq` sees an `Option` VARIANT, derives
   `head_tag = OPTION`, `kai_lookup_impl(EQ, OPTION)` → `Eq_Option_eq`
   (well-typed, boxed). The dispatch site increfs `a`/`b` and calls the
   impl, expecting it to consume both and return a `KaiValue*`.
3. `Eq_Option_eq` recurses the payload via `kai_op_eq` again. The payload
   is an `Int` → `kai_lookup_impl(EQ, INT)` → `Eq_Int_eq`.
4. The impl-table entry for INT is `&kai_protocols____pimpl_Eq_Int_eq`,
   whose **real** C signature is `static int Eq_Int_eq(int64_t, int64_t)`
   — UNBOXED. The dispatch casts it to `KaiValue*(KaiValue*,KaiValue*)`,
   reinterprets the boxed pointers as `int64`, gets back an `int` 0/1 read
   as a `KaiValue*` (`0x1`), and `kai_op_truthy(0x1)` dereferences `0x1` →
   SIGSEGV (`0x1` at -O0, `0x5` at -O2).

The four dispatch sites in `stage0/runtime.h` are correct *given a
well-typed impl*: `kai_op_eq` (VARIANT ~2936 + RECORD ~2982), `kai_op_lt`
(~3835), `kai_op_gt` (~3863). Each does `kai_incref(a); kai_incref(b)`
before the call (the impl must consume both), reads the return as a
`KaiValue*` (eq → `kai_op_truthy` + `kai_decref`; cmp → `_o_c->as.i` +
`kai_decref(_o_c)`). The bug was purely the `.fn` slot's ABI.

## Option A vs Option B

- **Option A (shipped):** emit a boxed shim
  `KaiValue* kai_<csym>__boxed(KaiValue* a, KaiValue* b)` per unboxed
  builtin impl, register the shim in the impl table, leave the raw pimpl
  reachable for the monomorphic fast path (which already works and is
  untouched).
- **Option B:** always emit builtin pimpls with a boxed signature and make
  the unboxed body an inline helper for the fast path.

Chosen **A**, confirmed by an asu design consult. A is purely *additive* in
the output (new shim defs + a changed `.fn` pointer), so it converges in
one selfhost iteration; it keeps the representation split (hot path = direct
unboxed call, cold path = boxed via table); and it touches neither the typer
nor the monomorphiser nor the runtime. B would re-box the hot path or add a
second code shape to keep both — more surface, more risk, no soundness win.

## Detection — UFnSig in situ, never demangle, never a hardcoded table

The key design call (also per asu): decide unboxed-ness from the **unbox
pass's own classification**, captured in situ. `collect_pimpl_rows` already
walks the `__pimpl_*` DFns; it now also takes the `[EFn]` registry and does
`lookup_ufn_sig(fns, name, mo)` at that point (where the original kaikai
`name` + module are in hand — no demangling the csym). `PimplRow` grew a
fifth slot `Option[UFnSig]`. `pimpl_row_is_unboxed` is true iff the sig is
`Some(US(pts, _))` with at least one scalar param (the unbox classifier only
emits `Some` when *every* param + the return is scalar, so one scalar param
implies the whole signature is unboxed).

Rejected alternatives, both of which would silently reintroduce this exact
segv the day the mangler changes or a new scalar builtin lands:
- demangling the csym back to `(proto, type)` and pattern-matching scalars;
- a hardcoded `(proto, type) -> boxed?` table — a second source of truth.

## The shim + RC

The shim unpacks each `a->as.X` / `b->as.X`, **decrefs each boxed arg**
(consume — mirrors the existing `__pimpl_*_thunk`'s `kai_decref(args[i])`
and the dispatch contract: caller increfs, impl owns + drops), calls the
raw pimpl, and boxes the return via `box_wrap(rt)`: `kai_bool` for eq,
`kai_int` for cmp, the value-type ctor for min/max. `kai_bool`/`kai_int`
return cache-immortalised cells, so the caller's `kai_decref` of the return
is a safe no-op.

RC verified with `KAI_TRACE_RC` on the regression fixture and a 1-vs-4
dispatch scaling test: the shim path leaks **fewer** allocs than the
monomorphic path (2 vs 3 for one comparison), and the residual leak is the
pre-existing codegen leak from building `head([...])`/`Some(...)` (present
identically in a `is_some(head(...))` control with no `==`). No double-free
(ASAN clean, exit 0), no leak attributable to the shim.

## emit_llvm needed the same fix

Yes. `llvm_emit_impl_register_calls` registered
`bitcast (%KaiValue*(%KaiValue*,...)* @kai_<csym> to i8*)`, but the raw
pimpl is defined as e.g. `i1 @kai_..._eq(i64, i64)` — the same ABI mismatch,
and clang's verifier is stricter than C about the bitcast type vs the
`define`. The mirror shim `@kai_<csym>__boxed` reuses the existing LLVM
unbox/box infra (`llvm_thunk_unbox_args`, `llvm_raw_ir_type_t`,
`llvm_box_ctor_t`) — it is the UFn thunk minus the `(self, args[], n)`
shape, plus a `(%KaiValue* %a0, %KaiValue* %a1)` param list. Registering the
shim makes the table bitcast identity-typed (accepted by the verifier).
Verified structurally (the `.ll` carries the well-formed shim and the
identity bitcast, compiles+links+runs) and via byte-identical `selfhost-llvm`.

## Fixtures

- `examples/protocols/poly_scalar_dispatch.kai` + `.out.expected`: `test`
  blocks whose `assert ... == ...` drive the crashing `kai_op_eq_v` path
  over Option[Int/Bool/Real/Char], nested Option[List[Int]], and List
  payloads. Run via the new C-only `test-proto-scalar-dispatch` make target
  (the `--test` runner is C-only). Confirmed it SIGSEGVs (139) on a pre-fix
  `kaic2` and passes (4/4, exit 0) after the fix — a genuine regression, not
  a tautology.

**Coverage gap (honest):** the LLVM mirror shim has no *program* fixture
that reproduces the crash, because the only surface that produces
`kai_op_eq_v` over a scalar Option payload is `assert` inside a `test`
block, and the `--test` runner emits C only. Every non-`assert`
comparison the monomorphiser specialises away (verified: generic
`fn f[T](a,b)=a==b` over `Some(1)` still gets `__mono__Int`, no crash). The
LLVM shim is covered structurally (well-formed `.ll` + identity bitcast) and
by `selfhost-llvm` (the compiler dispatches scalar Eq/Ord through the impl
table when self-compiling). A future lane that adds an LLVM `--test` runner
would let this fixture run dual-backend.

## Gates (all green)

- `make test-issue-318-include` — **OK** (was SIGSEGV 139; central gate).
- `make test-proto-scalar-dispatch` — **OK (C)**.
- `make selfhost` — fixed-point (`kaic2b.c == kaic2c.c`).
- `make selfhost-llvm` — byte-identical (`s1.ll == s2.ll`).
- ASAN on the fixture — exit 0, **0 errors, 0 leaks** added.
- `KAI_TRACE_RC` — shim path leaks fewer than the mono path; residual leak
  is pre-existing codegen, not the shim.
- `make tier0`, `make tier1` — green.

## Real cost vs. surprises

- The crash chain itself was handed over verified — the work was the
  *codegen* surgery, not the diagnosis.
- **Biggest time sink (process, not code):** the first ~2 hours of edits
  landed in the MAIN checkout, not the worktree — every `Read`/`Edit`/`Write`
  used `/.../kaikai/...` paths without the `.fix-proto-eq-abi` suffix, while
  Bash `cd stage2` ran in the worktree. This produced phantom build
  staleness (kaic2 compiled worktree sources that lacked the edits) and a
  confusing "passes then crashes" oscillation. Recovered cleanly via
  `git diff > patch` in main → `git apply --3way` in the worktree → revert
  the three files in main (leaving unrelated untracked files there
  untouched). The `[[feedback_kaikai_edit_lands_in_main_not_worktree]]`
  memo predicted exactly this; verify the worktree prefix on every path.
- **Fixture reproduction was non-obvious:** literal `Some(1) == Some(1)` in
  `main` does NOT crash — it monomorphises. Only `assert` inside a `test`
  block drops the type annotation and routes through `kai_op_eq_v`. Finding
  this required diffing the crashing stdlib test's `.c` (`kai_op_eq_v`)
  against my aislated fixture's `.c` (`Eq_Option_eq__mono__Int`).
- The `min/max` table entries are registered but never dispatched by
  pointer today (only `cmp` via lt/gt and `eq`). Shimmed anyway, so the
  table is uniformly type-correct and not a latent bomb if min/max dispatch
  is added later.

## Follow-ups

- LLVM `--test` runner would let `poly_scalar_dispatch.kai` run dual-backend
  and close the LLVM program-fixture gap noted above.
- `Some(1) < Some(5)` returns false (Ord-for-Option semantics) — dropped
  the Ord arm from the fixture rather than encode a dubious golden. Out of
  this lane's scope; worth a separate look at whether Option's derived Ord
  is what users expect.
