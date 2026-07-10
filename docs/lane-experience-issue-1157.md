# Lane experience — issue #1157: full variant_slot_mask parity on native

Closes #1157 (follow-up 2 of the #1136 retro). The native backend's
variant slot mask was Int-only (`variant_i64_mask`): enum and Real slots
stayed physically boxed on native while the C backend laid them
immediate/raw. This lane migrates native to the full `variant_slot_mask`
— construction, the match reads, the reuse/TRMC rebuilds and the startup
registration together, per the single-writer-layout doctrine. The lever
is representation parity and consistency, not wall time, and the honest
measurement below confirms that framing.

## Scope as planned vs as shipped

Planned: the coupled reshape, all four sides in one lane. Shipped exactly
that, in three commits (runtime readers first, the coupled compiler
reshape second, fixtures third):

- **Runtime readers land before the first writer.** `kaix_variant_arg`
  now routes every non-pointer kind through `kai_variant_slot_box` (the
  boxed-view accessor both backends share), so a kind-3 enum tag word is
  re-interned, never dereferenced. `kaix_take_enum` joins `kaix_take_real`
  as the consuming box→word border. The generic drop/eq walkers and the
  reuse shims (`kai_variant_u_fast`, `kai_variant_reuse_at`,
  `kaix_variant_at_argv_i64`) were already full-mask-aware — they are the
  same stage2/runtime.h code the C backend has exercised for months; the
  Int-only gap lived entirely in the KIR lowering and the shim's read
  guard.
- **One mask, one enum verdict.** `ls_ctor_mask` and
  `nproto_register_payload_ctors` now compute `variant_slot_mask` over the
  program's enum type names; `variant_i64_mask` is deleted. `LowerSt`
  carries `enums` (`collect_enum_type_names`, the same list emit_c's
  `cx.enums` holds), so the write mask, the binder read kind
  (`slot_kind_of`) and the registration resolve enum-ness identically —
  the read-matches-write invariant keeps living in ONE resolver. The
  builtin-tag sentinel (mixed-writer tags stay dynamic) is untouched.
- **Construction at the slot's kind.** `KSlotInit` grows `SIEnum`;
  `typed_inits` (reuse route) and `lower_ctor_args` (fresh route) classify
  Real and enum slots. The emitter's `nemit_slot_word` stores a kind-2
  slot as the raw `f64` (consuming `kaix_take_real`; a raw literal or raw
  `SReal` register skips the box entirely) and a kind-3 slot as the
  immediate tag (`kaix_take_enum`; a literal nullary-ctor arg peepholes to
  the tag constant, the C `{.i64 = tag}` mirror). The TRMC spine step
  routes through the SAME word emitter, so a spine cell lays every slot
  exactly as a fresh cell would. Under opaque pointers the `f64` store
  into the `[n x i64]` buffer needs no bitcast — the union word takes the
  bits directly.
- **Reads at the slot's kind.** The decision tree tests an enum slot as
  one raw `.i64` load + inline `i==` against the ctor tag (the C oracle's
  `emit_enum_slot_test`) and a Real literal slot as one raw `.r` load +
  `f==`. Binder reads stay `SBoxed` (`KProj` re-boxes at the border);
  only Int binders go raw, as before — the unbox-pass seeding and the
  #1156 permit machinery are untouched because kind-1 classification is
  invariant to the enums list.
- **The RC model flip for Real binders.** Under the Int-only mask a Real
  slot was physically a boxed pointer on native: its `KProj` read was a
  BORROW, so `pat_alias_binders` dup'd it. Under the full mask the read
  mints a fresh OWNED box (the C `is_alias=false` model), so
  `pa_slot_needs_dup` drops kind-2 — a structural dup would now leak one
  box per read. Enum binders stay in the dup set: the dup of an immortal
  interned singleton is a no-op, matching the C oracle's alias-shaped
  enum bind. This one-line flip is the entire RC-plan delta; everything
  else is representation, not refcounting.

## Measured

- **RSS parity, Real-heavy.** 2M-node `Node(Color, Real, Cell)` chain:
  native 210,681,856 B peak RSS vs C 210,698,240 B — byte-level layout
  parity (before the lane, each native node carried its Real payload as a
  separate heap box). Wall 0.42 s vs 0.41 s.
- **rb-tree flat, as predicted.** 1M random inserts, native, 24
  interleaved samples base-vs-lane: medians 0.436 s (base) vs 0.443 s
  (lane), mins 0.418 s vs 0.414 s — inside the noise floor. The enum-slot
  test got cheaper (raw load + icmp replaces ptr-load + tag deref) while
  the enum binder read got a shim call (`kaix_variant_arg` + re-intern
  where an inline `.ptr` borrow sat before); the two roughly cancel on
  this bench. The issue predicted "may be flat — primarily a
  parity/consistency lever": confirmed.
- **Counters exact.** `test-perceus-1104-balance-token-donation`
  (`alloc_total=100015`, no `reuse_freed`) and
  `test-perceus-1104-sibling-param-move` (`incref+decref=200000`,
  `alloc_total=100015`) unchanged; the #1136 and #1156 fixture families
  green on both backends.

## Structural surprises

1. **The runtime was already there.** The shared stage2/runtime.h is the
   C backend's runtime, so the drop walker, eq walker, slot-box accessor
   and every reuse helper already understood kinds 2 and 3. The entire
   migration lived in the KIR lowering + two shim seams
   (`kaix_variant_arg`'s read guard, the missing `kaix_take_enum`). The
   "teach the drop walker first" step the plan budgeted was a no-op.
2. **The native-built compiler cannot emit native objects — and never
   could.** The closing smoke (the `--emit=c` self-compile probe is blind
   to native-target miscompiles) surfaced that `kaic2-native --emit=native
   hello.kai` SIGSEGVs in `emit_native_fn__nfn_llvm_type` (EXC_BAD_ACCESS
   at 0x8, a null+8 slot read). Bisected against a clean `origin/main`
   worktree: the baseline crashes at the IDENTICAL frame and offset
   (`nfn_llvm_type +956`, same `ldr [x23, #0x8]`), so the escape is
   PRE-EXISTING and independent of this reshape — the gap has simply
   never been exercised (the gate runs `--version` and `--emit=c` only).
   What this lane's smoke DOES close: `kaic2-native` compiles both a
   hello-world and the lane's own fixture (whose lowering exercises every
   new full-mask path inside the native-built binary) byte-identically to
   the oracle, and the compiled programs run green. The `--emit=native`
   escape is reported on the issue with the repro and both stacks; it
   needs its own lane (the crash sits in the LLVM-API emit path executing
   inside a native-built binary, a different failure class from this
   lane's layout work).
3. **A reuse fixture is a stack fixture.** The first fixture draft ran
   `recolor` (deliberately NOT modulo-cons, to pin the plain `KConReuse`
   route) over the 200k spine — fine under `bin/kai`'s 128 MB
   `-Wl,-stack_size` link flag, a SIGSEGV at the default 8 MB stack. The
   diagnostic detour: the crash reproduced with the ORACLE's own C at
   `-O0` and `-O2`, which is what ruled out a miscompile and pointed at
   the harness. The shipped fixture recolors a 2000-deep chain (any
   stack) and keeps the 200k depth on the TRMC spine, which lowers to a
   loop.
4. **The enum-literal peephole falls out of the KIR.** The C backend
   needed an explicit `nullary_ctor_tag` peephole to store `{.i64 = tag}`
   for a literal `Red` argument. On the KIR side the same peephole is one
   `KIntV(tag)` atom minted in `lower_enum_slot_arg` — the emitter's
   `SIEnum(KIntV)` arm emits the constant word, no singleton load, no
   shim call. Narrow guard (bare `EVar`, not local-shadowed, registered
   arity-0 user ctor); everything else takes the consuming
   `kaix_take_enum` border.

## Fixtures added (wired)

- `examples/perceus/variant_full_mask_1157.kai` — one ctor carrying all
  four slot kinds (pointer / enum / Real / Int): construction, the static
  enum/Real/Int slot tests (a shared-tag match family mixing an enum test
  with Real and Int literal tests), binder reads, unique AND shared
  `KConReuse` rebuilds (flipping the enum word, shifting the Real word),
  a 200k-deep TRMC spine, and folds that read every slot back. Golden on
  both backends.
- Targets `test-perceus-1157-full-mask` (C oracle) and
  `test-perceus-1157-full-mask-native`, wired into tier1-native.yml (not
  TEST_LIGHT_TARGETS, per the lane brief).

## Gates run

tier0; native selfhost gate (COMPILE 0 subset-gaps + LINK + RUN +
SELF-COMPILE byte-identical); the native-built-compiler smoke (hello +
the lane fixture, byte-identical emit, compiled outputs green); modular
selfhost (KAI_BACKEND=c); full-corpus backend parity SERIAL
(BACKEND_PARITY_JOBS=1): pass=597 fail=1 skip=72, the one failure being
`demos/vs/python/main.kai`'s pre-existing C-oracle typer error (the #1156
retro's follow-up 1, reproduced on clean base); the #1104 counter
fixtures exact; the #1136/#1156 fixture families; `test-perceus-enum-slot`,
`test-perceus-747-native-real-slot-uaf` (ASAN), `test-native-static-slot-read`,
`test-native-1110-reuse-int-slot`.

The rc-detector currently FAILS on six `c/…` corpus cases — and fails
IDENTICALLY on a clean `origin/main` baseline worktree (same six
fixtures, same diagnostic). The shape is not a double-free: UBSan's
member-access check fires at the variant-block alloc (`v->rc = 1` at
runtime.h's no-pool `malloc(kai_var_block_size(n))`) because a
sized-exact block (#1155: 16–32 B for 1–3 slots) is smaller than
`sizeof(KaiValue)` — the detector's `-DKAI_NO_CELL_POOL` build has been
incompatible with exact-sized blocks since #1155 landed. The lane's
native detector half is green; the C-side false positive is reported on
the issue as pre-existing.

## Cost vs estimate

The mechanism was close to the plan (the four coupled sides were exactly
the four touch points; no piece had to stop the lane). The unplanned cost
was diagnostic, twice: proving the `--emit=native` SIGSEGV pre-existing
(a full baseline worktree build) and un-blaming the fixture stack
overflow (byte-identical C ruled the compiler out). Both closed with the
evidence in hand rather than a guess.

## Follow-ups (documented, on the issue)

1. `kaic2-native --emit=native` SIGSEGV in `nfn_llvm_type` — pre-existing
   (bisected to `origin/main`, identical frame), needs its own lane; the
   native-selfhost gate should grow a native-target smoke once fixed.
2. Enum BINDER reads now take the dynamic `kaix_variant_arg` shim where
   they used to be an inline `.ptr` borrow. A dedicated inline form (raw
   `.i64` load + `kai_enum_slot_box`, or an alwaysinline
   `kaix_variant_arg_enum`) would recover the rb-tree rotation binders'
   read cost — the residual the flat bench hides. Same one-verdict
   discipline: the KIR already knows kind 3 statically at the bind site.
3. The rc-detector's `-DKAI_NO_CELL_POOL` build false-positives on
   sized-exact variant blocks (UBSan member-access, six `c/…` cases,
   pre-existing since #1155 — identical on clean main). The no-pool
   alloc path needs a `sizeof(KaiValue)` floor (detector build only) or
   the UBSan object-size check scoped out at that seam.
