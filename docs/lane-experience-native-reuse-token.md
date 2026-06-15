# Lane retro — native reuse-in-place + variant-TRMC (Koka move semantics)

Branch: `native-reuse-token`. Base: `3162c30` (main).

Closes the reuse/rb-tree cluster of the in-process libLLVM native backend
(KIR Lane 1.5): the native eager-decref'd reuse slots 1:1 → double-collapse
(size:0 / SIGSEGV) on a non-bijective rb-tree rotation. The brief framed it
as a single "double-free in reuse"; the measured reality was **three** native
bugs, none of which was actually memory-unsafe (ASAN was clean throughout) —
the rb-tree collapsed *logically* because the native RC for the reuse/TRMC
path never ran. The C-direct oracle (`emit_c.kai`) lowers from the AST and was
always correct, so every bug was native-only.

## Scope as planned vs as shipped

Planned (brief): port the Koka drop-reuse-token protocol — unstick the trapped
`KDropReuse`/`KFreeToken` emit, swap the eager-1:1-decref `KConReuse` for the
shell-donating token model.

Shipped (after diagnosis corrected the brief): `KDropReuse`/`KFreeToken` turned
out to be **declared in the KIR but never produced** by the lowering — the
token model is an emit_c-internal arm-top transformation that the KIR discards.
The native only ever sees `KConReuse` (from perceus `__perceus_reuse_*`). So the
fix is asu's **OPTION C**, not the token port: keep the uniqueness gate, drop
the spurious child-decref. Three coupled fixes:

1. **Reuse routing by kind.** `KConReuse` routed *every* reuse — variant, cons,
   AND record — through `kaix_reuse_or_alloc_variant`, so `reuse_record_basic`
   hit a variant cell on a `KAI_RECORD` ("field access on non-record"). Added a
   `KReuseKind` tag (`RkVariant`/`RkCons`) on `KConReuse` and a separate
   `KRecordReuse(donor, fields)` node (records need field NAMES, variants/cons
   don't). The backend routes variant→`kaix_variant_reuse_at`,
   cons→`kaix_reuse_or_alloc_cons`, record→`kaix_reuse_or_alloc_record`.

2. **Variant reuse: eager-decref → MOVE semantics.** The eager-1:1-decref helper
   double-frees a non-bijective rotation: a child migrating from slot 1 of the
   scrutinee to slot 4 of a *new* child node is decref'd "because slot-1 differs"
   while it stays live in the rebuild. The fix is `kai_variant_reuse_at` (Koka
   move: uniqueness gate intact, donate the unique cell, write the new slot
   words, do NOT decref the donor's old children — perceus already balanced the
   RC for a move; the deconstructing arm owns the children). New shim
   `kaix_variant_reuse_at` (mirrors the existing `kaix_reuse_or_alloc_variant`
   boxed-args bridge, mask==0). **Which slots get written:** all of them, in slot
   order, from the KConReuse `inits` — but with NO old-child decref, the inits
   already carry the move discipline (kept children come dup'd, moved children
   raw). That is the whole "write-only-changed-slots" intent of the Koka
   protocol expressed through the existing all-boxed init buffer.

3. **Variant-TRMC + the dropmask.** The native had NO variant-TRMC — `nemit_trmc_step`
   built a binary `kaix_cons` regardless of cname, so the rb-tree `insert_loop`
   (a variant-TRMC over `Node`) was mis-built. And it IGNORED the TRMC/tcrec
   dropmask (a comment said "dropmask is a no-op here; a heap-param loop would
   need the drops, a later subset"). Together these meant the rb-tree's RC never
   ran (`decref_total=0` → `kai_check_unique` lies → reuse corrupts → size:0).
   Added: variant-TRMC spine build (`kaix_variant` with a NULL placeholder at
   the hole + new `kaix_variant_slot_addr` for the hole address); `KTrmcStep`
   now carries the ctor tag (resolved at lower time, as `KCon` does). The
   dropmask is **accepted and ignored** — see "what the dropmask taught us".

## Design decisions and alternatives

- **OPTION C over the token port (asu-validated, twice).** Perceus balances RC
  assuming the reuse ctor CONSUMES the donor (Koka move); the deconstructing arm
  already disposed the children. So the runtime eager-decref is double-counting =
  the double-free. The cure is to delete the decref, not to port the token
  protocol the KIR doesn't carry. Option A (fix the eager-decref's aliasing
  guard) was rejected: the 1:1 guard compares slot-i-old vs slot-i-new of the
  SAME node, so a cross-slot migration is invisible to it. Option B (produce
  `KDropReuse`/`KConReuse-at-token`/`KFreeToken` in the lowering) was rejected as
  far larger than the bug needs — move-semantics on the existing `KConReuse` is
  the minimal correct change.

- **`KReuseKind` + `KRecordReuse` over overloading `KConReuse`.** The record reuse
  needs field names (`[KFieldInit]`), the variant/cons reuse needs slot inits
  (`[KSlotInit]`). Forcing one node to carry both is uglier than a 2-constructor
  `KReuseKind` for the slot-init forms + a dedicated record node.

- **Tag on `KTrmcStep`, resolved at lower time.** The variant-TRMC build needs the
  ctor tag; the cname alone is not enough (`ls_variant_tag` lives in the lowering,
  not the backend). Resolving it once at lower time matches how `KCon` already
  carries its tag — the backend never re-derives a name→tag table.

## Structural surprises the brief did not anticipate

- **The token nodes are dead.** `KDropReuse`/`KFreeToken` are declared but never
  produced — the brief's "unstick the trapped emit" was a red herring; the live
  path is `KConReuse`. The trapped arms stay trapped (correctly — nothing emits
  them).

- **ASAN was clean from the start.** The bug was never memory-unsafety. size:0
  was logical corruption from a broken RC (`kai_check_unique` lying), not a
  double-free that ASAN would catch. The real soundness gate is therefore the
  result-vs-oracle parity + the `decref_total` going 0→nonzero, not "ASAN clean"
  alone (which held even when the tree was garbage).

- **The native never frees anything.** `free_total=0` even on `hello world` — the
  all-boxed native backend decrefs (now, after this lane: 0→314 on the rb-tree)
  but never cascades decref→free. This is a pre-existing backend gap, ORTHOGONAL
  to reuse, and the reason the residual leak is acceptable: it is bounded to
  live_peak (no transient over-alloc), and the lane MOVED the RC forward, never
  back. Discriminant that proves it's not a new leak from my move-semantics:
  `hello world` (zero reuse) shows the same `free_total=0`.

- **kaic1 lowers a value-returning `match Bool` to an LLVM `select`.** A
  `match cond { true -> emit_A  false -> emit_B }` that RETURNS a Handle compiled
  (under kaic1 stage1) to `select cond, A_result, B_result` — evaluating BOTH
  arms, emitting two ctor calls + an invalid `select ptr %c, null, @kaix_cons`
  that failed module verify for EVERY native program (even hello world, via the
  stdlib's cons-TRMC). The fix is to never return a Handle from such a `match`:
  the cons/variant TRMC steps are now two full Unit-returning bodies sharing a
  `ntrmc_extend_and_loop` tail. (A `match` over a real enum — `KReuseKind`,
  `KOp` — is fine; only the 2-arm `match Bool` triggers the select lowering.)

## What the dropmask taught us (the jwt near-miss)

Replaying the TRMC/tcrec dropmask as a naive per-index `kaix_internal_drop` of
the flagged param REGRESSED `jwt_encoder` (`json_lookup_str` returned
`"kid":"beef"` instead of `beef`) — it over-dropped a still-live heap param in
the stdlib `list_*` JSON-parse loops. The C oracle's `tcrec_emit_drops_masked`
respects a per-param `pmask` (raw scalars are never dropped) that the native KIR
does not thread. Rather than ship a half-right drop, the dropmask is
**accepted-and-ignored** on the native back-edge: reuse correctness is carried
by the move-semantics reuse + variant-TRMC build (verified result-correct +
ASAN-clean WITHOUT a back-edge drop on all 8 fixtures), and skipping the drop
only leaks more in the already-non-freeing backend. Emitting the dropmask
correctly needs the pmask threaded into the KIR — a follow-up, noted in the
baseline header. This is the "lowering gate must measure correctness" lesson:
the dropmask "looked" right (it mirrors the C bit-walk) but diverged on a
fixture the move-semantics path already covered; the gate that caught it was
the full parity suite, not the rb-tree alone.

## Fixtures closed and coverage

8 closed (baseline 14→6). The brief's six: `reuse_record_basic`,
`llvm_arm_top_reuse_shared`, `llvm_rc_nested_match`,
`nested_pattern_reuse_balance`, `demos/9d9l/huffman` — PASS native==C. Plus the
three former "Linux-only SIGSEGV" gaps the variant-TRMC/reuse fix also resolved:
`list_helpers`, `list_zip3_scan`, `demos/9d9l/weather` — verified native==C on
BOTH macOS AND Linux (LLVM 18.1.3 Docker, CI-equivalent), so I removed them and
superseded the earlier "STAY listed" note.

Existing fixtures cover the shapes end-to-end (rb-tree balance = nested variant
reuse + variant-TRMC; `reuse_record_basic` = record reuse). No new fixture was
needed — the regression that would have justified one (jwt) is covered by the
existing `jwt_encoder` in the parity corpus, which now gates the dropmask
decision.

## The 6th fixture — `scalar_fn_sig_deep_recursion` — closed by the rebase

During the lane this was a REAL separate gap, NOT a reuse bug: `deep(n-1)+1` is
non-tail recursion of 200000 frames, the native emitted boxed
`define ptr @deep(ptr)` (a `kai_int` box per frame) → 8 MB stack overflow
(SIGSEGV), while the C-direct oracle emits scalar `i64 @kai_deep(i64)` (tiny
frame). With `ulimit -s 65520` the native ALSO computed 200000, confirming the
divergence was purely frame-size, not logic. The lane's decision (asu-validated)
was to leave it as a documented separate gap (the cure being native scalar
signatures, a backend-wide lane — NOT a mac-only `-Wl,-stack_size` hack that
does nothing on the CI Linux gate).

Then the rebase onto `main` brought the in-process LLVM opt-passes lane (#839 /
#498), whose `mem2reg` promotes `deep`'s boxed allocas to SSA registers — the
frame shrinks enough that 200000 non-tail frames no longer overflow the default
stack. So `scalar_fn_sig_deep_recursion` now passes native==C (200000) and is
removed from the baseline. The frame-size cure was the opt-pass lane's, not this
one's; the gap is simply genuinely closed now, so all SIX of the brief's
fixtures pass. (#718 was the *text-LLVM-emitter* version of the boxed-frame
problem and is CLOSED; the in-process backend still emits boxed signatures —
scalar signatures remain a future improvement — but opt-passes made the deep-
recursion case fit the stack regardless.)

## Residual gaps (orthogonal, documented in the baseline header)

- **Native never cascades decref→free** (`free_total=0`). All-boxed backend RC
  honesty gap, pre-existing. Not a double-free (ASAN clean); bounded leak.
- **TRMC/tcrec dropmask un-emitted.** Needs the C oracle's pmask raw/boxed param
  distinction threaded into the KIR before a correct per-index drop can land.
- **Native scalar fn signatures** (the scalar_fn fixture's root).

## Cost vs estimate

The diagnosis was the cost, not the code. Three Docker round-trips (one
contaminated the macOS checkout with ELF binaries via a shared mount — the
"deep-clean before rebuild" lesson, re-learned; subsequent runs used an
isolated rsync-inside-container copy). The fix itself is ~336 lines across the
backend + one runtime shim file. `km`: emit_native_trmc.kai A++ (97.3, cogcom
avg 0.9/max 2), emit_native_ops.kai A (90.3, avg 0.8/max 4), emit_native_term.kai
A+ (95.2, avg 0.8/max 2) — all above the A− bar, all <400 LOC.

## Gates

- 8/8 closed fixtures: native==C-direct on macOS AND Linux (LLVM 18.1.3 Docker).
- ASAN clean on all 8 — zero double-free / UAF.
- KAI_TRACE_RC on the rb-tree: `decref_total` 0→314, size:0→200; residual leak
  is the pre-existing non-freeing backend (orthogonal).
- native-parity ratchet: OK, 6 gaps == baseline, ZERO new gaps (jwt held green).
- selfhost byte-id (kaic2b.c == kaic2c.c); tier0 OK.
