# Lane experience — issue #1164: cross-call reuse-token donation on KIR/native

## Scope as planned vs as shipped

Planned (issue + brief): give the KIR `KFn` ABI a donor channel equivalent to
the C backend's hidden `KaiReuse _donor` param, make the native backend pass
and consume it, grow the #1104 fixture family with a cross-call case that
fails pre-fix, and close the counter gap (native 6.3M variant allocs vs C
1.0M on rb-tree @1M).

Shipped: all of the above, plus one piece the issue's evidence table implied
but whose mechanism turned out to be different — the incref gap. The issue
attributed both the alloc gap AND the incref gap (8.3M vs 2.0M) to the
missing donation. Reproducing after the donor ABI landed falsified half of
that: allocs reached exact parity while increfs did not move. The incref gap
is the *survive-the-exit-drop* incref every native in-place variant reuse
paid (`kai_variant_reuse_at`'s `kai_incref` + the match-exit decref), which
the C backend elides with its `_r = _scr; _scr = NULL` move. Closing the
brief's binary incref gate required porting that move too: `RkVariantMove` +
static exit-drop neutralisation, including a fork for the previously
single-body flat reuse path. Both mechanisms shipped in this lane; both are
reuse-ABI parity, same files.

## Design decisions

- **Capability set = the C backend's scan, reused.** `kir_lower_fns` calls
  `rd_scan_capable` (emit_reuse_donate.kai) over the same post-perceus decls,
  so the two backends cannot disagree about which fns are donation-capable.
  The flag lives on `KFn.donor` and is printed in `kir_dump_fn_sig_pub`, so
  the native-modular content-addressed cache key flips every consumer when a
  callee's capability changes — the ABI is safe under separate compilation
  (unlike C, where donation is single-TU only, because C modular re-derives
  from bodies a cross-TU site cannot see; the KIR is lowered whole-program
  in both native modes).
- **Consumption sites are marked in the lowering, not guessed in the
  emitter.** A new `KReuseKind` (`RkDonorVariant`) marks the rebuild cell
  that absorbs the donor: the second same-shape sub-ctor in the rotation
  unique branch (after the inner-token steal), the first nested sub-ctor in
  the recolor-wrap unique branch and in the fresh/shared path — the same
  three sites the C emitter's `rd_donate` rewrites. The emitter degrades the
  mark soundly (null token → fresh alloc) in a fn with no donor channel.
- **Caller side is emitter-only.** Any direct call, raw call, or thunk to a
  capable callee fills the trailing param: a live `_arm_ru` MOVES in (read +
  null — the arm epilogue free and the TRMC step then see null), else null.
  No tail-position analysis, mirroring `emit_user_call`'s "whichever
  consumer runs first wins" contract. `KTailCall` is still unsupported
  native, so `nemit_call_direct` is the single funnel.
- **Leftover discipline**: every fn exit (`KRet`, `KTrmcApply`) of a donor fn
  frees an unconsumed token (null-safe), the mirror of `rd_wrap_body`.
- **Move reuse**: `kaix_variant_reuse_move_i64` overwrites in place and
  returns the cell at rc==1 with no incref; the lowering rebinds the
  enclosing match's exit-drop register (`st.mscr`, the same channel TRMC
  steps use) to unit in the unique branch only. The flat path now forks on
  `kaix_check_unique`, lowering args ONCE before the fork (atoms shared by
  both branches — unlike the cons dual, nothing binds differently per
  branch, so no double evaluation and no double dups).

Alternatives considered: doing the whole thing in the emitter (as C does)
was rejected because KIR has no arm context at emission — the rebuild cell
cannot be distinguished from an ordinary `KCon` there; a real `KParam` for
the donor was rejected because every consumer of `f.params` assumes user
arity (raw-signature classifier, thunk adapters, clause counters).

## Structural surprises

- The issue's premise that donation drives the incref gap was falsified by
  the closing measurement protocol itself (fix, re-measure, attribute the
  residual). Donation closed allocs exactly; increfs needed the move port.
- The existing fixture family's "native variant" (`test-perceus-1053-…`)
  gates at alloc < 1M over 100k inserts — loose enough that in-arm reuse
  alone passes it. The new target reuses the 1104 fixture source with the C
  target's strict gates (alloc < 150K, no `reuse_freed`) on native; it fails
  on pre-fix main at 815K allocs.
- `git stash` in a worktree shares the stash stack with every other lane —
  a no-op stash (clean tree exits 0) followed by `stash pop` applied a
  foreign lane's stash and conflicted three files. Recovered with
  `reset --hard`; the foreign stash entry survived untouched.

## Fixtures

- `test-perceus-1164-cross-call-donation` (stage2/Makefile): the 1104
  fixture built with `KAI_BACKEND=native`, gates alloc_total < 150000 over
  100k sequential inserts AND no `reuse_freed` line AND golden output.
  Pre-fix: 815039 allocs (FAIL). Post-fix: 100009 (OK). Wired into
  tier1-native.yml, not TEST_LIGHT_TARGETS.
- Existing `test-perceus-1104-balance-token-donation` (C) and
  `test-perceus-1053-nested-rotation-reuse` (native) stay green.

## Results (rb-tree @1M inserts, mac arm64)

| counter        | C backend | native pre | native post |
|----------------|-----------|------------|-------------|
| variant allocs | 1,000,030 | 6,301,552  | 1,000,024   |
| incref_total   | 2,000,006 | 8,301,534  | 2,000,006   |
| decref_total   | 2,000,017 | 8,301,542  | 2,000,014   |
| reuse_in_place | 6,301,528 | 6,301,528  | 6,301,528   |

Wall (best-of-5, interleaved, same run): pre-fix native 1.27× C-backend →
post-fix 1.13× C-backend (0.59s vs 0.52s @1M in the measured round; the
recovered ~0.07s matches the issue's 0.05–0.08s attribution). The residual
~1.13× is no longer RC traffic — counters are at exact parity — it is
emission quality, outside this lane's scope. Three-way including Koka:
koka 0.36s / C-backend 0.46s / native 0.52s (native 1.44× koka, C 1.28×
koka) in the interleaved round measured; mac wall is noisy (±10%), ratios
were stable across rounds.

## Cost vs estimate

The donor ABI itself landed close to plan (one lowering file, four emitter
files, one runtime forwarder). The unplanned half — falsifying the incref
premise and porting the move — roughly doubled the change surface inside
the same files.

## Follow-ups left

- `kaic2-native --emit=native` SIGSEGV (preexisting, bisected to clean main
  before this lane; reported on #1157) still needs its own lane; the
  native-selfhost gate's `--emit=c` probe remains blind to it.
- The C backend's donation is still single-TU only; if C modular ever wants
  it, the KIR's signature-carried capability is the model to copy.
- Cons reuse (`kaix_cons_reuse_move`) still pays the survive-the-drop
  incref; list-heavy workloads could get the same move treatment.
