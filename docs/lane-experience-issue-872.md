# Lane experience — issue #872 (native variant reuse leak)

## Scope as planned vs as shipped

**Planned (brief):** port the C oracle's two-branch `kai_check_unique` reuse
emission (`emit_match_arm_reuse_variant`) — the non-unique branch's per-child
incref — into the native KIR lowering, then delete #871's scrutinee-dup gate.
One bug, one mechanism: "shared-donor reuse arm skips per-child incref".

**Shipped:** the leak turned out to be **three distinct mechanisms**, not one.
The brief named only the first. Closing the issue required all three:

1. **Variant rotation reuse arm** (the brief's bug). On a SHARED donor the flat
   `KConReuse` fresh-allocs and keeps borrowed references to the kept children
   without the per-child incref. Fixed by `kai_incref_if_shared(donor, child)`
   emitted per embedded use (multiset). For the NESTED (non-bijective) rotation
   rebuild (`Node(_, Node(Red, lx,..),..) -> Node(.., Node(.., lx,..),..)`)
   native does NOT reuse the inner cell (the C oracle's Koka 2-of-3 token reuse
   was never ported), so it fresh-allocs the whole outer (`KCon`, not
   `KConReuse`) and `dup`s each embedded borrowed leaf unconditionally — the
   match-exit `kai_decref(donor)` then cascades correctly.

2. **TRMC variant modulo-cons** (`Node(c, insert_loop(l,..),..)`). The native
   `ntrmc_variant_step` ignored the consumed scrutinee entirely (only the cons
   `_s` step dropped it) — so every descent level leaked. Insert-only alone
   leaked ~10× over-alloc. Fixed by capturing the scrutinee as an arm-top reuse
   token and freeing the shell in the step.

3. **Arm-top reuse token, and the `KTrmcApply` terminal RET** — the real, deep
   half. An arm with a non-TRMC tail (`balance_left(ins(l,..),..)`) OR a variant
   con-reuse tail (`Node(Red, ins(l,..),..)`) ends in `KTrmcApply`, which is a
   **terminal RET**: it does NOT flow to the match's L0 continuation where
   `match_finish` plants `KArmTokenFree` and the owned-scrutinee `KDrop`. So the
   captured token shell AND the reuse-in-place-increfed scrutinee leaked once
   per recursion level. Fixed by (a) freeing `_arm_ru` in `nemit_trmc_apply`
   before the RET, and (b) dropping `st.mscr` in `lower_trmc_apply` when the leaf
   is a `__perceus_reuse_variant` of the scrutinee.

## Design decisions and alternatives considered

- **Opción C (compile-time mask + runtime conditional) over Opción B (replicate
  the oracle's two-branch state machine in IR).** asu's call: `emit_match_arm_
  reuse_variant` is not "two branches", it's a state machine (borrow-pure /
  owned / old-slot-drops / the second Koka token) that already shipped one
  silent soundness bug when a text needle stopped matching. Duplicating it = two
  sources of truth for the reuse protocol. The native model already branches
  unique/shared in the runtime helper; only the *information* of which args to
  incref was missing — so the fix moves that to compile time via
  `kai_incref_if_shared` / `kai_incref_if_token_null`, one runtime branch.

- **Multiset, not set.** A kept binder embedded twice (`Node(Black, lx,.., lx)`)
  needs two increfs; a set under-increfs and reintroduces the UAF the #871 dup
  masked. Counting USES in the rebuild (not binds in the pattern) gives the
  multiset for free.

- **`reuse_gc_uses` must NOT recurse into NON-ctor calls.** A binder passed to a
  fn call (`ins(l,..)`) is CONSUMED, not embedded — counting it produces a
  spurious incref. Gate the recursion on `kir_name_is_ctor`.

- **`kaix_variant_at` (reuse-in-place of the token) was abandoned.** It HANGS
  native codegen for a null token (`build`-shaped step). The TRMC variant step
  instead fresh-allocs + frees the token shell — correctness without reuse-in-
  place. The ~4× over-alloc that remains is a separate perf follow-up.

## Structural surprises the brief did not anticipate

- The brief's "one bug" was three. The TRMC variant path and the arm-top token
  are entirely separate reuse mechanisms the native backend never grew; the
  rotation arm (the brief's target) is only the smallest of the three.
- `KTrmcApply` being a terminal RET that skips the match continuation is the
  load-bearing surprise — every disposal planted in the L0 join is invisible to
  a reuse tail that ends in a TRMC apply. Nailed empirically with temporary
  `fprintf` in the `kaix_drop_reuse_token`/`kaix_reuse_free` forwarders
  (3 SHELL captures, 0 frees) after lldb/sample gave no usable backtrace on the
  hang-vs-deref ambiguity.

## Fixtures added

- `examples/perceus/native_variant_reuse_leak_872.kai` (+golden) — the full
  rb-tree shape (rotation + TRMC step + arm-top token + con-reuse recolor),
  bounded; gate `test-perceus-872-native-variant-reuse-leak` asserts native
  `leaked < 100` under `KAI_TRACE_RC` (it is 4), wired into tier1-native.
- `examples/perceus/native_variant_reuse_double_use_872.kai` (+golden) — the
  multiset gate: a kept grand-child embedded twice in the rebuild.
- Both added to `PERCEUS_NESTED_REUSE_FIXTURES` (output + ASAN, C-direct).

## Coverage gaps

- The ASAN gate runs the C-direct oracle, not the native lowering (libLLVM is
  in-process, not `-fsanitize`-able). Native soundness is gated by: output
  parity, `KAI_TRACE_RC` balanced (`leaked` a small constant), selfhost byte-id,
  and the serial backend-parity ratchet. No native ASAN.
- Token reuse-in-place is NOT done (the `kaix_variant_at` hang). Native still
  over-allocs ~4× vs C on rb-tree — a perf follow-up, not a leak.

## Measured result

- `rbtree_corpus` (20K inserts, reduced from 2M for host safety): native
  `leaked=7` vs C `leaked=13` — the issue's 57M leak is gone.
- `nested_pattern_reuse_balance`: native `leaked=4`.
- All isolated shapes (rotation / double-use / insert-only / balance / mixed)
  close to `leaked≈4`. selfhost byte-id CLEAN. existing reuse + cons gates green.

## Follow-ups for next lanes

- **Token reuse-in-place** (close the ~4× over-alloc): port the oracle's
  `kai_variant_at` reuse into the TRMC variant step + the con-reuse tail. The
  blocker is the codegen hang on a null token — needs root-causing (likely a
  declaration / IR-shape issue, not the runtime).
- A native ASAN harness (run the in-process backend under a sanitized runtime)
  would gate the UAF class directly instead of via TRACE_RC + selfhost.
