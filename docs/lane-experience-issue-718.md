# Lane experience — issue #718: scalar function signatures in the LLVM backend

**Scope:** make the LLVM backend emit scalar (unboxed) function
signatures for UFns — `define i64 @kai_deep(i64 %kair_n)` — matching the
C backend's `int64_t kai_deep(int64_t)`, instead of the all-boxed
`%KaiValue* @kai_deep(%KaiValue*)` with a `kaix_int` heap box per
arithmetic op. Closes #718 (the last open #622 parity divergence,
`m8_fiber_stack_overflow`) and empties the #622 parity-bugs section.

**Layer:** the codegen *consumer* layer (`stage2/compiler/emit_llvm.kai`)
only. The unbox classification (`unbox_pass` in `unbox.kai`, the
`UFnSig = US([Ty], Ty)` registry in `fnreg.kai`) was reused untouched —
this lane consumes `UFnSig` in a second backend exactly as #706/#708
consumed the shared `tcrec` analysis. No C-backend change (selfhost
byte-identity proves it), no unbox-analysis change.

## Localized UFnSig-consume, not an ABI rework

The brief flagged the risk that mirroring the C unboxed-signature path
might require reworking the LLVM value-mode dispatch or the call ABI. It
did not. Every piece is a read of `UFnSig` and an emit decision keyed on
it, structurally identical to how the C emitter already bifurcates. I
consulted the asu architect before writing (the one structural zone was
the TCO×scalar interaction, below); asu confirmed it is the natural
completion of the TCO path, not an ABI change. The five pieces:

1. **Signature + body** (`llvm_emit_fn`): resolve `lookup_ufn_sig` once,
   `match` on it. UFn → `define <ret_ir> @kai_<sym>(<ir> %kair_<p>, ...)`,
   params seeded as `LRaw(name, "%kair_<name>", ty)` (the issue #87
   raw-local mechanism — reused, not reinvented), body emitted via
   `llvm_emit_expr_raw`, `ret <ret_ir> <raw>`. Boxed → the unchanged
   `%KaiValue*` path. `main` is excluded by the classifier (its row is
   non-empty), so the runtime entry-point ABI is untouched.

2. **Call-sites, boxed context** (`llvm_emit_call`): the two named-call
   arms (EVar→standard-call, EModCall) now check `lookup_ufn_sig` /
   `efn_resolve` first. A UFn callee emits `llvm_emit_ufn_call_boxed`:
   raw-arg call (`call i64 @kai_<sym>(i64 ...)`) then `llvm_box_wrap_value`
   — mirror of C's `emit_ufn_boxed_call` (`box_wrap(rt, raw_call)`). A
   `lcs`/local shadow short-circuits to the boxed path, mirroring C's
   `list_has(lcs, callee)` guard.

3. **Call-sites, raw context** (`llvm_emit_kind_raw` ECall arm): was a
   `_` fallback that emitted boxed-then-unbox. Now a UFn callee emits
   `llvm_emit_ufn_call_raw` (no `kaix_int`/`kaix_to_int` round-trip),
   and a tcrec sentinel routes through `llvm_emit_call`. Mirror of C's
   `emit_ufn_raw_call` + the tcrec arm in `emit_kind_raw`.

4. **Thunk** (`llvm_emit_thunk`, now taking `Option[UFnSig]`): a UFn's
   first-class `_kai_<sym>_thunk` unboxes each boxed `args[i]` to a raw
   `%ar<i>` (via `kaix_to_*`), `kaix_internal_drop`s the boxed slot
   (mirror of C's `kai_decref(args[i])`), calls the raw entry, boxes the
   raw return. Keeps closure / passed-as-value uses working.

5. **TCO raw back-edge** — the one structural piece (below).

## The structural piece: TCO × scalar signatures

The #706/#709 LLVM TCO path was **boxed-only by explicit design**. #709's
note says it outright: "the LLVM backend has no raw-scalar param path —
every param is boxed `%KaiValue*`." Its back-edge `alloca`s
`%KaiValue*` slots, reloads `%p_<name>`, and on a UFn back-edge dropped
**every** slot value (`llvm_emit_tcrec_drops_all`) because the boxed
borrow-read body left each slot live.

With scalar signatures, a self-tail-recursive UFn needs the **raw** TCO
path the C backend has always had (`tcrec_emit_goto`'s `Some(US(...))`
branch, which emits ZERO drops and rebinds raw). I added the mirror:

- `llvm_emit_tcrec_slots_raw` / `_loop_header_raw`: `alloca iN` slots,
  `store iN %pargr_<name>`, reload as `%kair_<name>`.
- `llvm_emit_tcrec_goto_raw`: evaluate new args via `llvm_emit_expr_raw`
  into temps (decoupled from the stores so a new arg reading an old
  param sees the pre-iteration value — mirror of C's
  `tcrec_emit_arg_lets_raw`), `store iN`, `br %tcrec.loop`. **No drops**
  — raw scalars carry no reference count.
- `llvm_emit_tcrec_goto` now `match`es `llvm_lookup_ufn_by_csym` and
  dispatches raw vs boxed. asu's load-bearing advice: wire the `is_ufn`
  decision once and thread it to the three sites (slots, header, goto),
  or a "raw signature + boxed reload" mismatch generates ill-typed IR.
  I keyed all three off `opt_sig` in `llvm_emit_fn` and the
  `llvm_lookup_ufn_by_csym` in the goto.

This **deletes** `llvm_emit_tcrec_drops_all` (the #709 workaround): a UFn
now takes the raw back-edge with zero drops, so the boxed-UFn-with-leak
case it patched no longer exists. #709's residual cons-cell leak (1 box,
bounded, non-UFn) is unchanged and out of scope.

## The non-obvious second half: raw EIf / EBlock-tail

Emitting the scalar *signature* alone was not enough. The first build
gave `define i64 @kai_deep(i64)` but the body still boxed the `if n==0`
condition (`kaix_bool` → `kaix_truthy`) and re-boxed each arm — because
`llvm_emit_kind_raw` had **no EIf or EBlock arm**; both fell to the
boxed-then-unwrap `_` fallback. The C `emit_kind_raw` had had these raw
arms all along (ternary for EIf, tail-forward for single-tail EBlock);
the LLVM raw lowerer was simply incomplete, left that way because before
#718 no UFn body was ever emitted raw end-to-end. The measured effect:
with signature-only, `kai_deep`'s -O2 frame was 48 B (50k frames = 2.4
MB, overflows 64 KiB); with the raw EIf/EBlock arms added, the body is
pure `icmp`/`sub`/`add`/`phi` — identical to C — and the frame drops to
32 B / collapses, so 50k frames hold. I added `llvm_emit_if_raw`
(`br i1` + raw phi of the result ty) and a raw single-tail EBlock arm.
**Lesson:** "consume UFnSig at the signature" is necessary but not
sufficient; the raw expression lowerer must be complete enough that the
body produces no boxing, or the frame stays large and the fiber still
overflows. This is the part a signature-only reading of the issue would
miss.

## Verification

- **`m8_fiber_stack_overflow`: 50000, exit 0 on BOTH backends** (was
  exit 138 / fiber stack overflow under LLVM).
- **`examples/perceus/scalar_fn_sig_deep_recursion.kai`** (new, smaller
  isolation, non-tail `deep(200000)`, no spawn): 200000, exit 0 on both.
- **`make selfhost` byte-identical** (`kaic2b.c == kaic2c.c`) — the
  decisive gate; the stage-2 compiler is full of unboxable scalar fns,
  and a wrong scalar-sig emission would crash or change its output.
- **`make selfhost-llvm` deterministic** (`s1.ll == s2.ll`) — the
  compiler emitted *through the LLVM scalar-sig path* is itself stable.
- **`make tier0` green**; **tier1** green (two existing LLVM IR-shape
  tests needed their `awk` updated from `define %KaiValue* @kai_…` to
  `define i64 @kai_…` — `test-tco-llvm-706` and `test-unbox-llvm-bench`;
  the latter upgraded from link-only to run + C==LLVM cross-check, which
  the original comment requested "when LLVM TCO lands").
- **ASAN/UBSan**: clean (scalar/boxed boundary mistakes would be type
  confusion / UAF; the run found none).
- **`test-tco-llvm-709`**: `leaked=1` constant at N=1000 and N=100000 —
  the raw TCO back-edge introduces no per-iteration leak.

## Unbox-bench alloc delta

`unbox_bench` / `unbox_bench_real` now **run to completion on LLVM**
(both previously overflowed) with `KAI_TRACE_RC` traces **byte-identical
to C**: `alloc_total=1000003`, same per-tag counts. The delta vs pre-fix
LLVM is qualitative — from "boxes per arithmetic op per frame, overflows
before finishing" to "allocates only at genuine boundaries, exactly as
C." The inner `pure_chain` loop body is now 48 raw `i64` ops with zero
boxed `kaix_*` arithmetic and one boundary box (the multi-stmt block
tail; the loop arithmetic itself is fully raw).

## Cost vs estimate

Roughly as estimated for a "weight of #708" lane (+705/-180 in
emit_llvm.kai, comparable to #708's +606/-47). The bulk was reading the
C `emit_c.kai` oracle paths (signature, call-sites, thunk, tcrec raw
branch) and mirroring each. Two cost surprises, both caught by the local
gate before any push: (1) the inline `match pts { [t, ..._] -> t [] ->
fb }` form mis-parsed under kaic1 (`t []` read as an index) — extracted
to `pts_head_or`/`pts_tail`/`pts_head_ir` helpers; (2) the
signature-only build still overflowed because the raw EIf/EBlock arms
were missing — the `bin/kai` build + `clang -S` frame inspection (48 B →
32 B) is what surfaced it, not the fixture pass/fail alone.

## Follow-ups left for next lanes

- The multi-stmt EBlock body still round-trips its tail through one
  boundary box (`%t… = kaix_int(...)` then `kaix_to_int` before
  `ret iN`). C does the same; eliminating it is a raw-multi-stmt-block
  optimization, orthogonal to #718.
- This empties the #622 parity-bugs section. **#622 can close** alongside
  #718 — every LLVM↔C divergence audited under #622 is now resolved or
  exempt-by-design (non-determinism).
