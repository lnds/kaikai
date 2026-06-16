# Lane retro: native-default flip (execute)

**Branch**: `native-default-flip`
**Closes**: the three steps the Lane 1.5 design (`docs/lane-experience-native-default-flip.md`) wrote down and deferred. No issue closes — the PR *is* the flip.

## Scope as planned vs. as shipped

Planned (the three deferred steps, now unblocked):

1. Flip the runtime default `c → native` in `bin/kai resolve_backend`.
2. Promote `tier1-native` from a path-gated workflow to an always-on PR gate.
3. Static-link libLLVM into the released `kaic2` so the distributed binary runs the native default with no system LLVM.

Shipped: all three, plus the doc/help-text sweep the flip forces (every `default: c` / "opt-in until parity" line across `bin/kai`, `tier1-native.yml`, and `build-release.sh` flipped to describe native-as-default), plus a stale-smoke fix in `build-release.sh` (the release was still smoke-testing a `llvm` backend that #850 removed).

### Why the premise that blocked Lane 1.5 dissolved

Lane 1.5 designed the flip but did **not** flip, because native-vs-C parity was ~60% (168 gaps at first measurement). It left the infra in place — Makefile capability auto-detect, the wrapper's native wiring, the parametrised parity harness, and the ratchet (`tools/native-parity-baseline.txt`) — and gated the flip on the ratchet reaching empty.

By the time this lane ran, three things had landed:

- The native-parity burn-down closed every gap. `tools/native-parity-baseline.txt` now has **zero** non-comment fixture lines (it is all burn-down narrative). Full corpus parity holds.
- The llvm-text backend was removed (#850), so the backend-parity gate is already native-vs-C only.
- `make llvm-build` (the L0 static-prep scaffolding) is wired and idempotent.

So the flip was no longer a judgment call against partial parity — the gate the design named had gone green. This lane is the mechanical execution the design deferred.

## The load-bearing design decision honoured: bootstrap stays cc-only

The single most important constraint, re-stated from the Lane 1.5 design and respected here:

> Flipping the runtime default must NOT couple the bootstrap to libLLVM.

Two decisions are deliberately separate, and stay separate:

- **Build capability** (does this `kaic2` carry the native backend?) — owned by the Makefile via `llvm-config` auto-detect. A machine with no `llvm-config` builds a C-only `kaic2`; a machine with one builds native-capable. `stage0`/`stage1` are always `cc`-only (Tier 1: bootstrap from any `cc`).
- **Runtime default** (does `kai build` invoke native?) — owned by the wrapper's `resolve_backend`. This is what the lane flips.

The release build is the one place both meet: `build-release.sh` builds `kaic0`/`kaic1` with plain `make` (cc-only) and only `kaic2` with `KAI_LLVM=1`. The bootstrap chain never sees LLVM. We did **not** force `KAI_LLVM=1` as a Makefile default — that would re-import the Rust/Zig-style "the bootstrap needs the backend toolchain" coupling we explicitly reject.

## Step 1 — the graceful-degradation subtlety

The flip is not just "change `c` to `native`" in `resolve_backend`. A `kaic2` bootstrapped without libLLVM (the cc-only path — which every selfhost/tier0/tier1 runner uses, since those gates need no toolchain beyond `cc`) physically cannot run native. If `native` is the default and that `kaic2` aborted, a dev who bootstrapped without LLVM could no longer build anything.

The resolution distinguishes **implicit** default from **explicit** request:

- `resolve_backend` now echoes two words: `<backend> <origin>`, where origin is `explicit` (came from `--backend` / `KAI_BACKEND`) or `implicit` (the default). The default line is `native implicit`.
- `compile_to_binary` carries that origin. In the native branch, when it sees the C-only sentinel (`"not built into this compiler"`, emitted by the `#else /* !KAI_LLVM */` runtime stub):
  - **implicit** → degrade to the C backend with a one-line `kai: note:` and continue. The dev keeps building.
  - **explicit** → error with exit 2, unchanged from before. The user asked for a backend this compiler does not carry; silently downgrading would mask that.

This reuses the existing sentinel mechanism (the runtime stub already distinguishes "no native backend" from a real user compile error), so the only new machinery is threading the origin word. A native-capable `kaic2` never reaches the degradation path — the common case pays nothing.

Verified on a locally-built C-only `kaic2` (all paths):

| path | result |
| --- | --- |
| `kai build hello.kai` (implicit) | degrades to C with note, exit 0, program runs |
| `kai build --backend=native` (explicit flag) | error, exit 2 |
| `KAI_BACKEND=native kai build` (explicit env) | error, exit 2 |
| `kai build --backend=c` (explicit) | works, exit 0, no note |
| `KAI_BACKEND=c kai build` (explicit env) | works, exit 0, no note |
| `kai run … foo bar baz` (implicit) | degrades, forwards args intact |
| `kai test` | C path, untouched by the flip |

The native happy-path (a native-capable `kaic2` building via libLLVM with no flag) is gated by CI's `tier1-native` on Linux with apt's LLVM 18 — locally unreachable on this Apple-Silicon machine because the only system LLVM is Homebrew's 22, whose dynamic link fails on an unrelated `zstd` lookup. The flip changes *when* native is chosen, not the native codegen itself, so CI is the right gate for the native object path.

## Step 2 — tier1-native always-on

`tier1-native.yml` dropped its `pull_request: paths:` filter; it now runs on every PR like `tier1`. The doc-only skip guard stays (a mixed doc+code PR still short-circuits on pure-doc diffs).

The rationale flipped with the default: a path-gated gate was correct while native was opt-in (only diffs to the backend / its prims / the KIR lowering could regress an opt-in path). Now that native is the default, **any** change can regress it — a stdlib edit a native fixture exercises, a wrapper change, a runtime tweak — so the gate must see every PR. The native-vs-C ratchet step (full corpus, `TARGET_BACKEND=native ORACLE_BACKEND=c`) rides along, so backend parity is checked on every PR too. YAML validated with `python -c yaml.safe_load`.

## Step 3 — static-link libLLVM in the release

The shipped `kaic2` must carry libLLVM linked **statically** (Rust/Zig/Julia model): the distributed binary then runs the native default out-of-the-box, and the brew formula needs no `depends_on llvm`.

The Makefile was already ready — `stage2/Makefile` picks `-lc++` (macOS) vs `-lstdc++` (Linux) per `UNAME_S`, and already threads `--link-static` through `--libs` / `--system-libs`. The hardcoded-`-lc++` regression (#834) had already been fixed there. So Step 3 is *wiring*, not new link logic:

- `build-release.sh` now runs `make llvm-build` (idempotent — a cached `build/` is a no-op) to produce the vendored static archives, points `LLVM_CONFIG` at the vendored `build/bin/llvm-config`, and builds `kaic2` with `KAI_LLVM=1 LLVM_CONFIG=<vendored>`. `KAI_LLVM=1` (not auto-detect) so a missing/broken vendored libLLVM breaks the release **loudly** instead of silently shipping a C-only binary that degrades on every user's first `kai build`.
- A new capability check in `build-release.sh` probes the staged `kaic2` with `--emit=native` and FAILS the release if it sees the C-only sentinel — defence against a silent fallback that the C smoke test alone would not catch.
- The release smoke test now exercises `native` (the default) + `c`, replacing the stale `llvm` backend (removed in #850).
- `release.yml` installs `cmake`+`ninja` (release-host tools, NOT stage0 deps), caches the vendored LLVM tree (key: `runner.os` + `LLVM_VERSION` + `hashFiles('Makefile')`, per the L0 plan), and bumps the build job timeout to 90 min for the first cold build after an LLVM bump. Steady state (cache hit) is a no-op.

### Linux static-link reproduction

Per the standing memory that hardcoded `-lc++` broke `tier1-native` on Linux (apt libLLVM is libstdc++-built), the Linux link path was reproduced in `ubuntu:24.04` (Docker, llvm-config 18.1.3): install `clang`+`llvm-dev`, build `kaic2` with `KAI_LLVM=1 --link-static`, confirm it links with `-lstdc++` and carries the native backend, and build a hello via the native default. This validates the platform link line (`UNAME_S=Linux → -lstdc++`). The real release builds darwin-arm64 only (the matrix), but the Linux path is what `tier1-native` (now always-on) hits on every PR, so it is the one most worth a Docker repro.

The first Docker run surfaced a real distinction worth recording: a **minimal** `ubuntu:24.04` + apt `llvm-dev` link failed on `-lz` / `-lzstd` not found, because apt's `llvm-config --system-libs` declares zlib + zstd (its libLLVM is built with them) but the bare container lacks the `-dev` packages. This is NOT a flip regression — GitHub's `ubuntu-latest` ships those libs, which is why `tier1-native` (apt path) passes there without installing them. More importantly, the **release** does not use the apt path: it uses the vendored static build, configured (`Makefile` llvm-configure) with `-DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF`. Those `OFF`s mean the vendored `llvm-config --system-libs` lists only the base libs (`-lrt -ldl -lm -lpthread`) — so the release static link is *more* self-contained than the apt path, not less. The Docker repro was re-run with the apt dev-libs installed to confirm the `-lstdc++` link completes end-to-end.

## Structural surprises the brief did not anticipate

1. **The vendored LLVM tree is gitignored and per-worktree.** `stage0/third_party/llvm/` is populated on-demand by `make llvm-fetch`/`llvm-build` and never committed. A fresh worktree (like this lane's) has no `build/`. This is correct — we never commit LLVM source — but it means the release CI job *must* build (or cache-restore) it; there is no checked-in artefact. The cache step in `release.yml` is what makes the steady state cheap.
2. **The release smoke test had already rotted.** It still ran `run_smoke llvm` against a backend name removed in #850. The flip forced touching that code, which surfaced it. A release with that line would have failed its own smoke the next time it ran. Fixed here as a side effect.
3. **`resolve_backend` returning two words means callers must `set --` to split.** Both `cmd_build` and `cmd_run` now `set -- $(resolve_backend …)` to capture `backend` + `origin`. In `cmd_run` the user's forwarded args had to be re-appended and `shift 2`'d off — verified in isolation that 0-arg, 3-arg, and flag-bearing arg lists all survive.

4. **The degradation note broke `tier1` on the first push — every wrapper diagnostic line must carry the `kai:` prefix.** The first cut wrote the degrade note as a `kai: note:` line followed by an *indented* hint line with no prefix. `tools/test-packages.sh` runs against the C-only `kaic2` the tier1 runner builds (no libLLVM), captures `kai run . 2>&1`, and strips `^kai:` lines to isolate program stdout before the golden compare. The prefixed line was filtered; the bare indented hint leaked into `actual` and broke 8 package fixtures. Fix: prefix **every** line of both the degrade note and the explicit-native error with `kai:`. The deeper lesson: the degradation path is the one place the wrapper emits diagnostics on a *successful* build (a C-only `kaic2` building fine, just on the other backend), so it is the one place where unprefixed chatter contaminates captured output — every other wrapper message rides an error path that already aborts the compare. The harnesses that compare *error* goldens (`test-negative.sh`, the Makefile dump-* targets) call `./kaic2` directly, never the wrapper, so they never see the note — only wrapper-driven harnesses (package / doc / grammar / info) can, and of those only `test-packages` compares program stdout (the rest check build exit code).

## Fixtures added

None new. The flip is gated by the *existing* corpus through a stronger lens:

- `tier1-native`'s full-corpus native-vs-C ratchet now runs on **every** PR (Step 2), so the entire fixture corpus is the regression net for the native default.
- `build-release.sh`'s new capability check + native smoke is the release-side fixture for "the shipped binary really carries native".

A behavioural fixture for the *degradation* path (C-only `kaic2` + implicit default → C with note) is the one coverage gap: it needs a C-only `kaic2`, which the gates do not currently build a fixture around. The degradation was verified by hand (table above); a CI fixture would need a dedicated C-only-build job. Left as a follow-up rather than bolted on, since the gates already build C-only `kaic2`s for selfhost and could assert the wrapper note there.

## Real cost vs. estimate

Small. The design work was done by Lane 1.5; this lane is execution. The bulk of the time went to (a) the graceful-degradation threading (the one piece with real logic), (b) the doc/help-text sweep, and (c) the Linux Docker repro. No new codegen, no compiler change — `stage2`/`stdlib` are untouched.

## Follow-ups left for next lanes

These were blocked on native-as-default + a statically-linked `kaic2`, and L3 unblocks both:

- **#500 (L6: release/debug build modes + symbol/debuginfo emission).** DWARF emission via the in-process libLLVM path needs native to be the real build path, not an opt-in. Now unblocked.
- **#499 (L5: LLVM bitcode cache, stdlib + user modules).** A bitcode cache is only worth building once native is the default destination and the static libLLVM is the link path. Now unblocked.
- **Degradation CI fixture.** Assert the `kai: note:` on a C-only `kaic2` inside the existing C-only selfhost build, so the implicit-degrade path has a gate, not just a hand-check.
- **Release size measurement.** L0 estimated ~50-120 MB on top of the current ~5 MB for the static link. The first real release with this change should record the measured linked-`kaic2` footprint (the L0 retro left a slot for it).
