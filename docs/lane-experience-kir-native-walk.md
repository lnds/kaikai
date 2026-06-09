# Lane experience — KIR native walk (Lane 1.2 Parte B, Subset 1)

Closes the first vertical slice of the in-process libLLVM native backend's
**generic** KIR walk (docs/kir-design.md §7.2). Lane 1 (PR #780) emitted
only the `main -> 42` spine, validated by hand; this lane makes the walk
emit **every program of the pure subset** in behavioural parity vs the
C-direct backend (the oracle), over the three `examples/native/*.kai`
fixtures the path-gated `tier1-native` harness diffs.

## Scope as planned vs as shipped

**Planned (Opción 2 — vertical subset, widen by corpus):** start from a
program that dodges the heavy stdlib, widen construct by construct, ship
incremental green PRs. KIR all-boxed is correct (unboxing is a *later*
optimisation, not a parity requirement). The #779 EBlock-raw change is the
most dangerous — attack it last, in its own commit.

**Shipped:** the pure walk, complete. Arithmetic / let / calls
(KCall/KCallIndirect) / control flow (KBr/KCondBr/KSwitch/KTcrecGoto) /
ctors (KCon/KConReuse) / records (KRecord) / proj (KProj/KTagOf) / RC
(KDup/KDrop) / first-class-fn thunks / closures (KClosure) / lambdas
(KFnLambda). Effects (KPerform/KInstall/KPop/KSetjmp/KResume), TRMC
(KTrmcStep/KTrmcApply), explicit tail calls (KTailCall), handler clauses
(KFnClause), and reuse tokens (KDropReuse/KFreeToken) abort LOUDLY via
`nemit_unsupported` — later subsets. #779 stays deferred to its own commit.

The "premisa falsa" the strategy memo flagged held: there is no
"arithmetic-only" island — the auto-loaded core drags ~344 fns (char,
tuple, option, protocols, …) into every module, so the FIRST real fixture
already exercises records, closures, lambdas, mixed-signature ctors, and
first-class-fn thunks. The walk had to handle all of them at once; a
fixture as small as `fn main() : Unit = exit(42)` compiles the whole core.

## The load-bearing bug (and why the obvious fix was wrong)

The crash that survived from Lane 1's hand-validation into this lane: the
module **verifies** and its printed `.ll` **compiles cleanly with `llc`**,
but the in-process AArch64 AsmPrinter SEGVs in
`getKindForGlobal → isNullOrUndef(NULL)`.

Root cause (found by watching `LLVMGetValueKind` of the first declared
function flip 5→13 mid-declaration): a handle returned by a walk fn and
left in **statement position** is `kai_decref`'d by the type-blind kaic1
Perceus. A decref of an LLVM `Value*`/`Function*` writes its `->rc`,
clobbering the object's C++ SubclassID (Function 5 → ConstantVector 13).
The corrupt `Function` then fails `isa<Function>` in the AsmPrinter, falls
to the GlobalVariable path, and derefs a null initializer.

The exact repro: `nemit_declare_fns` discarded the `Function*` returned by
`llvm_get_or_declare_fn(...)`. Declaring fn #2 (`char__is_lower`) ran the
Perceus drop of fn #1's (`char__is_digit`) discarded declaration handle,
clobbering it.

**The tempting wrong fix:** print the module to text and re-parse it before
codegen — the round-trip rebuilds clean objects and the AsmPrinter is
happy. That HEALS the symptom (and produces a correct object: `llc` proves
the `.ll` is right), but it leaves the RC-clobbering bug live. Rejected per
the standing "lo correcto, no por ahora" rule: a workaround that leaves a
latent compiler bug in place is false-green for a *backend* lane.

**The real fix:** every discarded/passed-on handle is bound in a
`let _x = <handle-fn>` (the handle-binder exclusion `pcs_rhs_is_handle`),
never left as a bare statement. Sites: `nemit_declare_fns` (the repro),
`nemit_stmt` KDo/KLet/KStore, `nemit_rc` KDup/KDrop, `nemit_push_fields`.

## Structural surprises the brief did not anticipate

1. **LLVM 22 `LLVMGetNamedFunction` lazily de-syncs.** After many
   `AddFunction`s it returns NULL for a function the `GetFirst/GetNext`
   iterator still finds in the module — so the declare/fill phases each
   re-`AddFunction`ed under a `.N` suffix, leaving the real function empty.
   Fix: a linear-scan fallback in `get_or_declare_fn` (the iterator is
   authoritative). O(n²) over the corpus, acceptable for the oracle path.

2. **Cross-context constants crash the verifier.** A GEP index built with
   the global-context `LLVMInt32Type()` is a Value* of the wrong context
   for a private-context module; the verifier crashes (not errors) walking
   it. Every constant must derive its type from a context-bound handle
   (`LLVMGetTypeContext(arrtype)`). Same for `LLVMInt64Type()` in
   `build_load_arg`.

3. **`LLVMBuildGlobalStringPtr` is unserialisable here.** It inserts a GEP
   into the current block; on LLVM 22 opaque-ptr + private context the
   resulting value crashed the printer. Field names + string literals are
   now module-level `LLVMAddGlobal` + `LLVMConstStringInContext`.

4. **First-class-fn thunks are synthetic.** The KIR does not model the
   `_kai_<sym>_thunk` forwarder a `KClosure` over a named fn references —
   the C-direct emitter generates it (`emit_fn_thunk`). The native walk has
   to mirror that (`nemit_fn_thunks`): a thunk-ABI fn that unpacks
   `args[0..k]` and tail-forwards to the boxed `sym`.

5. **Symbol conventions differ from the LLVM-text backend.** The object
   links against `runtime_llvm.c`, whose `kai_prelude_*` bodies are
   `static` (invisible across compilation units); only the `kaix_prelude_*`
   shims are linkable. A builtin without a shim (`unit_name`) doesn't
   resolve — added `kaix_prelude_unit_name` (a compile-time intrinsic the
   KIR lowers as a call; `""` for unit-less values is subset-correct).
   `main` renames to `kai_main` (the runtime entry).

6. **The C-direct unbox pass `->as.i`-mangled `TyHandle`.** Mixed
   Handle+Int helper signatures (`nemit_int_lit(ctx: Handle, n: Int)`)
   raw-promote and the unbox boundary read the `ctx` handle as
   `(ctx)->as.i`, corrupting it — only on the *selfhost* C path, invisible
   to the native build. Fix in `emit_c.kai`: `unbox_boxed_scalar` /
   `box_wrap` get a `TyHandle` case (cast, never `->as.i`/`kai_int`). Plus
   a new `AReal` RArg in stage1 + `kai_llvm_const_real(void*, double)` so a
   boxed-Real param doesn't mismatch the unbox pass's `->as.r`.

## SOUNDNESS RISK #1, refined

The Parte A retro flagged that stage1 recovers handle-ness *syntactically*
(`pcs_rhs_is_handle` saw only a bare `ECall`). This lane extended it
through control flow — an `EIf` (both branches), `EMatch` (all arms), or
`EBlock` (tail) is a handle binder iff every branch is. And
`native_handle_fns()` must list EVERY walk fn returning Handle (added
`nemit_call0/1`, `nemit_declare_thunk`). **This manual list is the lane's
remaining fragility:** a new handle-returning walk fn that is NOT listed
re-opens the type-id corruption. A future lane should make stage1 recover
handle-ness from the rprelude return shape transitively, retiring the list.

## Fixtures + coverage

- `examples/native/arith_exit.kai` — user fn + Int literal + `exit` (the
  call + arithmetic + entry path). rc=42.
- `examples/native/branch_exit.kai` — `if`/condbr over two helpers. rc=13.
- `examples/native/main_returns_int.kai` — the spine (pre-existing).
- Gated by `tools/test-native-parity.sh` (path-gated `tier1-native.yml`):
  each fixture's native object is linked + run, diffed vs C-direct.

Coverage gap: the fixtures exercise the *observable* surface (exit code),
but the *corpus* (the auto-loaded core) exercises records / closures /
lambdas / thunks structurally — a fixture that *returns* a record or calls
a closure through `kai_apply` and prints it would tighten the behavioural
check. Deferred to the effects subset (which needs richer fixtures anyway).

## Gate (every item green at close)

- Native parity vs C-direct: 3/3.
- Selfhost byte-id (default C path): `kaic2b.c == kaic2c.c` — the
  TyHandle/AReal changes to the shared C path preserve determinism.
- ASAN on the linked native executables: 0 errors.
- `km`: emit_native_fn A+, emit_native_ops A, emit_native_term A+,
  emit_native A++ — all < 400 LOC, cogcom avg < 1.0 / max < 4.

## Real cost vs estimate

No hour estimate (gate by soundness, per the standing rule). The cost was
dominated by ONE bug — the handle-discard type-id corruption — which
masqueraded as four different crashes (VST de-sync, cross-context
constants, unserialisable globals, AsmPrinter SEGV) before the
`GetValueKind` watch pinned it to a single discarded statement. Each
masquerade was a real, separate bug that had to be fixed to *reach* the
next one; the round-trip detour (built, then deleted) was the cost of
distinguishing "module corrupt" from "codegen-side issue".

## Follow-ups for next lanes

- Effects subset: KPerform/KInstall/KPop/KSetjmp/KResume — evidence
  dispatch (%EvEff GEP) + cont alloca. The largest, riskiest subset.
- TRMC + KTailCall: reuse the block/tcrec machinery already here.
- KFnClause (handler clauses), KDropReuse/KFreeToken (reuse tokens).
- #779 EBlock-raw: its own commit, its own fixture (EBlock→scalar) +
  KAI_TRACE_RC + ASAN (the failure mode is RC, byte-id is false-green).
- Retire the manual `native_handle_fns()` list (see SOUNDNESS RISK #1).
- Native-default flip (Lane 1.5) + `bin/kai` wiring stay out of scope.
