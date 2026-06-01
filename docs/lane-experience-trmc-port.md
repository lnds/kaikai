# Lane experience: TRMC port from Koka (2026-05-31 → 06-01)

Goal: take the canonical Perceus rb-tree (1M inserts) toward ~1×C by
porting Koka's TRMC (Tail-Recursion-Modulo-Cons) + constructor-context
machinery shamelessly, rather than patching kaikai's existing reuse
recogniser. Koka runs the same algorithm at 0.28×C.

## Scope as planned vs as shipped

Planned (from the study workflow + asu pipeline-placement consult):
PR #1 = Steps 0–3 (reshape source → cctx runtime → TRMC recognition →
TRMC emission), expected to land ~6–8×C; Int-unbox + specialized struct
(Step 5) deferred as the lane that crosses ~1×C.

Shipped: Steps 0–3 complete and **mechanically correct** — the rb-tree
produces a correct tree (size=1M, height=29) with O(1) C-stack on the
modulo-cons descent, and the emitted C is a line-for-line mirror of
Koka's `rbtree__koka.c:267-344` (cctx threading, goto-loop, build-with-
hole, `kai_cctx_extend_linear` / `kai_cctx_apply_linear`). selfhost is
byte-identical (the compiler has no modulo-cons fns, so its emission is
unaffected; the lone `_trmc_node` string in build/stage2.c is the
emitter's own source literal, not a transformed fn). tier0 green.

**The wall-time did NOT move** (~2.07–2.24s, ~8.4–9×C, vs baseline
2.04s). This is the honest headline.

## The decisive measured finding

The spine TRMC is necessary but not sufficient. At 100k inserts the
RC trace shows `alloc_total≈7.4M`, of which the **top three sites are
all `tag=int`** (1.15M + 1.09M + 0.42M ≈ 35% in the top 3 alone; more
int sites below). Keys/values are boxed `KaiValue*` because
`insert_loop(t, k: Int, v: Int)` is a MIXED signature — `t` is boxed,
so the whole fn stays boxed and every level re-boxes the key/value via
`kai_int` / `kai_take_int`. Koka stores `key: int32_t` inline in the
node and never RC-touches it.

So the spine-allocation win that TRMC delivers attacks the ~40% variant
allocs, while the ~60% Int allocs + their RC traffic (incref 1.42M vs
decref 1.10M at 100k) dominate the wall. This is exactly what the study
workflow's architecture verdict predicted: **reaching ~1×C MANDATES
specialized structs with Int inline (Step 5), not TRMC alone.**

## Design decisions and alternatives considered

- **Pipeline placement (asu consult).** TRMC recognition+rewrite runs
  AFTER perceus (as a C-emitter pre-pass, like the existing tcrec),
  NOT before (like Koka's Core.CTail). Koka's Parc is cctx-aware by
  construction; kaikai's tcrec is a string-sentinel rewriter in the
  emitter. Going-before would mean rewriting the RC core and reopening
  C↔LLVM coherence. Verdict held up: the rewrite slots cleanly into
  `tcrec_rewrite_decl` / `tcrec_rewrite_pcs_ret_wrap`.

- **Single-fn-with-entry-label, not Koka's 2-fn wrapper.** Koka emits a
  `trmc_ins` taking the cctx + a wrapper seeding `cctx_empty`. kaikai
  declares `KaiCctx _kai_acc = kai_cctx_empty();` before the goto entry
  label — the single-fn-with-entry-label gets the wrapper's one-time
  seed for free. Caller signatures unchanged.

- **Sentinel wire-format.** Reused the `__kai_tcrec|...` sentinel pattern:
  `__kai_trmc|<sym>|<holeslot>|<cname>|<dropmask>|<p0>|...` for a
  modulo-cons step, `__kai_trmc_apply` for a non-modulo-cons tail leaf.
  The emitter (`emit_trmc_step` / the apply lowering) resolves ctor
  tag/mask from `cname` against `cx.variants`.

- **Reuse-token donate: implemented, then reverted.** The donate
  (`kai_variant_at(kai_check_unique(_scr) ? kai_ptr_reuse(_scr) : null,
  ...)`) is the piece that should collapse spine allocs. It fired only
  ~0.6×/insert (uniqueness mostly false through the `rb_insert` re-match
  layer) and left an RC imbalance (incref≠decref → potential UAF if the
  tree were freed). Reverted to fresh-alloc (`kai_variant_u`) for the
  consolidated checkpoint: the fresh path leaks the consumed spine cell
  (benign — lost cell, not UAF) but is correct and selfhost-clean.

## Structural surprises the brief did not anticipate

1. **Two layers of Perceus wraps, not one.** `insert_loop` post-perceus
   is `EBlock([SLet(__pcs_ret, INNER), exit_drops], Some(__pcs_ret))`
   (the fn-wrap, handled by `tcrec_is_pcs_ret_wrap`), AND each match arm
   is independently wrapped in `__pcs_arm_ret`. The modulo-cons ctor is
   buried under the arm-wrap; `trmc_unwrap_ret` peels both `__pcs_arm_ret`
   and `__pcs_block_ret` to reach it. The brief assumed the tcrec walker
   would see the ctor directly.

2. **`pcs_is_self_tail_call` carve-out only covers DIRECT tail-calls.**
   The arm-drop pass declines to wrap arms whose body is a direct
   self-tail-call (so tcrec can see it), but modulo-cons ctors DO get
   wrapped — the carve-out predicate doesn't recognise them. Worked
   around in the TRMC path (unwrap) rather than extending the perceus
   carve-out, to avoid regressing non-TRMC fns.

3. **The nested-subpattern / diagonal-reuse bug blocked compilation.**
   `balance_left`/`balance_right` have nested-variant arm patterns
   (`RBNode(_, RBNode(Red, lx,...), ...)`) which the reuse recogniser
   mis-recognised as 1:1-reusable, emitting C that references undeclared
   `kai_lx`. Fixed by cherry-picking `fix/reuse-diagonal-guard` (717c9b0:
   `pcs_all_flat_subs` + `pcs_reuse_is_diagonal`) — the genuinely useful
   correctness work from the prior session.

## Fixtures added

- `examples/perceus/reuse_diagonal_guard.kai` + `.out.expected`
- `examples/perceus/reuse_nested_subpattern.kai` + `.out.expected`
  (both from the cherry-picked diagonal-guard commit; ASAN leak-clean)

Coverage gap: no fixture yet exercises a minimal modulo-cons TRMC fn
with a leaked==0 gate, because the donate is reverted (fresh-alloc
leaks the spine cell). That fixture lands with the donate fix.

## Mixed-signature Int unboxing — implemented end-to-end, parked in stash

The session went on to attack the Int-boxing floor directly (the lever
the diagnosis named). The mixed-signature unboxing — let a fn with some
boxed and some unboxable params pass the unboxable ones as native i64,
exactly Koka's `trmc_ins(tree t, int32_t k, bool v)` ABI — was
implemented across every touch point and **compiles the rb-tree
correctly** (`insert_loop` emits `KaiValue* kai_t, int64_t kair_k,
int64_t kair_v`, size=1M/height=29). The work is in `git stash@{0}` on
this branch ("WIP mixed-signature Int unboxing"). What landed there:

- `fnreg.kai classify_unbox_sig`: accept mixed (`some_param_unboxable` +
  no lambda), record per-param Ty in `US`. Crucial unblock: the
  `body_is_effectful` `EModCall(_, name) -> true` arm was a false
  positive for qualified constructors (`RBTree.RBNode`); split by casing
  (effect ops lowercase, ctors uppercase) — without it every pure fn with
  a qualified ctor was disqualified. Excluded `__proto_*` / `__pimpl_*`
  (protocol glue uses fixed boxed fn-pointer casts a raw param mismatches).
- `unbox.kai ufn_param_env`: a boxed param stays MBoxed (reads `kai_<n>`),
  an unboxable param is MUnboxed (reads `kair_<n>`).
- `emit_c.kai`: `emit_param_list_unboxed_each` (kai_/kair_ per param),
  `raw_c_type` → `KaiValue *` for boxed, `emit_call_args_mixed` (per-arg
  marshalling at call sites — both boxed-context and raw-context UFn
  calls), `emit_fn_thunk` (per-param unbox + boxed-vs-raw return), the
  TCO goto raw path (`tcrec_emit_drops_mixed` / `tcrec_emit_rebinds_raw`
  with per-param prefix + boxed-return dead-tail), `emit_trmc_step`
  rebind, and the boxed-return body emit in `emit_fn_body`.

**Why it is parked, not shipped (two honest blockers):**

1. **Selfhost breaks at runtime.** The emitted C compiles, but `kaic2b`
   (the compiler compiled by the mixed-sig kaic2) crashes compiling its
   own source with `field access on non-record`. The global mixed-sig
   reclassified many compiler fns; one ABI path is uncovered — a boxed
   param reaching a `.field` access where the new raw ABI passed a
   scalar (or vice-versa). Needs the offending fn bisected; likely one
   more call/return path (closure capture? higher-order?) to marshal, or
   a tighter eligibility gate that excludes the compiler's own shapes.

2. **The wall did not move even on the rb-tree (~2.1s, ~8.5×C).**
   Signature unboxing ALONE is inert on the clock: `insert_loop`'s body
   still emits `kai_op_lt(kai_int(slots[2].i64), boxed_k)` because the
   match binder `kx` (a variant Int SLOT) is still read as
   `kai_int(slots[2].i64)` (boxed) — `emit_pat_binds_variant` (emit_c.kai
   ~2687) re-boxes every Int slot, so `decide_mode_aware`'s `op_is_raw_cmp`
   path (unbox.kai:503) sees a boxed operand and keeps `<` boxed. The
   COMPLEMENTARY change — **variant Int-slot unbox-on-read** (read
   `slots[2].i64` as a raw `int64_t kair_kx`, mark the binder MUnboxed) —
   is required for the compare to lower to native C `<` and the
   `kai_int`/`kai_op_lt`/`kai_bool` churn to vanish. Per project memory
   [[project_kaikai_1b1_native_bind_unbox_one_change]] and
   [[project_kaikai_variant_int_field_unbox_lane]]: native-bind +
   unbox-on-read must be ONE change (bind-site alone regressed +60%
   historically). Signature unboxing is now the prerequisite that memory
   named; the slot-read is the second half, gated on `leaked==0` with
   random keys outside the small-int cache.

Next-lane order: (a) bisect the selfhost field-access crash (or narrow
eligibility to non-compiler shapes), (b) land variant Int-slot
unbox-on-read so the compare goes raw, (c) measure — only then does the
wall move toward Koka's 0.28×C.

## Follow-ups for the next lane (the real path to 1×C)

1. **Int-in-slot unboxing of RBNode key/value (the dominant lever).**
   Land `KAI_VAR_SLOT_INT` for the key/value slots so they never box —
   removes ~60% of allocs and the bulk of the RC traffic. This is what
   closes the wall-time gap; TRMC is inert on the clock without it.
   Prerequisite: mixed-signature unboxing (`insert_loop(k: Int, v: Int)`
   passes k/v as native i64) — large ABI lane, see
   [[project_kaikai_mixed_param_unboxing_perf]].

2. **Fix the donate RC balance.** The reuse-token donate needs the
   match-extract reconciled with the cctx ownership transfer (Koka
   borrows children on the unique path; kaikai's match always increfs).
   Gate: `leaked==0` on random keys + ASAN. This collapses the variant
   spine allocs (the other ~40%).

3. **Specialized 1-malloc node (FAM / per-ctor struct).** kaikai's
   uniform KaiValue + separate slots array = 2 mallocs/node vs Koka's 1.
   The residual ~2× allocator + cache tax. Largest lane.

asu's estimate stands: TRMC + donate + int ≈ 3–4×C on the current
representation; FAM closes to ~1.5–2×C; full ~1×C needs the specialized
monomorphic representation. 0.28×C (Koka) is not a v1 target.

## Real cost vs estimate

Estimate (study workflow): PR #1 = Steps 0–3, ~6–8×C. Actual: Steps 0–3
landed and are correct, but ~8.4–9×C because the Int-boxing floor was
under-weighted in the "expected landing" column (the table credited
TRMC with removing return-unwind allocs, but those were already cheap;
the spine rebuild allocs it removes are dwarfed by Int boxing). The
study's own architecture verdict (§1, "the Int ceiling") had this right;
the per-step landing estimates were optimistic. Net: the mechanism is
de-risked and correct; the clock-moving lane is #1 above.
