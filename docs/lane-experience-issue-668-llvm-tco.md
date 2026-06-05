# Lane experience — issue #668 LLVM TCO (stack overflow on deep tail recursion)

## Scope as planned vs as shipped

**Planned (from the user's ask + asu consult):** the LLVM backend overflowed
the 64 KiB fiber stack on `list.map` over a large list inside a fiber
(`examples/effects/issue_668_map_large_in_fiber.kai`: C exits 0, LLVM exits
138). The asu consult diagnosed it as missing TRMC (tail-recursion-modulo-cons)
lowering — the LLVM backend ran `tcrec_rewrite_decls(post_perceus, allow_trmc =
not use_llvm)`, so modulo-cons functions fell back to ordinary recursion. The
plan: port the `__kai_trmc|` step + `__kai_trmc_apply` sentinel lowering to
`emit_llvm.kai` with reuse-in-place, enable `allow_trmc=true`, gate on
issue_668 exit 0 + parity 388 + selfhost + ASAN.

**Shipped:** TWO things, because the TRMC diagnosis was only half the story.

1. **The actual issue_668 root cause — alloca accumulation in the TCO loop.**
   The functions in the fixture (`map_loop`, `reverse_loop`, `length_loop`)
   were ALREADY TCOed by the plain `__kai_tcrec|` path (#706); the IR showed
   `tcrec.loop` + back-edge for all of them. The fiber still overflowed because
   each loop iteration's `kaix_apply` arg buffer (`alloca %KaiValue*, i32 1`)
   sits INSIDE the `tcrec.loop` block. In LLVM an alloca outside the entry
   block executes every time control flows through it and is NOT reclaimed
   until the function returns — 40K iterations accumulate ~25 B each and
   overflow. The C backend never hit this because it emits block-scoped C
   compound-literal arrays (`(KaiValue *[]){...}`) whose storage is freed at
   the end of each `({ ... })` statement-expression. Fix: bracket the
   `tcrec.loop` body with `@llvm.stacksave` (at the loop header) and
   `@llvm.stackrestore` (before every back-edge), reclaiming each iteration's
   allocas. This dropped issue_668 from O(n) stack (needed 1 MiB for 40K) to
   O(1) (passes with a 16 KiB fiber stack).

2. **TRMC modulo-cons lowering for LLVM (the planned work).** Still shipped —
   it is a genuine feature-parity gap (modulo-cons reuse-in-place was C-only)
   and the asu-recommended path. `llvm_emit_trmc_goto` builds the spine node
   with a NULL hole, `kaix_cctx_extend`s the constructor context (two prologue
   allocas `%_kai_acc.res` / `%_kai_acc.hole`), rebinds loop params from the
   self-call args, and branches back; `llvm_emit_trmc_apply` plugs the terminal
   leaf into the open hole. `allow_trmc` is now `true` unconditionally.

## Design decisions and alternatives

- **stacksave/restore vs hoisting allocas to entry.** Hoisting per-iteration
  allocas to the entry block would also work, but the buffer size can vary per
  call site (different arities) and hoisting a dynamic `alloca` to entry is
  awkward; stacksave/restore is the idiomatic LLVM answer for "allocas in a
  loop" and is local to the TCO machinery. The `%pslot_<p>` param slots and the
  `%_kai_acc` cctx allocas stay in entry (before the stacksave), so the
  back-edge restore never frees them — only the iteration's transient buffers.

- **TRMC v1 fresh-allocs the node.** Like the C backend's v1 (emit_c.kai:12877
  "v1 allocates the node FRESH"), the LLVM step calls `kaix_variant` rather than
  donating a reuse token (`_arm_ru`). Reuse-in-place is an orthogonal increment;
  the correctness (O(1) stack, identical output) does not depend on it.

- **Rejected (asu): the accumulator+reverse bridge.** Never considered for the
  alloca fix; for TRMC, asu rejected lowering modulo-cons to an accumulator loop
  (loses reuse, creates a third TCO strategy that lives only in LLVM).

## Structural surprises the brief did not anticipate

- **The brief blamed TRMC; the real bug was alloca accumulation.** The asu
  consult (and the issue title) framed issue_668 as a TRMC gap. It was not: the
  fixture's functions use the accumulator form (`map_loop(t, f, [f(h), ...acc])`)
  which is plain self-tail, already TCOed. A frame-pointer backtrace from the
  fiber's ucontext (the sigaltstack `backtrace()` cannot see the fiber stack)
  showed a SHORT stack — `map_loop` appeared once — while the overflow scaled
  linearly with n (1 MiB for 40K). ~25 B/elem is far too small for a stack
  FRAME; it is the size of one `alloca %KaiValue*` slot. That ruled out
  recursion and pointed at per-iteration alloca leak. Lesson: when "TCO is
  missing" but the IR clearly shows `tcrec.loop`, suspect what the loop BODY
  leaks, not the tail call.

- **Two separate runtime.h files.** The C backend includes `stage2/runtime.h`,
  the LLVM backend `stage0/runtime.h` (+ `runtime_llvm.c`); they diverge ~1500
  lines. A spine-drop hypothesis (later refuted) was briefly chased in both.

## Fixtures added and coverage gaps

- `examples/perceus/trmc_modcons_llvm.kai` + `.out.expected` — modulo-cons
  (`map_inc`, `dup_each` with a nested cons) with VALUE goldens, run by
  tier1-backend-parity on both backends. issue_668's existing fixture covers the
  fiber-stack-O(1) shape; this new one covers the TRMC step/apply lowering
  directly.
- Gap: no fixture asserts the 16 KiB-stack O(1) bound mechanically (the parity
  harness runs with the default 64 KiB). The O(1) claim was verified manually
  (`KAI_FIBER_STACK_SIZE=16384`).

## Cost vs estimate

The TRMC lowering (the planned work) was ~half the diff and compiled/passed
quickly. The bulk of the time was diagnosis — the backtrace from inside the
fiber needed a hand-rolled arm64 frame-pointer walk + ASLR-slide arithmetic
because lldb hung on the fiber-stack overflow and `backtrace()` runs on the
sigaltstack. The stacksave/restore fix itself was ~20 lines.

## Follow-ups for next lanes

- **TRMC reuse-in-place on LLVM.** v1 fresh-allocs; port the `_arm_ru` reuse
  token donate (the C backend's increment) to `llvm_emit_trmc_goto` for the
  zero-spine-alloc path.
- **No new compiler bug — a name collision, not a codegen fault.** A scratch
  fixture defined `fn sum(xs, acc)` (2 args), colliding with stdlib `list.sum`
  (1 arg). Inside `list.sum`'s own body the recursive `sum(t)` resolved to the
  user's 2-arg `kai_sum` instead of `kai_list__sum`, so the C backend emitted
  `kai_sum(kai_t)` (1 arg) against a 2-arg forward declaration → "too few
  arguments". The LLVM backend happened not to surface it (different symbol
  binding). This is the known stdlib-name-shadowing hazard (a user fn named like
  an auto-loaded stdlib fn breaks far away), NOT a regression from this lane.
  Renaming the fixture's `sum` to `my_total` compiles and runs clean on both
  backends. No issue to file; the hazard is already documented.
