# Lane experience — native-default flip (KIR Lane 1.5)

## Scope as planned vs as shipped

**Planned (brief):** flip `kai build`'s default to the in-process libLLVM
native backend. Steps: (1) run native-vs-C parity over the 388-fixture
corpus, fix/document gaps; (2) flip the default; (3) promote `tier1-native`
to an always-on PR gate; (4) static-link libLLVM in the release. The brief
asserted the native backend "emits the whole language" — effects with
stateful+alias handlers, defaults, RC/Perceus, TRMC, match, closures —
since Lane 1.2 had just closed (#805).

**Shipped:** the flip did **not** proceed. The premise was false for the
general corpus. Measured native-vs-C-direct parity over the real corpus
(513 fixtures, not 388 — it grew): **~250 pass / 168 fail / 96 skip**, ~60%
(167 deterministic gaps + 1 flaky; see the flakiness surprise below).
The brief's own gate — "the flip does NOT proceed with incomplete parity" —
fired. What shipped instead is the **infrastructure** that the flip needs,
plus a burn-down ratchet that locks progress toward it:

1. `stage2/Makefile` — `KAI_LLVM` capability auto-detection (`llvm-config`
   present → native-capable; absent → C-only) with an announce line. Keeps
   `tier0`/`tier1` green on an LLVM-less runner (they build a C-only kaic2),
   while a dev/CI with libLLVM gets native automatically.
2. `bin/kai` — the `native` backend fully wired (parser, `resolve_backend`,
   `compile_to_binary` link path via `runtime_llvm.c`, an actionable error
   when a C-only kaic2 is asked for native). **The default stays `c`**;
   `native` is reachable opt-in via `--backend=native` / `KAI_BACKEND=native`.
3. `tools/test-backend-parity.sh` — parametrised on `TARGET_BACKEND` /
   `ORACLE_BACKEND`, so the existing #575 C↔LLVM-text gate and a new
   native-vs-C gate share one harness. Plus a `NATIVE_PARITY_RATCHET=1`
   mode that gates against `tools/native-parity-baseline.txt`.
4. `tools/native-parity-baseline.txt` + `docs/native-parity-gaps.md` — the
   167 allowed gaps (ratchet) and the same set grouped by root-cause family
   (burn-down input).

## Design decisions and alternatives considered

**The two load-bearing architecture calls (validated with asu):**

- *Capability vs runtime-default split.* The Makefile decides what backend
  is *compiled into* kaic2; the wrapper decides what backend *runs*. We
  rejected flipping the Makefile default to `KAI_LLVM=1` (asu's first
  instinct, retracted): `make tier0`/`tier1` build kaic2 on a runner with
  no LLVM, so a hard `KAI_LLVM=1` default would force `llvm-config` into
  every bootstrap CI job — reimporting the Rust/Zig "bootstrap coupled to
  LLVM" pain. The bounded R2 (auto-detect capability, never touch the
  runtime default, announce which capability was built) keeps the bootstrap
  invariant intact.
- *No silent fallback.* When a C-only kaic2 is asked for `native`, the
  wrapper emits an actionable error and **exits** — it does NOT fall back to
  C. A hot fallback would mask a real native codegen bug as a green build,
  the project's "no false-green parity" rule. The native CI job must also
  *verify* `llvm-config` is present and fail loud if not, so a deleted
  `apt install llvm-dev` cannot silently downgrade "native tested" to
  "native untested but green".
- *Parity harness driven by the wrapper (not a hand-link replica).* The
  gate runs `KAI_BACKEND=native kai build` vs `KAI_BACKEND=c kai build`,
  exercising the exact link path a user runs. `examples/native/` (14) stays
  as a separate curated smoke (`tools/test-native-parity.sh`).

**Ratchet shape.** Rather than gate on a bare count (the
`coverage-baseline.txt` pattern), the native ratchet gates on the *set* of
failing fixtures: a new failure that is not in the baseline fails CI even if
some other gap closed in the same diff (which would leave the count flat).
The baseline only tightens — closed gaps are prompted for removal, new gaps
are rejected. This locks the burn-down so a lane can only move the number
DOWN.

## Structural surprises the brief did not anticipate

1. **"Emits the whole language" meant 14 curated fixtures, not the corpus.**
   Lane 1.2's green `tier1-native` (14 `examples/native/*.kai`) read as
   full coverage. The corpus tells a different story: the native backend
   self-labels most build failures `unsupported KIR node (subset gap): …`.
   It is, by its own admission, a *subset* walk. The lesson is the recurring
   one in this repo (the memory note on it): a curated-fixture green is
   false-green for "does it emit everything" without a corpus oracle.
2. **All 11 `demos/` core programs fail parity** (collatz, factorials,
   fizzbuzz, quicksort, …) — the most basic programs, not exotic ones. The
   gaps are foundational (register binding, builtin symbols), not edge
   cases.
3. **The corpus grew to 513**, so the brief's "388" was already stale; the
   harness walks the documented example dirs + demos, which is the right
   denominator.
4. **`KAI_BACKEND=llvm` is a third, *different* backend** (LLVM-text +
   clang), declared dead-path in §648 of the design but still load-bearing
   for the #575 gate. Adding `native` made three backends with one dead —
   we kept `llvm` reachable (its gate is live) and made the help/`resolve`
   tell one coherent story (c = oracle/default, native = intended
   destination/opt-in, llvm = legacy text).
5. **Some native gaps are FLAKY, which a naive set-equality ratchet can't
   gate.** The first ratchet self-test failed on `hex_basic.kai` as a
   "new gap" — it diverges ~50% of runs (intermittent SIGSEGV; output
   carries raw pointers / non-deterministic RC). Three back-to-back full
   runs otherwise agreed exactly (167 == 167), so flakiness is rare but
   real. A strict ratchet would fail CI at random on those. Fix: (a) list
   the known flaky fixture in the baseline (intermittent parity is still a
   gap), and (b) the ratchet re-verifies a candidate new gap up to 3× and
   only flags a regression when it diverges on EVERY attempt — a sub-100%
   flaky fixture always clears, a deterministic regression always survives.
   The flakiness itself is a backend bug (non-deterministic codegen), a
   burn-down item in its own right.

## Gap families (the burn-down — full list in `docs/native-parity-gaps.md`)

| Family | Count |
|---|---:|
| unbound-register | 35 |
| runtime-type-mismatch | 27 |
| missing-symbols | 24 |
| build-failed-other | 24 |
| output-mismatch | 20 |
| SIGSEGV/SIGABRT | 12 |
| exit-code-mismatch | 11 |
| no-effect-handler (Spawn 7 / Clock 2 / NetTcp 1) | 10 |
| timeout | 2 |
| clause-param-origin | 2 |

The largest family, `unbound-register` (35), is a single KIR-walk lowering
class — likely a high-leverage first burn-down lane (one fix may close many
fixtures). `runtime-type-mismatch` (27) and `missing-symbols` (24) are the
box/unbox-discipline and builtin-emission frontiers respectively.

## Fixtures added / coverage gaps

- `tools/native-parity-baseline.txt` — the 167-fixture ratchet, gated by
  `tier1-native` in `NATIVE_PARITY_RATCHET=1` mode. This IS the regression
  fixture for this lane: it locks the gap set so no lane can regress it.
- `docs/native-parity-gaps.md` — categorized burn-down list.
- No new positive fixtures: the lane shipped infra, not a feature; the
  ratchet is the test wiring.

## Real cost vs estimate

The brief budgeted a flip; the work was a *measurement* that refuted the
flip's precondition, plus the infra to make the refutation a durable,
ratcheted artifact. The measurement (two full corpus parity runs) dominated
wall-clock. The architecture calls were settled up front with asu before any
code, which avoided a churny CliOptions change (a rejected alternative for
the object-output path — solved instead by compiling a `$tmp` copy so the
`.o` never lands in the user tree).

## Follow-ups for next lanes

- **Burn-down lanes**, by family, gated by the ratchet (each closes a slice,
  removes those lines from `native-parity-baseline.txt`):
  1. `unbound-register` (35) — one KIR-walk lowering class.
  2. `missing-symbols` (24) — builtin/runtime symbol emission in the object.
  3. `runtime-type-mismatch` (27) — native box/unbox discipline.
  4. `no-effect-handler` (10) — Spawn/Clock/NetTcp handler installation.
  5. crashes/timeouts (14) — SIGSEGV + infinite-loop codegen.
- **The flip (Lane 1.5 proper)** re-runs only when
  `tools/native-parity-baseline.txt` is empty (full corpus parity). At that
  point the deferred steps land: flip the default in `resolve_backend`,
  promote `tier1-native` to always-on, static-link libLLVM in the release.
  All three are wired-but-dormant in this lane's diff comments.
