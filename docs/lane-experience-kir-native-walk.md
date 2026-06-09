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

---

# Lane experience — KIR native walk, effects subset (2a)

Closes the second vertical slice: algebraic effects in the in-process
libLLVM native walk. Covers `KInstall` / `KSetjmp` / `KPop` / `KPerform` /
`KResume` / `KFnClause` for **one-shot user handlers, no state, no
`as`-alias, no builtin defaults** (those are subset 2b). Two new fixtures,
both in native parity vs C-direct: `effect_handle.kai` (resume-tail, exit
42) and `effect_discard.kai` (a `Cancel.raise()` clause that discards
`resume`, longjmping out of the handle, exit 7).

## Scope as planned vs as shipped

**Planned:** the design memo (asu consult) resolved the one load-bearing
question — how to emit `setjmp` in libLLVM without mem2reg corrupting a
local that crosses a longjmp — as **Camino 3: do not run mem2reg on a fn
containing a setjmp.** It is sound by construction (no SSA promotion → the
post-longjmp load reads fresh memory, the LLVM analogue of C99 §7.13.2.1
`volatile`), and it is *already* the default: the native backend runs no
mem2reg today, so the effects subset is sound from the start with zero
pipeline work. The remaining work was a mechanical port of the validated
`emit_llvm.kai` (the abandoned LLVM-text backend) effect emission to C-API
calls — the same string-`.ll`→`kai_llvm_build_*` translation the pure walk
used. The runtime effect contract (`kaix_evidence_*`, `kaix_cont_*`,
`kai_jmpbuf_size`, the discard-unwind triple) already shipped in
`runtime_llvm.c` (refs #622); only the `setjmp` itself the backend emits.

**Shipped:** exactly that, plus three runtime helpers the C-API path needed
(`kaix_ev_op_at`/`kaix_ev_set_op`/`kaix_ev_handler_id` for Ev-struct field
access without a struct-nominal type in IR; `kaix_op_finish` folding the
discard test + unwind into one straight-line call so a `KPerform` stays one
KIR block = one LLVM block; `kaix_bool_of_i32`), and the
`kai_llvm_build_array_alloca` C-API forwarder for the runtime-sized
`jmp_buf`. Two new modules, `emit_native_fx.kai` (pass-1 alloca reservation
+ handler lookup, A++) and `emit_native_fx2.kai` (pass-2 emitters, A+).

## The discard fixture earns its keep (asu's warning, vindicated)

The design memo flagged that a resume-tail-only fixture would **false-green
the very hazard the setjmp pad guards** — the mem2reg-corrupts-a-live-local
case only manifests when the longjmp actually lands and a body-path local is
read after. So the discard fixture (`effect_discard.kai`, exit 7) was
written *first*, validated under the C-direct oracle before any emitter
code. It is the only fixture that exercises the longjmp landing; without it
the whole subset would have shipped green on the resume path alone.

## Five bugs, five distinct failure modes

1. **SOUNDNESS RISK #1 — the load-bearing one.** Every new walk fn that
   returns a `Handle` (an LLVM `Value*`/`Function*`) must be listed in
   stage 1's `native_handle_fns()`, or the type-blind kaic1 Perceus
   `kai_decref`s its result, clobbering the LLVM object's C++ type-id. Ten
   new fns (led by `nemit_declare_for_abi`, whose discarded `Function*` is
   the fn being defined) were unlisted. Symptom: **344 fns processed, 0
   bodies emitted** — the module *verified clean and linked* (a body-less
   declaration is valid IR), then segfaulted/link-failed on the missing
   bodies. The diagnostic that cracked it: `nm` showed every user fn `U`
   (undefined), only thunks `T`. This is precisely the manual-list
   fragility the subset-1 retro flagged as the lane's remaining risk; it
   bit on the very next lane, exactly as predicted. The follow-up to
   retire the list is now load-bearing, not nice-to-have.

2. **setjmp boxed as Int → segfault.** The `KSetjmp` i32 result, boxed via
   `kaix_int`, is a tagged immediate (`0x1` for 0); the boxed `condbr`
   reads it through `kai_op_truthy`, which does `v->tag` and dereferences
   `0x1`. Fix: box as a Bool (`kaix_bool_of_i32` = `kai_bool(i != 0)`) —
   the truthy predicate checks `tag == KAI_BOOL`, and a Bool is never a
   tagged immediate. The C-direct oracle sidesteps this by comparing the
   i32 directly (`setjmp(...) == 0`); the KIR models the split as a boxed
   `condbr`, so the native walk must produce a boxed value the predicate
   accepts.

3. **`k` vs `resume` register naming.** The translator names the `PResume`
   clause param after its surface name (`resume`), but `lower_resume`
   emits `KResume(KVar("k"), …)` with the fixed `k` (mirroring emit_llvm's
   `%k`). The clause body's `resume(...)` therefore reads register `k`,
   which `collect_fn_specs` never created. Fix: seed the clause's PResume
   param into a register named `"k"`, and reserve a matching `k` alloca for
   clause fns in the entry head (`nfx_reserve_k`).

4. **Module privacy, invisible until selfhost.** The bundle-concat build
   (kaic1) ignores `import`/visibility, so cross-module calls to the new
   `nfx_*` helpers and `nemit_declare_clause` compiled fine — and only
   `make selfhost` (real imports) surfaced the missing `pub`. The standing
   "always selfhost before done" rule caught it.

5. **Stale build masquerading as a code bug.** `make kaic1` does not
   rebuild on a `stage1/compiler.kai` edit alone (it keys on
   `build/stage1.c`'s mtime), so a freshly-registered prim (`array_alloca`)
   was absent from the running kaic1, and a stale kaic2 silently kept
   compiling an *old* bundle. Hours of the "0 bodies" hunt were spent
   before `rm -f stage1/kaic1 stage1/build/stage1.c` forced the rebuild and
   the real bug (#1) surfaced. Lesson: when a compiler-source edit seems to
   have no effect, force-clean the bootstrap binary before trusting any
   diagnosis.

## Ev struct layout (the one place IR meets C layout)

The C-direct emit's `struct EvX` is `{ KaiHandlerId handler_id; void *env;
KaiValue *state; <op fn-ptrs…>; }` — a **three-field header**, ops starting
at field index 3 (the first design note assumed two — `state` is always
present, even for stateless handlers). The native walk builds the Ev as a
`[3 + nops x ptr]` alloca and reaches fields by index through the
`kaix_ev_*` helpers, never as a struct-nominal type in IR (opaque pointers
make that unnecessary, and the C-API has no struct-type builder). This
keeps the layout knowledge in C, mirroring the `kai_jmpbuf_size` /
`kaix_handle_discard_unwind` discipline established for #622.

## Gate (every item green at close)

- Native parity vs C-direct: 5/5 (3 pure + 2 effects, incl. the discard
  longjmp path).
- ASAN on the linked native effect executables: 0 errors.
- Selfhost byte-id (default C path): `kaic2b.c == kaic2c.c` — the TyHandle
  / stage1 RP / runtime additions are off the C path, byte-id preserved.
- tier0 OK.
- `km`: emit_native_fx A++ (97.6), emit_native_fx2 A+ (96.9), both
  < 400 LOC; every edited file held its grade.

## Follow-ups for next subsets

- Effects 2b: stateful handlers (`State`, the `state` field + 2-arg
  `KResume2` + `kaix_clause_state_set`), `as`-alias dispatch
  (`kaix_evidence_lookup_node_by_id` + the `__alias_id__` sentinel),
  builtin-default install (`kp.install_order` → `kai_main_install_defaults`,
  today an empty no-op).
- Richer effect fixture that *returns* a handler value (the current pair
  only observes the exit code; a fixture printing a clause result would
  tighten the behavioural check).
- Retire `native_handle_fns()` (now urgent — see bug #1).
- TRMC + KTailCall, #779 EBlock-raw still pending from the pure-walk retro.
