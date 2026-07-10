# Lane experience — issue #1160: de-opaque %KaiValue in the native emitter

Closes #1160, the last axis of the koka-parity arc (Axis 1 of
`docs/koka-parity-redesign-vision.md`). The native emitter's tag tests,
typed-slot reads and scalar comparisons now lower to direct loads and
inline bit math instead of opaque shim calls. The lane also re-measured
the issue's premise before touching the emitter and found it had moved —
twice — which reshaped both the implementation and the honest closure
below.

## Scope as planned vs as shipped

Planned: four stages — (1) tag test, (2) slot reads, (3) scalar boxing,
(4) stores/construction if the profile says it pays. Shipped:

- **Stage 1 — inline tag test.** `KTagOf` emits the C oracle's
  `scr->variant_tag` read (typed GEP + u16 load + zext), no shim, no null
  branch. The layout module (`emit_native_slot.kai`, which #1155/#1157
  had already seeded with the slot readers) gains the tag reader;
  `runtime_llvm.c` pins the header offsets with `_Static_assert`s, so a
  header change (or a `KAI_TRACE_RC` build of that TU) fails at compile
  time instead of miscompiling every inline read.
- **Stage 2 — static-kind slot reads.** New KIR op `KProjKind(v, i,
  kind)`: the pattern binder's typed-slot read at the compile-time kind
  the single-writer resolver (`slot_kind_of`) already produces. The
  native emitter lowers it to one raw slot-word load plus the kind's
  minimal re-box — tagged Int mint, fresh Real box, or the interned enum
  singleton via the new `kaix_enum_slot_box` shim — replacing
  `kaix_variant_arg`'s mask-table lookup. The mixed-writer sentinel stays
  dynamic `KProj`. This also closed #1157's follow-up 2 (enum binder
  reads through the dynamic shim).
- **Stage 3 — scalar comparisons, not scalar boxing.** The issue's stage
  3 (`kaix_int`/`kaix_bool` → inline bit math) was measured to be already
  delivered: the bc-linked shims inline fully, and the binary census
  shows zero surviving `kaix_int` calls — emitting the bit math from the
  emitter would change nothing (see "The premise moved"). What the
  profile did show out-of-line is `kai_op_lt`/`kai_op_gt` (~8% of native
  wall). The shipped stage 3 is the tagged-Int fast path INLINE in the
  six comparison shims (`kaix_eq/ne/lt/gt/le/ge`): two immediates compare
  as one signed word compare (the n*2+1 encoding is monotonic), the
  heap/mixed shapes keep the generic path. Measured: `kai_op_lt/gt`
  vanish from the profile entirely.
- **Stage 4 — declined for stores, taken for the call-arg verdict.**
  Asm inspection shows construction ALREADY folds to `kai_alloc` + direct
  header/slot stores (SROA eliminates the slot-word buffer; the "frozen
  vectorized copy-loop" of the #1083 era is gone post-target-features +
  #1155). An emitter-side store path would be a no-op; not shipped, per
  the staging gate. The measured residual instead was the boxed verdict
  on a binder whose only escaping use is a raw-param call arg (#1156
  retro follow-up 2, the rb-tree Black-arm `kx`): `native_arm_binder_raw_safe`
  now models a direct arg of a direct call whose callee param is raw,
  resolved through the same UFnSig mask the call lowering reads.

## The premise moved (root-cause-first, twice)

1. **"An opaque call per access" is no longer what the binary shows.**
   On current main the bc-linked shims DO inline (post-#1083
   target-features): zero `kaix_*` calls survive in the rb-tree hot loop.
   The cost that remained was the inlined GENERIC bodies — chiefly
   `kai_slot_mask_of(tag)`, a load from a startup-populated table that
   LLVM can never constant-fold, plus duplicated cold fallbacks. The
   redesign still applies (the KIR knows the kind statically; LLVM
   cannot), but the win mechanism is fold-visibility, not call removal.
   Reported on the issue before implementation.
2. **"RC matches across backends" is false on current main.** Under
   `KAI_TRACE_RC` @1M inserts: C backend 1.00M variant allocs vs native
   6.30M, incref 2.0M vs 8.3M, with identical `reuse_in_place` (6.30M)
   and identical live peak. Root cause: the C emitter passes a
   `KaiReuse _donor` PARAMETER into `balance_left`/`balance_right`
   (cross-call arm-token donation, the #1104 lever); the KIR/native path
   does not model cross-call donation, so every balance rebuild allocates
   fresh and frees the donor. That is Perceus/token machinery — expressly
   outside this lane's charter — and is filed as its own issue with the
   counter evidence. The #1104 donation fixture did not catch it because
   its native variant exercises the in-arm shape, not the cross-call one.

## Measured (rb-tree fill @1M, macOS arm64, interleaved)

| | wall (median) | vs C | instructions |
|---|---|---|---|
| Koka | 0.29s | 0.79× | — |
| kaikai C backend | 0.36s | 1.00× | 2.02G |
| native at lane start | 0.52s | ~1.43× | 5.90G |
| native + stages 1–4 | 0.46s | **~1.28×** | 4.06G |

Per stage (interleaved pairs): stage 1 ≈ −5%, stage 2 ≈ −4%, stage 3 ≈
−7%, call-arg verdict ≈ −4%. Instructions −31% overall. The hot loop
census: no surviving accessor/compare shims, no mask-table loads; the
survivors are allocation (`kai_alloc`/`kai_slab_alloc`), `kai_variant_u`
cold fallbacks and `kai_free_value` — all genuinely out-of-line runtime.

**The 1.2× perf gate is NOT met, and the reason is measured, not
guessed:** the remaining ~0.08–0.10s is dominated by the missing
cross-call donation (5.3M extra alloc+free pairs and 6.3M extra RC ops
per 1M inserts — the RC-parity premise of the gate does not hold on
main). Within the surface this lane is allowed to touch (no typer, no
Perceus, no representation), the de-opaque axis is complete: every
staged item is either shipped or declined with binary evidence.

## Structural surprises

1. **The layout module already existed.** #1155/#1157 had seeded
   `emit_native_slot.kai` with the typed header struct and slot readers;
   this lane grew it instead of creating a new module. The "no magic
   offsets" rule was already institutionalised there — the one gap was
   the runtime-side static assert, now in `runtime_llvm.c`.
2. **The kaic1 handle-RC clobber bites across `if` joins.** The first
   `nemit_projkind_boxed` threaded an LLVM handle through an if/else
   join and kaic1's type-blind RC corrupted the instruction (the module
   verifier crashed on a mangled opcode). The fix is the established
   pattern: one flat frame per kind, every handle `let`-bound in the
   frame that consumes it. The LLVM-IR dump hook (`KAI_NATIVE_DUMP_IR`)
   plus `opt -passes=verify` located it in minutes.
3. **Inlining hides where cost lives.** Static call censuses inflate
   (inlining duplicates cold paths — 92 `kai_alloc` sites in one
   function); sample profiles collapse everything into `insert_loop`
   self-time. The measurements that actually discriminated: dynamic
   instructions retired (`/usr/bin/time -l`), the RC counters under
   `KAI_TRACE_RC`, and reading the optimized asm at specific call sites.
4. **The C oracle's reuse ABI is wider than the KIR's.** Discovering
   `KaiReuse _donor` in the C-emitted `balance_left` signature was the
   single most informative diff of the lane: the two backends do not
   share a reuse ABI, only a reuse PLAN — the KIR consumes less of it.

## Fixtures added (wired)

`examples/perceus/{variant_tag_inline,variant_projkind_read,
tagged_compare,binder_raw_call_arg}_1160.kai` — one per stage, golden and
byte-equal on both backends: inline tag reads over payload variants /
nested scrutinees / cons cells / enums; static-kind boxed reads including
the >2^62 heap-Int mint; the tagged-compare fast path plus every generic
slow path (heap/mixed Ints, String, Real, structural variant equality);
the raw call-arg binder with the #1156 sibling-arm widening shape.
Targets `test-perceus-1160-de-opaque{,-native}`, wired into
tier1-native.yml (not TEST_LIGHT_TARGETS).

## Gates run

tier0; native selfhost gate (COMPILE 0 subset-gaps + LINK + RUN +
SELF-COMPILE byte-identical C emit); the new fixture family on both
backends; `test-kir` (goldens regenerated for `projk`); the #1104
counter fixtures exact (`alloc_total=100015`, `incref+decref=200000`);
the #1136/#1156/#1157 fixture families; full-corpus SERIAL backend
parity (`BACKEND_PARITY_JOBS=1`); modular selfhost (`KAI_BACKEND=c`).
`km`: `emit_native_slot.kai` A++; `unbox_native_raw.kai` B→B (81.8→80.8,
edited pre-existing file, floor kept); cogcom avg 3.9 / max 14.

## Cost vs estimate

The mechanism work (four stages) was close to plan. The unplanned cost
was diagnostic and paid off twice: proving stage 3's briefed form was
already delivered (which redirected the effort to the comparison shims,
the actual out-of-line residual), and root-causing the missed perf gate
to the cross-call donation gap with counter + signature evidence instead
of shipping speculative emitter work that could not have closed it.

## Follow-ups (filed / documented)

1. **Cross-call reuse-token donation on the KIR/native path** — the
   dominant remaining native-vs-C driver (6.3× variant allocs on the
   rb-tree; C's `balance_*` take a `KaiReuse _donor` param, the KIR has
   no such ABI). Filed with the repro and counters; it is the #1104
   family's next lane and the credible path from ~1.28× to near-C.
2. Cons-cell `KProj` head/tail reads still take the dynamic shim; they
   are statically pointer-shaped and could ride `KProjBorrow` (list-heavy
   workloads, not rb-tree).
3. `EModCall` callees are not modelled by the call-arg raw verdict (bare
   `EVar` heads only) — same one-verdict discipline, one more shape.
4. The issue's instruction-count attribution style ("call/ret overhead")
   should be retired in future briefs for this backend: with bc-link
   inlining, opacity costs manifest as non-foldable table loads and
   fallback bloat, not calls.
