# Lane experience — native non-tail raw-scalar call residual (#861)

## Scope as planned vs. as shipped

**Planned (brief + issue #861):** close the native backend's last non-heap
residual — a non-tail recursive call to a raw-scalar UFn re-boxed its result
(`kaix_int`) and the consuming arith op unbox-borrowed it back
(`kaix_int_field`), one box + one unbox per call. On the `fib` call tree this
was ~8× over the C-direct oracle (deep_rec: native 0.08s vs C 0.01s). The
issue's proposed fix named two sites:

1. `lower_direct_call` — bind the `KCall` result at the callee's RAW return
   slot instead of the unconditional `SBoxed` of `bind_op`.
2. `lower_expr_raw_kind` — add an `ECall` arm so a call in arith-operand
   position lowers raw.

**Shipped:** the issue's diagnosis was correct but INCOMPLETE — closing the
call result alone left two more box/unbox sites on the same `fib` arm. The
full residual was FOUR coupled pieces, all mirroring the C oracle's
fully-raw `int64_t kai_fib(int64_t)`:

1. **Call result raw** (`lower_direct_call`) — bind the `KCall` at
   `ret_slot_of_sig(sig)`. A raw-return UFn lands its `i64` in a raw
   register; a consuming `i+` reads it directly, a boxed use boxes once at
   the border (`nemit_load_reg_boxed`, the oracle's `box_wrap`).
2. **Call args raw** (`lower_direct_call`) — lower each arg at the callee's
   PARAM slot via the existing `lower_tcrec_args` (shared with the
   self-tail back-edge). Without this, `fib(n-1)` round-tripped
   `int.box(n-1)` → the callee's `nemit_raw_args` unbox-borrowed it back.
   The issue did not name this; the KIR `.t3: box = int.box(.t2)` made it
   obvious once the result was raw.
3. **Raw call in arith-operand position** (`lower_expr_raw_kind` ECall arm)
   — route a call operand through `lower_call` so it inherits (1)+(2).
4. **Raw `if`-join** (`lower_if_blocks`) — the DOMINANT site the issue
   missed. `fib`'s body is an `if` whose join register was unconditionally
   `SBoxed`: the `+`'s `i64` was boxed into the join (`kaix_int`) and the
   raw-return `KRet` unbox-borrowed it back (`kaix_int_field`). Mirroring
   the oracle's raw ternary, the join now carries the result's raw slot
   (`if_join_slot`), each arm stores raw, and `ret` reads the `i64`
   directly. This is what actually emptied the `fib` loop of box/unbox.

Result: the `fib` body is native `add`/`sub`/`cmp` only; `fib(34)` runs at
**C parity (0.01s both, was ~8×)**.

## Design decisions and alternatives considered

- **Slot-driven box decision, not a new lowering flag.** The native backend
  is already mode-slave to the KIR's `KSlot` (the #845/#829 discipline): a
  register's slot is the single source of its repr, and the load side
  (`nemit_load_reg_as_int` / `nemit_load_reg_boxed`) already adapts raw↔boxed
  by slot. The fix extends the SAME discipline to the call-result bind, the
  store side (`KStore`), and the `if`-join — no new "is-raw" set threads
  through `LowerSt`. The box happens exactly where a boxed consumer reads a
  raw register, symmetric with the existing load adaptation.

- **Reused `lower_tcrec_args` for call args** rather than a new arg-lowering
  path. It already lowers each arg at a sig's param slot (raw or boxed by
  mask), built for the self-tail back-edge; a normal call has the identical
  need. `None` sig (FFI / effectful / generic / proto) falls back to plain
  `lower_exprs`, so non-raw callees are byte-identical.

- **`if`-join raw scoped to Int/Real only.** `if_join_slot` returns a raw
  slot only for an Int- or Real-typed `MUnboxed` `if`; Bool/Char raw `if`s
  stay boxed, matching the `kir_lower_raw` scope bound (no raw KIR register
  form for Bool/Char). The unbox pass already gates `EIf` `MUnboxed` on both
  arms + the cond being raw, so a raw join is only minted when both arms
  genuinely lower raw.

- **`rspec_add_store` — a store no longer widens its register's slot.** The
  alloca-spec collector treated every `KStore` destination as `SBoxed`,
  which CONFLICTED with the raw `if`-join's `KLet(.t, SInt64)` and widened
  the join back to `ptr` (an `alloca ptr` / `store i64` mismatch). A store
  now keeps the declared slot (its `KLet` decides it) and only seeds
  `SBoxed` for an un-declared name. The `KStore` always follows its `KLet`
  in the same block, so the declaring slot is already present.

- **Raw-arg lowering is fully PARAM-SLOT-DRIVEN, never inferred from the
  arg.** Routing normal calls through `lower_tcrec_args` (built for the
  self-tail back-edge) surfaced THREE arg-marshalling bugs the back-edge
  never hit, because before this lane only self-tail-calls used that path.
  All three are the same root cause — the arg lowering tried to decide the
  raw repr from the ARG instead of the callee's declared PARAM slot — and the
  final fix is one helper, `lower_expr_raw_at(arg, param_slot)`, that crosses
  the box→raw border at the KNOWN slot:

  1. **Bool raw arg** — the type-driven border (`lower_scalar_unbox_border`)
     only split Int-vs-else→Real, so a `neg: Bool` arg fell to `real.unbox`
     → `kaix_bool_field(double)`, a verifier type mismatch. (Added the Bool
     arm + `kir_ty_is_bool` + native `nemit_raw_bool_unbox`.) Surfaced by
     `examples/stdlib/money_basic.kai` (`dec_parse_body(…, neg)`).

  2. **Char raw arg (silent miscompile)** — a `c: Char` param is `mask=true`
     but maps to `SBoxed` (no raw KIR register form). Keying "raw" off the
     bare `mask[i]` lowered it raw while the callee declares `SBoxed` (native
     0 vs C's 5). Fixed by gating on `param_slot_for(ptys, mask)` — the SAME
     slot the param declaration uses.

  3. **Untyped generic-fn arg** — `take(xs, mid)` inside generic `sort_by`:
     `mid : Int` but its `EVar` reference carried no `.ty` (the typer left
     the ref untyped under monomorphisation). `lower_scalar_unbox_border`,
     keying off `mid.ty`, fell to `real.unbox` → `kaix_int_field(double)`,
     breaking the native build of every fixture that streams through
     `sort_by` (the 6 `stream_*` + `wc` regressions the serial ratchet
     caught). Fix: `lower_expr_raw_at` crosses the border at the PARAM's slot
     (`SInt64`), never at `mid.ty`. This subsumes (2) — a `SBoxed` param
     keeps the boxed arg regardless of the arg's type.

  All three are param-vs-arg disagreements that only a call with the
  offending param TYPE / an untyped arg exercises — the lane's own `fib`
  (Int, typed) never hit them; the SERIAL parity ratchet over the full
  corpus did (the parallel ratchet false-greened 6 of them via flaky
  recheck — the documented reason serial is the gate). The lesson:
  marshal a call arg at the callee's declared slot, full stop — never
  re-derive the repr from the arg expression.

## Structural surprises the brief did not anticipate

- **The dominant residual was the `if`-join, not the call site.** The issue
  diagnosed the call-result re-box (real, but a minority of the cost). The
  `fib` body is `if … { n } else { fib + fib }`; the `if`-wrapper boxed the
  `+` result and unbox-borrowed it at the raw `ret`. Fixing only the call
  left the loop still paying one box + one unbox per arm via the join. The
  KIR dump (`--emit=kir`) vs the native LLVM IR dump diverged — KIR showed a
  raw join while the IR showed a boxed `alloca ptr` — which is what
  surfaced the `rspec` widening bug.

- **The hardest bug was an RC-corruption "risk #1" violation, not the
  lowering.** A first cut of the slot-aware `KStore` routed through a helper
  `nemit_store_value(...) : Handle` that RETURNED the raw LLVM value handle.
  The type-blind kaic1 RC-wraps a returned raw pointer (the documented
  `emit_native_fn.kai` "risk #1"), and that corruption rewrote neighbouring
  module state — surfacing as INVALID `select`-shaped CallInsts on UNRELATED
  `kaix_ge`/`kaix_and` declarations in the stdlib `char__is_*` functions
  (which have no calls, no `if`, and an identical KIR to main). The native
  module verifier crashed before even printing the IR. Fix: the slot-aware
  store (`nemit_store_slot_aware`) returns `Unit` and does the
  `nemit_store_reg` internally, keeping the handle inside the seam — exactly
  the "no LLVM handle crosses a return/record boundary" rule the module
  header states. This cost the bulk of the lane's debugging time; the lesson
  is that ANY new native-emit helper returning a `Handle` is suspect.

## Fixtures added and coverage gaps

- `examples/native/nontail_raw_call.kai` — `fib(20)` non-tail tree,
  diffed native-vs-C by `tools/test-native-parity.sh` (gated by
  `tier1-native.yml`). Guards the raw call ABI (result + args) AND the raw
  `if`-join. Asserts `fib(20) == 6765` at parity; the KIR carries
  `.tN: i64 = fib(...)` and a single `i+` with NO `int.box`/`int.unbox` in
  the loop.

- `examples/native/nontail_raw_call_mixed_args.kai` — `classify(c: Char,
  want_x: Bool, n: Int)` non-tail self-recursion. Guards the two arg shapes
  the parity ratchet caught: a `Bool` raw arg (must cross `bool.unbox`, not
  `real.unbox`) and a `Char` arg (`mask=true` but `SBoxed`, must stay boxed).
  Asserts `classify('x', true, 5) == 5` at parity.

- Coverage gap: a Real-typed non-tail recursive `+` (the `SReal` `if`-join
  path) is exercised by construction (`if_join_slot` handles Real) but has
  no dedicated fixture; the Int path is the issue's repro and the common
  shape. A Real fixture is a cheap follow-up if the SReal join ever
  regresses.

## Real cost vs. estimate

The lowering changes were small and direct (the four pieces are ~40 lines
total). The cost was entirely in the RC-corruption bug: a `select`-shaped
verifier crash with a stripped backtrace pointing at `SelectInst::
areInvalidOperands` (a red herring — no `LLVMBuildSelect` exists; it was a
corrupted CallInst). Bisecting it required a per-function `LLVMVerifyFunction`
debug hook to localise the broken stdlib fn, then a per-piece re-enable to
isolate the offending helper. The fix once found was one line (drop the
`Handle` return).

## Follow-ups left for next lanes

- The remaining native heap residual is the cons/list RC path (#860, now
  closed in #863) and the `kai_op_eq` UAF (#858, closed in #862); this lane
  was the non-tail scalar residual, the last non-heap one. No new follow-up
  opened.
- The `nemit_store_slot_aware` pattern (Unit-returning, handle-internal) is
  the right shape for any future slot-aware native-emit helper; worth a note
  in the module header if a third such helper appears.
