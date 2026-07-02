# Lane experience — issue #1025 bug#3: native cons-reuse dual-branch (+ the chain behind it)

## Scope as planned vs shipped

Planned: fix bug#3 — the last miscompile blocking the native-built compiler
from compiling any program (`field access on non-record` in
`validate_pub_access`, a resolved `DImpl` read back as a live cons cell) —
and close #1021 with the full recursive self-host.

Shipped: bug#3 fixed (genuine dual-branch cons reuse) **plus** a second,
pre-existing lowering bug the fix un-masked (bug#4: un-dup'd nested
discriminating binders). The recursive self-host does NOT close: a third,
independent pre-existing bug (bug#5, TRMC `__kai_cons_s` double-free in
`append_op_arities`) sits behind them, and per the lane's stop rule the
chain was documented and escalated instead of chased further. PR ships with
`refs #1021 #1025`, not `closes`.

## The fix (bug#3): genuine dual-branch, not a smarter single body

Root cause (hardware watchpoint, prior session): in a cons-reuse arm
`[d, ...rest] -> [f(d), ...g(rest)]`, when `f` reuses `d` in place the
rebuilt head IS the donor's old head cell; the single-body native reuse kept
one binding mode for both uniqueness outcomes, and the match-exit
`decref(donor)` cascaded onto the just-embedded result. Whether `f` reuses
its argument is not decidable at the call site — three prior attempts
(dup-all, direct-binder KDup, arm-top token model) all failed because they
patched a single-body model that cannot express per-branch ownership.

The dual-branch (mirroring `emit_c`'s `emit_match_arm_reuse_cons`):

- fork on `kaix_check_unique(donor)`; rebuild args lowered TWICE (textual
  duplication, branches mutually exclusive — exactly how the C oracle
  interpolates `h_new_c` into both arms);
- UNIQUE: direct cons slots (head `PBind`, `...rest`) stay borrows (no
  KDup); `kaix_cons_reuse_move` writes the new slots RAW — no decref of the
  old slots, since a borrow gave up no ownership. Borrow-no-incref and
  store-raw-no-decref are two halves of one saving and must travel together;
- SHARED: dup the direct slots, fresh-alloc;
- DECONSTRUCTED sub-binders of a head (`RP(name, _, ptys, rty)`) are dup'd
  unconditionally OUTSIDE the fork — the oracle's `emit_pat_binds`
  `is_alias=true` path increfs them in both branches. The partition is
  static, by pattern shape, which is what every single-body attempt missed.

Two design walls that turned out to be false:

1. "The all-boxed KIR cannot express the borrow the unique branch needs" —
   wrong: C is borrow-pure only for the DIRECT PBind/PWild cons slots; a
   deconstructed head increfs its sub-binders in BOTH branches. A borrow in
   the KIR is simply "omit the KDup", no borrow-read call needed.
2. Applying the dual-branch to VARIANT reuse as well — wrong: the AST
   already carries perceus dups protecting re-embedded variant slot binders,
   so a variant fork double-counts. Flat variant reuse keeps the single-body
   move (`kaix_variant_reuse_at`); only the CONS arm needs the fork.

One trap worth recording: the shared branch first emitted
`KCon("Cons", ...)` — but `Cons` is the builtin list ctor, not a registered
variant, so the native backend allocated a KAI_VARIANT cell whose
`as.cons.head` read back `var_n_args = 2` (the literal `0x2` pointer that
crashed infer). Reduced fixtures missed it because fresh lists are always
unique; only shared stdlib spines take the shared branch. Fresh cons cells
must go through `KPrim("kai_cons")`.

## Bug#4 (pre-existing, un-masked): nested discriminating binders never dup'd

With bug#3 fixed the pipeline reached `check_field_privacy` and died on a
heap-use-after-free ASAN pinned precisely: `lookup_record_field`'s arm
`DType(_, tn, _, TBRecord(fields), _, _, mo)` binds `fields` two levels down
(inside the discriminating nested sub-pattern), but the #858 structural dup
used the FLAT top-level collector (`variant_arm_alias_binders`), so `fields`
stayed a bare KProj borrow; `find_field_in_decls(fields)` consumed the ref
the decl's slot still held, and the second lookup of the same record walked
freed memory. Fix: one line — use the recursive `owned_arm_alias_binders`
(the same set #1028 validated for self-tail arms). The C oracle was never
affected (`emit_pat_binds` increfs at every depth), which is why tier1-asan
(C backend) stayed green.

## Bug#5 (pre-existing, escalated): TRMC `__kai_cons_s` double-free

Next ASAN stop: `infer__append_op_arities`
(`[h, ...rest] -> [h, ...append(rest, b)]`) — lowered as a TRMC
`trmc-step __kai_cons_s`, NOT through the reuse path. alloc/free/use all
inside the function; runs in `add_effect_op_sigs`, so every compile hits it.
Third distinct mechanism; left documented with the ASAN stack + KIR dump.

## What made this lane tractable

- **ASAN on the self-hosted binary** was the decisive tool: compile
  `main.kai` with `KAI_NATIVE_RUNTIME_BC=` (no embedded bitcode), build
  `runtime_llvm.c` with `-fsanitize=address -DKAI_LLVM -DKAI_NO_CELL_POOL`,
  link with asan. Each chain link fell in ~20 minutes with exact
  alloc/free/use stacks, where watchpoints had cost hours (heap addresses
  are not stable across runs — libLLVM in-process allocations shift the
  layout even under lldb).
- **Whitelist bisection in the lowering** (dual-branch gated per enclosing
  fn symbol) separated "my fix regressed X" from "pre-existing bug
  un-masked": the empty whitelist reproduced bug#3 exactly, proving the
  harness faithful.
- The `.bc` is EMBEDDED into `main.o` at compile time: any runtime
  instrumentation requires regen-bc → recompile main.o → relink. One
  earlier bisection was invalidated by skipping the middle step.

## Fixtures added

- `examples/perceus/cons_reuse_embed_unique_1025.kai` — the unique-branch
  embed shape (`f(d)` reuse-in-place, result read back).
- `examples/perceus/variant_nested_discriminant_dup_1025.kai` — the level-2
  discriminating binder consumed by a callee, list re-read afterwards.
- (`cons_reuse_shared_donor_1025.kai` from #1027 already covers the shared
  donor.)

Coverage gap: none of the reduced fixtures crash pre-fix under the plain
native build (allocation-layout dependent); they pin behaviour, while the
real gate remains the self-host compile itself, which now needs bug#5.

## Cost vs estimate

Estimated as "one deep bug". Actual: one deep bug plus a chain of two
pre-existing RC-lowering gaps behind it, each with a different mechanism.
The pattern to expect for the remaining self-host work: the compiler binary
is the first program to exercise the native RC lowering across ALL arm
shapes on shared heaps; each newly-reached pipeline stage may surface
another gap. ASAN-on-self-host is the cheap detector.

## Follow-ups

- bug#5 (TRMC `__kai_cons_s`): filed with ASAN stack + KIR; blocks #1021.
- Consider a systematic RC audit of `kir_lower_*` arm paths against
  `emit_pat_binds` (the oracle's is_alias discipline) rather than
  link-by-link discovery, if bug#5's fix reveals a fourth gap.
