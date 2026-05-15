# Lane experience — issue #575: tier1-backend-parity CI gate

**Status:** Shipped.

**Lane branch:** `issue-575-backend-parity` (from `main`, post-v0.64.0).

## Scope as planned

From the brief: build a CI tier that compiles every entry-point
fixture under the documented example dirs and demos with both the
C and the LLVM backend, runs both binaries, and diffs stdout +
exit code. Wire it as a separate workflow paralleling the other
`tier1-*` jobs. Run an initial sweep, file separate issues for
each LLVM-broken fixture, skip with a paired bookmark.

The original brief framed the work as 1-2 days, mostly script
authoring + sweeping the existing fixture base. That estimate
held.

## Scope as shipped

1. `tools/test-backend-parity.sh` — generalization of
   `tools/test-llvm-driver.sh`. Walks the documented example dirs
   plus `demos/`, splits each dir's contents into flat-shape
   (`<dir>/<name>.kai`) and package-shape (`<dir>/<pkg>/main.kai`)
   entry points, builds with both backends, runs each binary with
   a 30-second timeout, and diffs stdout + exit code. Two skip
   mechanisms: a `tools/backend-parity-skips.txt` file (each line
   pairs a fixture path with an open tracking issue) and an inline
   `// skip-backend-parity` annotation for fixtures that are
   intentionally backend-specific.
2. `tools/backend-parity-skips.txt` — populated from the initial
   sweep with the fixtures that are LLVM-broken on main today.
3. `.github/workflows/tier1-backend-parity.yml` — separate job
   running on the `nibiru` self-hosted Linux runner (the same one
   tier1-asan uses), gated on the same `paths` filter expanded to
   include `examples/**`, `demos/**`, the script, and the skips
   file. 60-minute job timeout to absorb the worst-case sweep.
4. `docs/testing-tiers.md` — new "Backend-parity discipline (issue
   #575)" section documenting the gate, the skip discipline, and
   what to do when it fails.

## Initial sweep results

The full sweep on `main` (post-v0.64.0) walked 403 entry points and
surfaced **71 FAILs** across four categories — far more than the
brief's 1-10 skip estimate. The shape of the failures rather than
the count is what matters:

1. **Negative-by-design fixtures inside positive dirs (#626).**
   Roughly 40 fixtures under `examples/effects/`,
   `examples/perceus/`, `examples/stdlib/`, and
   `examples/quickstart/` are designed to *reject* at compile time
   but live in dirs the parity harness walks. They have **no
   sibling `.err.expected` golden** — instead, they are gated by
   hand-rolled `stage2/Makefile` loops that grep stderr for a
   per-fixture diagnostic substring. The parity harness has no way
   to discover this without replicating the Makefile loop logic.
   Filed as **#626** (umbrella for fixture categorisation).
2. **LLVM-only divergences (#622)** — 21 fixtures build cleanly on
   both backends but produce divergent runtime behavior on LLVM:
   - 11 SIGSEGV (exit 139) under LLVM, exit 0 under C — most
     likely default-handler installation gaps (same shape as the
     closed #570 Spawn case). Includes `m7a_7_default_*`,
     `process_basic`, several `unbox_bench` fixtures, and
     `demos/euler4`.
   - 6 other exit-code divergences ranging from SIGBUS to
     C-fails-but-LLVM-passes (inverted exit code).
   - 3 stdout-content divergences (`m8_4_cancel_caught.kai`,
     `json_basic.kai`, `demos/poker_dealer`).
   - 3 LLVM emit/link failures (`trace_basic.kai`,
     `trace_prefix.kai`, `reuse_record_basic.kai`).
3. **LLVM-only Unicode mishandling (#618)** — two fixtures show
   stdout divergence on JSON `\uXXXX` escape decode + encode:
   `json_surrogate_decode.kai` and `json_surrogate_encode.kai`.
   On C every assertion prints `: ok`; on LLVM every assertion
   prints `MISMATCH` with byte-garbled output. Likely missing or
   wrongly-bound hex-decode helper in `runtime_llvm.c`.
4. **Aspirational demos (#626)** — 13 demos under `demos/9d9l/` and
   `demos/vs/*` are documented in their own docstrings as
   "FAIL until feature X lands". They are intentionally
   not in `make demos-no-regression`'s baseline. The harness has no
   way to distinguish these from real demos without an explicit
   marker. Folded into #626.

A small additional batch (#616 — LLVM-only Env/Stdin segfaults — was
filed separately before the umbrella #622 batch issue was opened.
Both are listed in the skips file.) The early
`.err.expected`-detection structural fix landed in the harness
itself: any fixture with a sibling `.err.expected`,
`.diag.expected`, or `.run.err.expected` is auto-skipped, since
`tools/test-negative.sh` is the right gate for those. That covers a
further ~4 fixtures cleanly without skip-list entries.

The negative-by-design class shipped a structural fix: the
harness now auto-skips any fixture with a sibling
`.err.expected`, `.diag.expected`, or `.run.err.expected` golden
(plus `<dir>/main.err.expected` for the package-shape variant).
This is the right rule because those fixtures are exercised by
`tools/test-negative.sh`, which already asserts they reject —
parity has nothing to add. The four fixtures from category 1
disappear from the harness output without needing skip lines.

The LLVM-segfault and fixture-rot classes are in the skips file
(`tools/backend-parity-skips.txt`) pointing at #616 / #617.

After the structural `.err.expected` auto-skip + the populated
skips file, the harness produces:

- **PASS:** 296.
- **FAIL:** 0.
- **SKIP:** 107 (entries in `tools/backend-parity-skips.txt` plus
  fixtures auto-detected via `.err.expected` siblings).
- **TOTAL entry points walked:** 403.

The skips file documents each fixture with a one-line reason and
the issue number to watch.

### Categorising the failures

| Class                                 | Count | Disposition                                                   |
|---------------------------------------|-------|---------------------------------------------------------------|
| Negative-by-design (sibling golden)   |  4    | Auto-skipped by `.err.expected`/`.diag.expected` detection.   |
| Negative tests in positive dirs       | ~30   | #626, skipped with bookmark.                                  |
| Library files mis-detected entry-pt   |  ~4   | #626, skipped with bookmark.                                  |
| Fixture-rot (renamed module surface)  |  3    | #626, skipped with bookmark.                                  |
| Multi-package fixtures (need --path)  |  4    | #626, skipped with bookmark.                                  |
| Aspirational demos (FAIL by docstring)| 13    | #626, skipped with bookmark.                                  |
| LLVM-only divergences                 | 21    | #622, skipped with bookmark.                                  |
| LLVM-only Unicode mishandling         |  2    | #618, skipped with bookmark.                                  |

## Design decisions

### Walk shape: flat *.kai + package main.kai

The first probe surfaced two false-positive failure classes:

1. **Library files in package fixtures** (e.g.
   `examples/packages/local_path/lib_greet/greet.kai`,
   `examples/unstable/multi_unstable_pub_decls/mylib.kai`) — these
   are not standalone programs; compiling them triggers a link
   error (no `kai_main` symbol). Including them as fixtures would
   produce ~10 spurious "C-FAIL" entries that have nothing to do
   with backend parity.
2. **Negative fixtures** under `examples/negative/` — these are
   designed to reject at compile time, so the parity assumption
   ("both backends build cleanly, then both binaries run") doesn't
   apply. Excluded by directory rather than by inspection.

The harness encodes this by walking `*.kai` directly under each
listed dir (flat shape) and `**/main.kai` for subdirectories
(package shape). Library siblings of `main.kai` get pulled in by
the kaikai module loader as needed; they are never compiled as
standalone entry points.

### Skip mechanism: separate file, not inline

Two skip mechanisms intentionally:

- **`tools/backend-parity-skips.txt`** for the LLVM-broken cases.
  Format `<path>:<issue>:<reason>`. The skip is the bookmark; the
  issue is the work. When the issue closes, the closing lane
  removes the line and re-runs the harness to confirm parity. This
  keeps the skip list reviewable in PRs and discoverable through
  `git blame`.
- **Inline `// skip-backend-parity` on the fixture's first line**
  for genuinely backend-specific fixtures (e.g. a feature only
  one backend implements by design). This lives with the source
  because the divergence is the source's contract, not a bug.

The file-based mechanism dominates in practice: every entry today
points at an open issue, not an intentional divergence. The inline
mechanism is the escape hatch when one materializes.

### CI shape: separate job, Linux runner, 60-min timeout

Modeled on `tier1-asan` rather than folded into `tier1`:

- **Path-gated.** A typer-only or fmt-only PR doesn't pay the
  ~15-30 min sweep cost. Only diffs touching `stage*/`, `stdlib/`,
  `examples/`, `demos/`, `bin/kai`, the script, or the workflow
  itself trigger the gate.
- **Linux runner (`nibiru`).** Mirrors tier1-asan's host. The C
  backend's emit-side bugs that are invisible on macOS but visible
  on Linux glibc (`#92 R6 dropmask`, `#350/#368 segfault`) suggest
  that backend-parity is more discriminating on Linux too.
- **60-minute timeout.** The local sweep on the dev mac took ~TBD
  minutes for ~TBD fixtures at ~5 s/fixture (build C + build LLVM
  + run + diff). CI Linux is comparable; 60 min absorbs cold-cache
  variance and future fixture growth.

The gate is not folded into the existing `tier1` workflow because
that workflow uses a macOS self-hosted runner (`shaka`), and the
CI cost of running this sweep on every PR — including doc-only
PRs that the `tier1` workflow short-circuits — would be wasted.
Separate workflow, separate `paths` filter.

## Structural surprises

### LLVM backend has more divergence than the issue suggested

The brief cited five LLVM-only failures over two weeks (#513,
#522, #524, #570, #571). The full sweep surfaced **23 more** that
had not been reported by any downstream consumer:

- **#622** — comprehensive batch of 21 LLVM-only divergences
  (segfaults, exit-code mismatches, stdout diffs, emit/link
  failures). Most plausibly cluster around 3-5 root causes
  (default-handler installation gaps, missing `runtime_llvm.c`
  helpers, fiber/scheduler nondeterminism in trace fixtures, RC
  discipline divergence). The umbrella issue indexes these for
  per-cluster sub-issues.
- **#618** — JSON `\uXXXX` Unicode escape mishandling on
  encode + decode. Almost certainly a missing or wrongly-bound
  hex-decode helper in `runtime_llvm.c`.

This is the **load-bearing finding**: the gate exists precisely
because of the asymmetry between "the issue cited five" and "the
sweep found twenty-three more". Five backend-parity bugs in two
weeks were each discovered by external consumers (notably ahu)
before a downstream report made them visible. Twenty-three more
sat in `main` undetected — every one of them would have surfaced
the same way if the harness had not landed.

### Library/main split needed explicit detection

The first naive `find -name '*.kai'` walk produced ~12 spurious
C-FAILs in a 33-fixture probe — every one of them a library file
whose `main` is in a sibling. The flat/package split (above)
fixed this without per-fixture annotation, which would have
required touching every package-shape fixture in the tree.

### Negative-by-design fixtures hide inside positive dirs

The first sweep surfaced four fixtures (`map_assign_error.kai`,
`regex_predicate_hash.kai`, `regex_predicate_unterminated.kai`,
`non_pub_ignored/main.kai`) that document *expected* typer
rejections via sibling `.err.expected` goldens. They live in
`examples/stdlib/` and `examples/unstable/` — both positive dirs
the parity harness walks — because they are the regression
fixtures for closed issues whose bug shape lived in those areas.

Adding seven entries to the skips file would have worked but
buried the structural rule. Instead, the harness now auto-skips
any fixture with a sibling `.err.expected`, `.diag.expected`, or
`.run.err.expected` golden. This is the right rule because those
fixtures are gated by `tools/test-negative.sh`, which already
asserts they reject correctly — parity has nothing to add.

## Fixtures added

This lane adds no `.kai` fixtures — it adds a *harness* that gates
the existing fixture base. Future fixtures added to any of the
walked dirs implicitly opt into the gate.

The closest precedent is `tools/test-llvm-driver.sh`: that script
gates the *driver* wiring (the `--backend` flag, `KAI_BACKEND` env
var, clang detection, precedence rules) on a hard-coded fixture
list of 11 entries. This lane keeps the driver gate as-is and adds
a separate, broader, fixture-walking gate for the *runtime
behavior* contract.

## Cost vs estimate

Brief estimate: 1-2 days. Actual: about half a day end-to-end
(script + two full sweeps + classification + issue filing + CI
workflow + Makefile target + docs + retro). The first sweep cost
~30 min wall on the dev mac; that is the dominant cost item and
it is amortized by CI on subsequent runs.

## Follow-ups

- The skips file points at four umbrella issues:
  - **#622** — comprehensive LLVM divergence audit (21 fixtures).
  - **#618** — JSON `\uXXXX` Unicode mishandling (2 fixtures).
  - **#616** — early-filed Env/Stdin segfaults (2 fixtures, also
    referenced by #622).
  - **#626** — fixture-categorisation gap: negative tests in
    positive dirs, fixture-rot, multi-package fixtures, and
    aspirational demos (~50 fixtures).
- When any umbrella issue spawns sub-issues, the sub-issue
  references the same fixtures; closing a sub-issue removes its
  lines from `tools/backend-parity-skips.txt`.
- The largest follow-up is **#622** — the comprehensive LLVM
  audit. Triage this batch into root-cause clusters (probable:
  default-handler installation, runtime_llvm.c helper gaps, fiber
  scheduler nondeterminism, RC discipline divergence) before
  spawning per-cluster fix lanes.
- **#626** is structural rather than code: migrating the ~30
  negative tests under `examples/effects/` and friends to
  `examples/negative/` (or adding sibling `.err.expected`
  goldens) lets the harness shrink its skip list and lets future
  walkers (LSP diagnostics, doc-sync, fixture-coverage) discover
  the categorisation without replicating Makefile loop logic.
- A potential future expansion is to add a third axis: optimization
  level (`-O0` vs `-O2`). The brief explicitly excludes this from
  scope (out-of-scope item 3) — the parity contract is at the
  default build level only.
- If the sweep ever exceeds 60 minutes on CI, the response is to
  parallelize the harness (split DIRS across N shards via a matrix
  job) rather than to relax the gate. The script already produces
  a deterministic entry-point list; matrix shape is purely a CI
  concern.
