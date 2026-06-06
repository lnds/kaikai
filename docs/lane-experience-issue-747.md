# Lane experience — issue #747: LLVM backend Perceus RC (the 3.7 GB leak)

## Scope as planned vs. as shipped

**Planned (per the issue):** the LLVM rb-tree benchmark leaked `tag=int`
objects (3.7 GB RSS, ~5.5× C wall). The issue framed it as two gaps:
(1) "emit decref for out-of-cache boxed Int" and (2) "port Lane B
i64-inline so Int rides raw i64". A targeted Int-decref fix plus the
representation port.

**Shipped — much larger root cause than the issue described.** The
`tag=int` leak was a *symptom*. `KAI_TRACE_RC` on the LLVM binary showed
`tag=variant allocs=1076800 frees=0` — the LLVM backend **never
implemented the Perceus match RC discipline at all**. It leaked every
scrutinee and every boxed slot in every match, in every program. The
honesty target's "garbage-free" claim held only for the C backend; the
LLVM backend was a naive never-free path. The issue's `tag=int` framing
was an artifact of which sites the reporter grepped.

The lane ported the C backend's full match RC layer to the LLVM emitter:

1. **kind-1 Int binders read raw** (`kaix_to_int` once, no `kaix_int`
   box, no `__perceus_dup`+`kaix_to_int` leak). Honours the unbox pass's
   existing `seed_variant_int_binders` MUnboxed seeding via a new
   `LRaw` push in the bind walk. `kaix_variant_arg` → `kaix_to_int` →
   `llvm_push_raw_local`.
2. **`if` int-order-comparison fast path** (`llvm_expr_is_int_order_cmp`)
   — a `k < kx` condition lowers to a raw `icmp` + `br i1`, no
   `kaix_bool`/`kaix_truthy`/`kaix_decref` round-trip. Mirror of emit_c's
   `expr_is_int_order_cmp` (fires even when `.mode` is MBoxed).
3. **`__perceus_dup`/`__perceus_borrow` over a raw-local in raw context
   is a MOVE** — `llvm_emit_expr_raw` reads the inner LRaw directly
   instead of re-boxing then unboxing.
4. **`kaix_variant_arg` switched from incref to BORROW** (runtime_llvm.c).
   This was the load-bearing decision (see below).
5. **Survivor incref at bind** — a kind-0 pointer-slot named PBind takes
   a fresh `kaix_incref` to outlive the match-exit drop cascade.
6. **Match-exit scrutinee drop** — owned scrutinee `kaix_internal_drop`
   after the phi (skipped for `__perceus_borrow`); plus a TRMC-step drop
   (`__pcs_scr_drop`) so the tail-recursive-modulo-cons loop reclaims the
   old subtree-root node it abandons each iteration.
7. **Two-pass pattern lowering** — `llvm_emit_pattern_test` (borrows
   only, may fail) then `llvm_emit_pattern_binds` (binds + survivor
   increfs, on the taken arm only). This was the final fix.

Result: rb-tree LLVM is **garbage-free** (`frees == allocs - live tree`),
RSS **3.62 GB → 205 MB** (18×), ASan clean, selfhost byte-identical,
C == LLVM parity, output unchanged (size/height correct).

## Design decisions & alternatives considered

### The convention question: borrow vs uniform-ownership (the crux)

`kaix_variant_arg` originally **incref'd** every slot read ("LLVM keeps
ownership uniform" — runtime_llvm.c comment). The C backend uses
**borrow** (`_scr->slot[i].ptr`, no incref) + bind-site incref of
survivors + match-exit decref of the scrutinee. Two candidate models:

- **Model 1 (keep uniform-ownership):** every read is +1; the emitter
  drops the scrutinee + every un-consumed read. REJECTED. It forces
  *per-destination* incref accounting the bind-site cannot do: a
  ctor-bound survivor needs +1 over the match-exit cascade, but a TRMC
  recursive-child that `br`s to the loop before the cascade needs a bare
  move. The IR proved it: with Model 1 the recursive `l` got +2 and
  leaked +1 every iteration. Uniform-ownership conflates "read gives
  ownership" with "the cascade also decrefs the slot."
- **Model 2 (switch to borrow, mirror C):** ADOPTED. One rule — a binder
  increfs iff it survives its arm's match-exit drop — is
  destination-agnostic and identical to the C backend. Blast radius was
  only ~7 `kaix_variant_arg` call-sites, and *nothing correct depended on
  the old incref* because the LLVM backend leaked everything (no balanced
  binary existed to break). Perceus shipped correct RC first; reuse is a
  separable layer on top — you cannot make reuse sound over RC that
  leaks. Model 2 *enables* the eventual reuse-in-place perf lane.

### Reuse-in-place deferred (the remaining 3.5× perf gap)

The C backend recovers perf with coupled arm-top reuse
(`kai_drop_reuse_token` / `kai_variant_at` / `kai_reuse_free`). The LLVM
TRMC stays **fresh-alloc + drop** (the C v1 shape) — leak-free but
alloc/free per node instead of in-place rewrite. That, plus the LLVM
path linking the older `stage0/runtime.h` (no slab) rather than
`stage2/runtime.h`, is the residual 3.5× wall / RSS gap vs C. It is a
**Tier-2 perf** follow-up, orthogonal to this Tier-1 correctness fix, and
is explicitly out of scope here (the issue's gap #2 perf target).

## Structural surprises the brief did not anticipate

- **Two runtime.h files.** `stage0/runtime.h` (heap-everything, used by
  `runtime_llvm.c` and the LLVM path) vs `stage2/runtime.h` (Koka slab +
  reuse, used by the C backend). The LLVM backend never got the slab
  work — part of the perf gap, not the leak.
- **The Perceus pass models ownership semantically but does not emit the
  consume code.** `__perceus_dup`/`__perceus_drop` markers cover only
  ordinary multi-use reads / dead bindings. "The match consumes its
  scrutinee" and "the ctor steals its slots" are *implicit* — each
  emitter must materialise them. The C emitter owned this layer; the
  LLVM emitter never did. (perceus.kai:2757-2809 documents the assumed
  convention; the markers don't carry it.)
- **The real leak was a stranded incref on a mid-pattern fail edge.**
  Once Models 1/2 were settled, the residual leak fired ONLY with two
  nested-pattern arms in one function (balance_left/right, nesting a
  sub-node in slot 1 AND slot 4). The single-pass test-and-bind emitted a
  survivor incref *before* a nested sub-test that could `br` to the next
  arm — stranding the +1. The C emitter avoids this structurally:
  test-first (borrows), bind-after (increfs). The two-pass split is the
  fix; bisecting micro-tests (m2–m11) localised it.

## Fixtures added & coverage gaps

- `examples/perceus/llvm_rc_nested_match.kai` + `.out.expected` — a
  self-contained rb-tree insert exercising all of: kind-1 raw binders,
  int-order-cmp `if`, TRMC modulo-cons, balance rotations with nested
  patterns in slot 1 AND slot 4, and a recolor-wrapper match.
- `stage2/Makefile` `test-issue-747` — wired into `.PHONY`,
  `TEST_LIGHT_TARGETS`, `test-fast`. Gates: LLVM output == C output ==
  golden, AND under `KAI_TRACE_RC` the LIVE variant count ≈ the final
  tree size (not the alloc churn). Pre-fix would report `live ≈ 392972`;
  the gate fails at `live ≥ 40000`.

**Found in code review (linus) and fixed before close:**
- **Record-field TEST pass leaked one ref per field on a failing arm.**
  `llvm_emit_pat_record_fields_test` used `@kaix_field` (which increfs);
  a `PRecord`/`PVariantRecord` arm that failed mid-test stranded the +1
  — the same stranded-ref bug the PVariant split fixed, but for records.
  Fixed: the test pass now uses `@kaix_field_borrow` (mirror of emit_c's
  `kai_op_field_borrow` in its test pass) and skips PWild/PBind fields
  (no test needed); the bind pass uses `field_borrow` + one incref for
  survivors. Verified: `mrec.kai` (record match with a failing arm) frees
  all 10000 records (`tag=record frees=10000`).
- **kind-2 Real binders dropped by the split** — the two-pass split
  initially only bound kind-0/kind-1 slots; a `JReal(r)` Real-payload
  binder fell through and was never bound (garbage output). Caught by the
  `json_real_scientific` backend-parity fixture. Fixed: `bind_survivor`
  is `slot_kind != 1` (every boxed-pointer slot — kind-0/2/3 — is a
  survivor; only kind-1 raw Int is the LRaw exception).

**Coverage gaps (follow-ups):**
- **Guard + survivor incref:** an arm whose guard fails *after* the bind
  pass leaks the survivor increfs (the bind runs before the guard, which
  can `br fail`). No guards in the rb-tree path; narrow. Should move the
  bind after the guard, or drop survivors on the guard-fail edge.
- **Pre-existing LLVM arg-pass leak (NOT a #747 regression).** A variant
  passed owned to a fn that destructures and DISCARDS it after extracting
  only a scalar (`fn use_box(b) = match b { Wrap(n) -> n ... }`, `b` owned,
  not reconstructed) still leaks the `Wrap` cell. Confirmed identical on
  pristine `origin/main` (the LLVM never-free baseline) — #747's match
  fix reduces it everywhere the scrutinee is reconstructed (rb-tree,
  balance) but does not close this discard-after-scalar-extract shape.
  RSS grows with N (2.4 GB at 50 M iters). It is an LLVM argument-passing
  / scrutinee-ownership gap orthogonal to the match RC layer; the rb-tree
  is garbage-free because its scrutinees are reconstructed, and `is_red`
  is borrow (not owned). Worth its own issue; out of this lane's scope.
- **Top-level bare PBind scrutinee** (`match x { y -> y }`): binds the
  scrutinee directly (no incref) and the match-exit drop consumes it;
  relies on perceus dup'ing surviving reads. Covered by tier0/parity but
  not unit-isolated.
- **Mid-pattern fail in list/record** still reads slots that may strand
  refs on the fail edge; lists/records kept the legacy single-pass shape
  (the split is PVariant-only) because they are not in the leak repro and
  `kaix_cons_head/tail` keep their incref convention.

## Real cost vs estimate

Far larger than the issue implied. The issue read as a localised Int
fix; it was a full Perceus-RC port to a backend that never had it,
gated through four architecture consults (borrow vs uniform, match-exit
soundness, ctor-arg convention, the two-pass split) and ~10 micro-test
bisections to localise the stranded-incref. The decisive moves: reading
`kaix_variant_arg`'s actual incref (refuting the "mirror C borrow"
premise once), then the IR-level RC count on `balL` that pinned the
stranded incref on the fail edge.

## Follow-ups left for next lanes

1. **Reuse-in-place for the LLVM TRMC** (coupled arm-top reuse) — the
   Tier-2 perf lane that closes the 3.5× gap. Now unblocked (RC is
   balanced; reuse is a layer on top).
2. **Point the LLVM path at `stage2/runtime.h`** (slab + reuse helpers)
   instead of `stage0/runtime.h`, or port the slab allocator — the other
   half of the perf gap.
3. **Guard-fail / list-fail stranded-incref** — extend the two-pass
   split to lists/records and move the bind after the guard.
4. The issue's **C-side `kai_slab_alloc` under `-DKAI_TRACE_RC=1`
   side-note is stale** — in current `stage2/runtime.h` the definition
   (1663) precedes the uses (1791/1852); no forward-reference, compiles
   clean. No action needed.
