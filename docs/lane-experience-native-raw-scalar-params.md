# Lane experience — native raw scalar params (P3)

**Lane:** `native-raw-scalar-params` · **Date:** 2026-06-17 · **Backend:**
in-process libLLVM (KIR Lane 1.5) · **LLVM:** 18.1.8 (Homebrew `llvm@18`).

P3 of `docs/native-codegen-perf-plan.md`: make the native backend emit
function signatures with **raw scalar params/returns** (`i64`/`f64`/`i1`)
instead of all-`ptr` boxed. With raw params, the body (already unboxed by P1)
and the runtime (inlined by P2 before O2) compose into pure native arithmetic
that O2 amplifies exactly as it does the C-direct backend's.

## Scope as planned vs as shipped

Planned (per the brief's six pre-made decisions): teach the native emitter to
READ the `UFnSig` the typer already computes, with the C-direct emitter as the
byte-exact oracle. Shipped exactly that, plus one structural correction the
brief did not anticipate (the return-value crossing — see below) and two
back-edge paths the brief named only one of (TRMC alongside TCRC).

The eligibility condition is **not** re-deriving the `UFnSig` — it is reading
the KSlot the lowering populated from it. This is the #845/#829 mode-slave
discipline: KSlot is the single source of a register's type; the native walk
reads `KParam.slot` / `KFn.ret`, never the `UFnSig`. The C-direct emitter
keeps reading the `UFnSig` directly (it emits from the AST, not the KIR).

## The five pieces (all behind one decision: populate the KSlot from the UFnSig)

1. **Lowering populates raw slots** (`kir_lower_fns.kai` `lower_fn` +
   `kir_lower.kai` `lower_params_sig` / `ret_slot_of_sig` /
   `kslot_of_raw_ty`). `lookup_ufn_sig(st.fns, name, mo)` resolves the fn's
   sig; each raw param (mask[i]) gets `SInt64`/`SReal`/`SBool`, the return its
   raw slot. **Char stays `SBoxed`** — the KIR has no raw Char register form
   (no `kaix_char_field` native shim), so the native does not raw-promote a
   Char param even though the `UFnSig` mask includes it. This is consistent
   WITHOUT matching the C IR byte-for-byte: the native decides raw by KSlot,
   the C by the `UFnSig` mask, and each backend reads/writes a Char uniformly
   within itself. The unbox pass reads the SAME mask (`ufn_param_env`), so the
   body's raw param reads and these slots can never disagree.

2. **Raw LLVM signature + param seed** (`emit_native_fn.kai` `nfn_llvm_type` /
   `nfn_has_raw_slot`; the seed in `nemit_seed_params` was already slot-driven
   — `collect_param_specs` allocas at `p.slot`, so a raw param's alloca is
   `i64` and `store (param) → alloca` just works once the signature is raw).
   The forward declaration (`nemit_declare_fns`) and the definition
   (`nemit_declare_for_abi`) BOTH call `nfn_llvm_type` — they must agree, as
   `get_or_declare_fn` is type-keyed.

3. **Call-site borders** (`emit_native_ops.kai` `nemit_call_direct` /
   `nemit_call_raw`). A `KCall` to a raw UFn (looked up by symbol in the
   threaded `fns` list) passes each arg at its param slot (raw arg direct, boxed
   arg unbox-borrowed via `nemit_atom_raw`) and re-boxes a raw return (the KCall
   result register is always boxed — `lower_direct_call` binds it `SBoxed`).
   Mirror of emit_c's `emit_ufn_boxed_call` + `ufn_call_result`. The direct-vs-
   indirect decision is reused verbatim from the KIR (`callee_is_direct`, which
   already honours the locals-shadow rule — a `KCall` IS direct, a
   `KCallIndirect` is not).

4. **The thunk bridge** (`emit_native_term.kai` `nemit_one_fn_thunk`). The
   `_kai_<sym>_thunk(self, args, n)` is the boxed-ABI NET every indirect /
   `KClosure` call enters through. It ALWAYS exists; for a raw fn it unboxes
   each masked arg, calls the raw direct fn, and re-boxes a raw return. This was
   the first crash: before the bridge, the thunk called the (now raw) fn with
   the boxed signature → LLVM `Call parameter type does not match` abort.

5. **Both re-loop back-edges** (`nemit_reloop_value` in `emit_native_fn.kai`,
   shared by `KTcrecGoto` in `emit_native_term.kai` and `KTrmcStep` in
   `emit_native_trmc.kai`; the lowering side is `lower_tcrec_args` in
   `kir_lower_walk.kai`, used by both `lower_tcrec` and `lower_trmc_seal`). A
   raw param's re-loop value is materialised raw — no per-iteration `kaix_int`
   re-box, and the store matches the `i64` alloca. The lowering also lowers the
   self-call's args raw (resolving the recursing fn's sig by its mangled symbol
   via `lookup_ufn_by_csym`), so the back-edge carries a raw register, not a
   boxed one the native then has to unbox.

## Structural surprises the brief did not anticipate

- **The return crossing is in the native `KRet`, not the body lowering.** The
  brief framed borders as "box at the call-site / unbox at the border." The
  first instinct — lower a raw-return UFn's BODY raw so the `KRet` carries a raw
  value — broke the many `…is_none : Bool` fns: `lower_expr_raw` on a Bool body
  routes through `lower_scalar_unbox_border`, whose Int-vs-else split mis-routes
  a Bool to `real.unbox` (an `f64` fed to `kaix_bool_field` — a verify
  failure). The fix keeps the body BOXED and crosses to raw only at the
  native `KRet` (`nemit_ret_value` → `nemit_atom_raw`, which knows the exact
  return slot). One crossing, at the one point the slot is known exactly.

- **TRMC has its own back-edge.** The brief named the TCRC back-edge
  (`tcrec_emit_*_masked`). The mixed-signature fixture (`insert(Tree, Int,
  Int)`) recurses modulo-cons (TRMC), and `KTrmcStep` re-stores params through
  a SEPARATE path (`ntrmc_eval_assigns`/`ntrmc_store_assigns`). Fixing only
  TCRC left the TRMC back-edge storing a boxed `ptr` into an `i64` alloca — a
  silent miscompile (native returned garbage, C returned 1488). Both back-edges
  now share `nemit_reloop_value`.

- **Bundle-concat hid two module-boundary errors selfhost caught.** (a)
  `lookup_ufn_by_csym` was private to `emit_c`; the KIR lowering needs it too,
  so it moved to `emit_shared` (pub). (b) A helper (`nparam_type_push`)
  carried a spurious `/ Console` it does not perform; the bundle ignored the
  over-propagation, selfhost's per-module effect inference flagged the caller.
  Always `make selfhost` before "done" — `make kaic2` (bundle) is false-green
  on privacy + effect rows.

- **Raw proto impls broke the dispatch ABI — caught by CI, masked locally by
  the flaky-held ratchet.** A pimpl like `Hash_String_hash(String) : Int` now
  classifies raw (`i64 (ptr)` — boxed param, raw return). `emit_native_proto`
  registered the impl fn-ptr verbatim into the runtime table, on the
  documented assumption "the KIR is all-boxed, the impl IS the dispatch ABI."
  P3 broke that: the dispatcher (`kaix_proto_dispatch1`) casts the registered
  fn to `KaiValue*(KaiValue*)` and calls it — a raw `i64(ptr)` impl returns an
  `i64` read as a `ptr`, dereferenced → SIGSEGV (hashmap/hashset). The fix
  mirrors the C-direct oracle: emit a `_kai_<csym>__boxed(ptr × arity)` adapter
  (unbox masked args, call the raw impl, re-box the raw return) and register
  THAT for a raw impl (`pimpl_row_is_unboxed`), the bare csym for a boxed one.
  Adapters are emitted in an up-front pass (before `_kai_proto_init_llvm`
  opens) so each owns the builder cleanly. **The local ratchet marked these
  "flaky — passed on recheck" (the parallel-worker false-green the memory
  warns about); only CI's serial-ish run failed consistently.** Lesson:
  proto-heavy fixtures need a SERIAL native-vs-C check, not the parallel
  ratchet, when an ABI-touching change lands. A second trap surfaced fixing
  this: `nproto_scalar_ty` (`Ty -> Handle`, a raw `void*` return) recursed
  TAIL on Dim/Refine; the C-direct TCO rewrite emitted `_r = (goto-stmt)`,
  which cc rejects as int->pointer for a `void*` return. Isolating the
  recursion in a `Ty -> Ty` strip helper (`nproto_strip_ty`, boxed return, no
  raw-goto hazard) + flat matches fixed it.

- **The RC double-free trap on buffer construction.** `nfn_llvm_type`'s first
  form passed `llvm_buf_new()` INLINE as an arg (`nparam_type_buf(ctx, ps,
  llvm_buf_new())`). The type-blind kaic1 RC-dropped that inline handle, then
  `llvm_buf_free` freed it again → a hard LLVM abort with no IR dump (the crash
  is during construction, before the pre-verify dump). The fix is the
  established idiom: create the buffer in a `let` first, fill it via a helper
  (mirror of `nemit_push_atoms`). Diagnosed by tracing `begin_fn` /
  `get_or_declare_fn` in the C ctx until the last good point was located.

## Soundness

- **RC:** a raw param/return carries no refcount header, so it never enters
  Perceus (mode-slave to the body's unbox, the #829/#845 precedent).
  `KAI_TRACE_RC` on the pure-raw 200M-iter loop: `alloc_total=7` (setup only,
  NOT 200M), `incref_total=0` over the loop — zero RC on the raw slots.
- **Tagged-int:** all box→raw crossings use `kaix_int_field` / `kaix_int`
  (which go through `kai_intf`/the cache), never `->as.i` / `->tag` (the
  0x1-segfault trap of the two `runtime.h` copies).
- **Wrapping:** Int arith stays raw `add`/`sub`/`mul` (no `nsw`), already P1.
- **Back-edge:** a raw param is re-stored raw and never dropped (no RC); a
  boxed param keeps the prior behaviour (the dropmask the native already
  ignored pre-P3 — out of this lane's scope).

## Measured native-vs-C factor (the point of the lane)

`sum_loop(i, acc)` — the brief's fixture — now compiles to
`define i64 @sum_loop(i64, i64)` (was `ptr @sum_loop(ptr, ptr)`).

| bench (200M iter) | before P3 | after P3 | C |
|---|---|---|---|
| pure-raw arith loop (`+ - *` only) | 1.94 s (~21× the brief's quote; ~4.7× this host) | **0.00 s — parity with C** | 0.00 s |
| `arith_runtime` (`… + i*i - i/3`) | 1.94 s | 1.65 s (~23×) | 0.07 s |

The pure-raw loop reaches **parity with C** (both const-folded by O2 once the
loop body and back-edge are wholly raw — the `i64 @sum_loop(i64,i64)` body
carries ZERO `kaix_*`). `arith_runtime`'s residual is the boxed `i / 3`:
native `sdiv`/`srem` are UB on `/0` and `INT_MIN/-1`, so P1 deliberately keeps
`/` and `%` boxed (`kaix_div`). That `kaix_div` (+ the `kaix_int` of its
operands + `kaix_int_field` of its result) is the entire residual — it is a P1
scope boundary, NOT a P3 gap. A future lane wanting div-heavy loops fast would
need a guarded raw `sdiv` (branch on the UB cases), out of scope here.

## Fixtures added

- `examples/native/raw_scalar_sum_loop.kai` — the pure-raw arith loop; the
  positive fixture asserting native == C and (via the IR-grep in the test) the
  raw `i64` signature.
- `examples/native/raw_scalar_mixed.kai` — the balance-shape mixed signature
  (`insert(Tree, Int, Int)`: boxed Tree + raw k/depth, TRMC recursion); guards
  the TRMC back-edge under a partial raw mask. Both verified native == C.

## Coverage gaps

- Char/Handle params stay boxed by design (no raw KIR register form). If a
  future lane adds a raw Char slot, `kslot_of_raw_ty` is the one seam to extend
  (plus the `kaix_char_field`/`kaix_char` native shims).
- The boxed `/`,`%` residual (above) bounds div-heavy loop perf; logged here so
  it is not re-discovered as a P3 regression.

## Real cost vs estimate

The implementation is small (the brief's "one lane" held). The cost was three
debugging round-trips, each a structural lesson, not a code-volume one: the
thunk-bridge crash (ABI net), the Bool-return verify failure (return crossing
belongs in `KRet`), and the mixed-fixture miscompile (TRMC back-edge). Each was
caught by a minimal fixture + IR inspection, exactly the "measure, don't
promise" discipline.

## Follow-ups for next lanes

- Guarded raw `sdiv`/`srem` for div-heavy loops (the `arith_runtime` residual).
- Raw Char param slot (if a Char-heavy hot path ever shows up).
- The native back-edge still ignores the TCO dropmask (pre-P3 behaviour); a
  boxed param's old value is not dropped before overwrite. RC-neutral for raw
  params (no header); for boxed params it is the same as before P3. A separate
  RC lane owns that.
