# Lane experience — issue #1021, the RC self-host detector

Institutionalises the ASAN-self-host method that pinned every link of the
#1021/#1025 RC bug chain in minutes, turning a reactive UAF hunt into an
exhaustive CI detector. Ships infrastructure; does not close #1021 (Lane 2
closes it with the native self-compile).

## Scope — planned vs shipped

Planned: three components — (1) ASAN self-host on both backends over a
minimal corpus, (2) `KAI_TRACE_RC` as an absolute counter over directed
family-B fixtures, (3) CI wiring at a daily / RC-path-gated cadence.

Shipped: all three, with one design correction to component (2) forced by
what the runtime actually exposes (below). No new harness framework — the
detector extends the existing `test-perceus-*-asan` pattern and the
`native-selfhost-link.sh` bitcode-hiding trick already used to instrument
the native runtime.

- `tools/rc-selfhost-detector.sh` — the detector.
- `tools/rc-detector-corpus.txt` — the corpus (families A + B, reusing
  `examples/perceus/` fixtures).
- `stage2/Makefile` target `rc-detector` (+ `.PHONY`).
- `.github/workflows/rc-detector.yml` — path-gated to RC code + daily cron,
  installs libLLVM, runs both backends.

## The two blind spots it closes

1. The C cell pool recycles a freed cell into the next same-size alloc, so
   a double-free hands back a live-looking cell and the allocator never
   trips. `-DKAI_NO_CELL_POOL` turns the pool off, so the freed cell hits
   the real allocator and ASAN's redzone catches the second free / stale
   read. This is why native (real malloc) exposed the chain first.
2. Differential "byte-id against the C oracle" is blind to a bug both
   backends inherit from the shared front end (perceus/emit): an over-decref
   planted for BOTH backends passes a diff that compares them to each other
   (bug#5 proved this). So the detector checks an ABSOLUTE invariant on the
   C backend AND the native backend — the C oracle is not assumed clean.

## Design surprise — the strict-ledger `DOUBLE` flag is weak, ASAN is the real signal

The brief framed component (2) as "each cell reaches rc 0 exactly once,
assert it absolutely." Two facts from the runtime reshaped that:

- **`leaked` is never 0 at exit.** The runtime does no final free walk;
  whatever `main` holds live at `exit()` counts as leaked. A clean
  fixed fixture reports `leaked=18` under `-DKAI_NO_CELL_POOL`. So
  `leaked==0` is not the invariant — a per-fixture leak baseline would be
  fragile and off-target for a chain that was all double-frees, not leaks.

- **The strict per-tag `DOUBLE` flag (`frees > allocs`) does not fire on a
  classic double-free.** Under `-DKAI_TRACE_RC=1` the runtime poisons a
  freed cell's bytes with `0xDEADBEEF` — including its `tag`. The second
  free then reads a garbage tag, so `kai_rc_free_by_tag[tag]` is bumped for
  the wrong (or out-of-range) tag and the per-tag `frees > allocs` test
  never sees it. The poison IS the protection, but it manifests as an ASAN
  abort (freed+poisoned cell read/freed → redzone) or as a corrupted-value
  output diff, NOT as the `DOUBLE` marker.

Correction: the strong, verified signal is **ASAN + `-DKAI_NO_CELL_POOL`**
(it caught a toy `heap-use-after-free` cleanly) plus a **functional diff**
against the golden (silent corruption that does not crash). The strict
`DOUBLE` grep stays as a cheap complementary check — it fires only when a
second decref does NOT poison-then-reclassify — but it is not the primary
gate. The detector's absolute assertion is: no sanitizer diagnostic + output
matches the golden, on both backends.

## Native instrumentation — the bitcode-hiding trap

The P2 native path links the runtime bitcode INTO the object and the final
`cc` link is just `obj + libm` — ASAN then instruments nothing (there is no
runtime TU to instrument). To instrument the native runtime the detector
forces the legacy link path by hiding `stage0/runtime_llvm.bc` for the
duration, so `bin/kai` falls back to `cc … runtime_llvm.c` and `$CFLAGS`
(carrying `-fsanitize=address -DKAI_NO_CELL_POOL`) instruments it. The bc is
restored on any exit via a trap — a failing fixture must never leave the
tree stripped.

## Proof it catches — reverting the #1040 fix

Reverted `stage2/compiler/emit_c.kai` to its pre-#1040 state
(`git checkout 9aff4941^ -- …`), rebuilt kaic2, and ran the detector over
`trmc_spread_shared_arg` (the family-B fixture that fix repairs). Both
backends failed with `heap-use-after-free … in kai_decref` and the detector
exited non-zero — confirming it catches the exact double-free the chain was
made of, on C AND native. Restored the fix; tree clean; detector green
again.

## Corpus

Reuses `examples/perceus/` fixtures across both families:

- **Family A (nested alias binders):** `selftail_nested_variant_binder_1025`,
  `variant_nested_discriminant_dup_1025`, `native_variant_reuse_double_use_872`,
  `match_shared_tag_subdiscrimination`, `gap3_tail_rebuild_variant`,
  `gap3_tail_rebuild_record`.
- **Family B (TRMC / reuse-cons with a shared arg):** `trmc_spread_shared_arg`,
  `cons_reuse_shared_donor_1025`, `cons_reuse_embed_unique_1025`,
  `native_cons_selftail_leak_860`, `native_shared_reuse_corruption_995`.

No new fixtures were needed — the chain already left directed goldens for
every shape. Adding a fixture to `rc-detector-corpus.txt` extends coverage
with no code change.

## Cadence decision

A dedicated `rc-detector.yml`, path-gated to RC code + a daily cron, not a
slot in `make daily` or in tier1/tier1-asan. Reasons: (a) `make daily` runs
on an ubuntu box with no libLLVM installed and is already at its 30-min
budget (its own comment records tier1-asan/parity being pulled out for
exactly this); (b) tier1/tier1-asan are per-PR merge gates and the detector
is too heavy for that. Path-gating runs it on PRs that touch RC code (where
the risk is) and the cron gives a daily floor, while non-RC PRs pay nothing.

## Follow-ups

- If a future link needs the strict ledger to catch a double-free directly
  (not via ASAN), teach `kai_free_value` to detect the poison sentinel on
  entry and count a real `DOUBLE` before reclassifying the tag.
- The native strict-ledger pass is skipped (the strict macros are C-link
  only); native's absolute check is the ASAN pass. A native strict ledger
  would need the counters wired through the in-process runtime bitcode.
