# Lane experience — remove the llvm-text backend

**Lane:** `remove-llvm-text-backend` · **Date:** 2026-06-16 · No closing issue
(the PR explains the retirement and the gate adjustment).

## Scope as planned vs as shipped

**Planned (brief):** remove the dead llvm-text frontend (`--emit=llvm` /
`--backend=llvm`, the `.ll` + clang path) completely, and retarget the parity
tier so the gate runs only the two real backends — C-direct (oracle) and native
(in-process libLLVM). PR with green gates.

**Shipped:** all of the above, plus the surface the brief did not enumerate —
the `benchmarks/rb-tree` harness's `kaikai-llvm` column, `tools/test-llvm-driver.sh`,
a moved shared helper (`expr_kind_name`), and the user-facing `docs/install.md`
backend section, which still documented `--backend=llvm` as the clang-autodetected
default. Net diff: **−9.8K lines** (8575 of them `emit_llvm.kai`).

## What was removed

- `stage2/compiler/emit_llvm.kai` (8575 LOC) — the entire `.ll`-text frontend.
- `stage2/tests/test_emit_llvm.kai` (155 LOC) — its unit tests.
- `.github/workflows/tier1-backend-parity.yml` — the #575 C↔LLVM-text CI gate.
- `tools/test-llvm-driver.sh` — the `--backend=llvm` driver-wiring smoke (no
  Makefile/CI invoked it; dead with the backend).
- driver.kai: the `MEmitLlvm` mode, the `--emit=llvm` parse arm + help line,
  `use_llvm`, the `emit_program_llvm(...)` branch. `cons_ok = not use_llvm`
  became `cons_ok = true` (the gate existed only because the llvm-text emitter
  lacked the cons-cell TRMC step builder; C and native both have it).
- bin/kai: `resolve_backend`/arg-parse now reject `llvm` (`c|native` only); the
  `--backend=llvm` compile branch, `ensure_clang`, `CLANG`/`CLANGFLAGS`, and the
  llvm-text help/usage all gone.
- stage2/Makefile: 9 whole llvm-text-only targets (`test-llvm`,
  `test-llvm-coverage`, `selfhost-llvm`, `test-tco-llvm-706/709/mixed`,
  `test-unbox-llvm-bench`, `test-issue-747`/`test-issue-747-mixed-slots`/
  `test-arm-top-reuse-shared` — the LLVM-Perceus-RC gates, redundant with the
  C-side `test-perceus-*`; `test-issue-524` — an explicitly "LLVM-side
  regression" whose C side is covered by `test-stdlib`); plus the `(LLVM)`
  sub-blocks of ~13 dual targets (keeping the `(C)` block); plus the
  `BUNDLE_SRCS` entry and the three target lists (`.PHONY`,
  `TEST_LIGHT_TARGETS`, `test-fast`).
- benchmarks/rb-tree: the `kaikai-llvm` column replaced by `kaikai-native`.

## What was preserved (and why)

- **`stage0/runtime_llvm.c` stays.** It is the shared `kaix_prelude_*` forwarder
  the **native** backend's emitted object links against (`bin/kai`:
  `$CC ... "$obj" "$RUNTIME_LLVM_C"`). It was the llvm-text backend's shim first;
  the native backend inherited it. Deleting it would break the native link.
- **`examples/llvm/` fixtures stay.** Despite the directory name and their
  llvm-text-era comments, they are ordinary kaikai programs (regression repros
  for #582 etc.) that the backend-parity harness walks by directory — they now
  exercise native-vs-C. Renaming the directory would be churn that breaks the
  harness `DIRS` list and any references; left intact (one fixture's comment was
  reworded off the removed `test-llvm-driver.sh`).
- **The `emit_native_*` / `kir_*` historical comments stay.** Their many
  `emit_llvm` mentions are prose ("mirrors the validated shape of lane #622") —
  bitácora, not live imports or calls. Left as record.

## The shared-helper trap (the brief anticipated this)

`expr_kind_name(ExprKind) : String` was defined **only** in `emit_llvm.kai` but
called by `driver.kai`'s `find_enc_expr` (the `--library-mode` enclosing-node
walk). Deleting the file blind would have broken library-mode at link time. It
is a pure `ExprKind → String` helper, so it moved to `emit_shared.kai` (which
already imports `compiler.ast`) — the proper home for analysis shared across
backends. `kir_lower.kai`'s `kir_expr_kind_name` is a distinct name; no
collision. The brief's point-6 ("if they share a helper, move it to
emit_shared.kai before deleting") was exactly right.

## Tier adjustment

The brief's two readings of "tier1-backend-parity runs only native-vs-c"
diverged in CI outcome (one workflow vs two). Confirmed with the user:
**delete `tier1-backend-parity.yml` entirely.** Its sole reason was the #575
C↔LLVM-text gate; `tier1-native.yml` already runs
`TARGET_BACKEND=native ORACLE_BACKEND=c NATIVE_PARITY_RATCHET=1
test-backend-parity.sh` over the full corpus, so it becomes the single
backend-parity gate. The harness default flipped `TARGET_BACKEND` `llvm → native`,
and the root Makefile `tier1-backend-parity` target retargets to native-vs-c so
`daily` still resolves. `native-parity-baseline.txt` is empty (0 gaps), so the
retargeted gate is green.

## Structural surprises the brief did not anticipate

- The llvm-text surface reached **past the named files** into
  `benchmarks/rb-tree/{run.sh,README.md}` and `docs/install.md` (a user-facing
  doc that still listed `--backend=llvm` as the clang-autodetected default — the
  most stale doc in the tree). A whole-repo grep at close caught these; the
  initial recon (scoped to the brief's file list) did not.
- Several "dual (C)+(LLVM)" Makefile targets were in fact **LLVM-only by
  construction** — their C side existed only as the diff partner, with the real
  C coverage living elsewhere (`test-perceus-*`, `test-stdlib`). Those were
  deleted whole rather than reduced to a C block. Two targets (`test-modules`,
  `test-modules-path`) diffed C-vs-LLVM with no golden; converted to C-vs-inline-
  golden (the output was already pinned in the comment as `42/42/12` / `42\nKAI!`).

## Gates

- `make selfhost` → **byte-id OK** (`kaic2b.c == kaic2c.c`): removal does not
  change the C path's output (expected — only the dead llvm-text path went).
- kaic2 rebuilds C-only without `emit_llvm.kai`.
- `--backend=llvm` / `KAI_BACKEND=llvm` → clear error (`must be 'c' or 'native'`).
- native-vs-C parity ratchet: 0 gaps (baseline empty).
- Whole-repo grep: no live `--emit=llvm` / `MEmitLlvm` / `emit_program_llvm` /
  `backend=llvm` outside `runtime_llvm.c`, retirement notes, and dated
  retro/measurement records.

## Follow-ups left for next lanes

- **`kaic2 --emit=llvm` directly does not error — it falls through to C.** The
  driver's flag dispatch treats any unrecognised `--…` as a path (pre-existing
  behaviour for *every* unknown flag, not just `--emit=llvm`), so
  `kaic2 --emit=llvm foo.kai` silently emits C for `foo.kai`. Not a crash (the
  brief's bar), and the user-facing surface (`bin/kai`, `KAI_BACKEND`) rejects
  `llvm` cleanly. Making the driver reject unknown `--flags` is a global
  behaviour change out of this lane's scope; flagged here rather than fixed.
- `#832` (Map SIGSEGV in the C oracle, flaky/Linux) is pre-existing, not this
  lane. If it trips the native-vs-C gate in CI, it is documented as such — not
  silenced with an invented skip.
