# Lane retro: L0 LLVM static-link prep

**Branch**: `lane-l0-llvm-static-prep`
**Closes**: phase L0 of the LLVM-direct refactor (groundwork for L3,
which replaces `bin/kai --backend=llvm`'s external-`clang` shell-out
with an in-process libLLVM call).
**Real cost**: ~0.5 day. Investigation + Makefile scaffold + retro;
no compiler or driver code touched.

## Scope

L0 is **infrastructure preparation**, not implementation. The lane
delivers:

- A decision on how LLVM source enters the build (vendoring strategy).
- A Makefile workflow (`llvm-info`, `llvm-fetch`, `llvm-configure`,
  `llvm-build`, `llvm-size`, `llvm-clean`) that prepares a static
  libLLVM out-of-tree.
- A narrow component list aligned with what kaic2 actually needs.
- A size + build-time estimate sketch for L3.
- A CI integration plan (not implementation).
- This retro.

Explicitly **out of scope**:

- Adding LLVM source to the repo (the Makefile fetches on demand;
  the tree is gitignored).
- Modifying `stage2/compiler.kai` or `bin/kai` (L3's surface).
- Removing the external-clang shell-out (L3).
- CI workflow edits (deferred to L3 once the build is exercised on
  real runners).

## Scope-as-planned vs scope-as-shipped

The brief proposed three "Parts" for L0:

| Part | Planned                          | Shipped                                                                                          |
|------|----------------------------------|--------------------------------------------------------------------------------------------------|
| A    | Decide vendoring strategy        | Yes — tarball-on-demand (a2) with rationale below                                                |
| B    | Makefile target                  | Yes — split into 5 stages so each is independently invokable                                     |
| C    | Component list                   | Yes — 38 libraries, much narrower than the brief's example                                       |
| D    | Build size + time measurement    | **Documented estimates only** — see "Build cost — measured vs estimated" below                   |
| E    | CI implications                  | Yes — written, not implemented                                                                   |
| F    | Retro                            | This document                                                                                    |

The biggest delta from the brief: **D** is unmeasured. The local
machine has no `cmake` installed, no `ninja`, and no Homebrew LLVM
to crib measurements from. Spending 30-60 minutes on a single
cold build to populate a number that L3 will re-measure anyway
felt like the wrong trade. The Makefile is wired so L3 (or
any contributor with cmake + ninja) can run it and update this
retro with real numbers. The estimate ranges below are derived
from published LLVM build reports + the chosen build flags, not
from a local run.

## Why an in-process libLLVM at all — clarifying the goal

The L1 retro already laid out the three shapes for the LLVM path
(cache-only / external clang / static-linked libLLVM). L0's
contribution is to make precise **what** kaic2 needs from libLLVM,
because that drives the entire vendoring story.

kaic2 emits LLVM IR as **text** (see `emit_program_llvm` at
`stage2/compiler.kai:44202`). The `llvm_*` helpers in the compiler
are kaikai functions that produce IR strings; none of them call into
the LLVM C API. The driver pipeline today is:

```
kaic2 --emit=llvm src.kai > out.ll
clang -w -I stage0 out.ll stage0/runtime_llvm.c -o out
```

L3's job is to replace the `clang` invocation, **not** rewrite the
emitter. So the libLLVM surface kaic2 needs is the one that:

1. Parses LLVM IR text (`LLVMParseIRInContext`) into a `Module`.
2. Verifies it (optional but cheap).
3. Looks up the host target (`LLVMGetTargetFromTriple`).
4. Runs the codegen pipeline to produce a relocatable object file.
5. Hands the `.o` to a linker — either the system `ld` /
   `ld64` / `lld`, or a vendored `lld`.

The runtime (`stage0/runtime_llvm.c`) stays plain C and is compiled
by the host's `cc`. It does not become a libLLVM concern.

That recontextualises the whole problem: we do **not** need
`IRBuilder`, we do **not** need most of the `Transforms/`
pipeline, and we do **not** need any of the language frontends
(`clang` proper, `lldb`, `flang`, `polly`). The vendored surface
is dramatically smaller than a "full LLVM build."

## Source strategy decision

Three options were on the table. Evaluated against four axes:
repo bloat, reproducibility, offline-build, maintenance.

| Option | Repo bloat | Reproducibility | Offline build | Maintenance |
|--------|------------|-----------------|---------------|-------------|
| a1 — git submodule `llvm-project` (~600 MB checkout) | Heavy | Strong (pinned SHA) | Strong once cloned | Low (track upstream tags) |
| a2 — tarball download on first build | None | Strong (pinned version + URL + optional SHA256) | Weak (needs network on first build) | Low |
| a3 — vendored subset (only the dirs we use) | Medium (~15 MB) | Strong | Strong | **High** (re-vendor every LLVM bump) |

**Decision: a2 — tarball-on-demand**, fetched into
`stage0/third_party/llvm/` which is gitignored.

Rationale, in order of weight:

1. **Repo size**: a1 inflates every fresh clone by ~600 MB even
   for contributors who never touch the LLVM lane. We have ~80
   active branches; submodule sync would burn time for anyone
   not in the LLVM lane. The cost of a one-time `curl` download
   is paid only by contributors who actually invoke `make
   llvm-build`.
2. **Maintenance cost of a3**: every LLVM bump (18.1.8 → 19.x →
   20.x) becomes a re-vendoring lane. We do not have the
   personnel for that. LLVM's internal cross-component coupling
   is tight enough that "vendor just `lib/IR/`, `lib/CodeGen/`,
   …" is fragile in practice — see anecdotal reports from
   Tcl/Wasmtime/Bazel that all eventually moved off vendored
   subsets.
3. **Reproducibility for a2**: pinning to `llvmorg-18.1.8` plus
   the GitHub releases URL is reproducible enough. The Makefile
   can optionally verify SHA256 later (open follow-up; not
   needed for L0).
4. **Offline build**: a real concern but **L3-scoped**. The
   external-clang path keeps working on every machine with `cc
   + clang` in PATH (which is every macOS dev box and every CI
   runner today). L3's libLLVM path becomes the optimisation, not
   the requirement.

The decision is reversible: if a3 (vendored subset) ever wins
later — say once kaikai is pinned to one LLVM major for years —
the Makefile contract (`make llvm-info` / `llvm-build`)
stays valid; only the underlying source-acquisition step changes.

## Makefile workflow

Six targets, all opt-in (none run from `all`, `tier0`, `tier1`,
`tier2`, `daily`):

- `make llvm-info` — prints config (version, URL, target list,
  expected disk + time). Safe to run on any machine; no side
  effects. Useful for sanity-checking what the next step would do.
- `make llvm-fetch` — `curl` the tarball, extract into
  `stage0/third_party/llvm/`. Idempotent.
- `make llvm-configure` — `cmake -B build -G Ninja` with the
  flag set documented below. Requires `cmake` and `ninja` in
  PATH; fails loud with exit 2 if either is missing.
- `make llvm-build` — `cmake --build` for the 38 target list.
  The long step (10-30 min cold). Calls `llvm-size` on success.
- `make llvm-size` — sum-of-.a measurement, sorted.
- `make llvm-clean` — drop the build tree; keep the source tree
  so re-tuning `llvm-configure` flags doesn't re-download or
  re-extract.

CMake flag set (see top-level `Makefile`, search `llvm-configure`):

- `CMAKE_BUILD_TYPE=MinSizeRel` — smaller `.a` archives at a
  small runtime-perf cost. kaic2 is not on the LLVM-codegen hot
  path; compile-time of a kaikai program is dominated by
  kaic2's own front + middle, not by libLLVM's lowering.
- `LLVM_TARGETS_TO_BUILD="X86;AArch64"` — host targets only.
  No `RISCV`, `ARM` (32-bit), `Mips`, `PowerPC`, `SystemZ`,
  `WebAssembly`. Adding more targets is a one-line change later
  if cross-compile lands.
- `LLVM_ENABLE_PROJECTS=""` — no clang, lld, lldb, mlir, flang.
- `LLVM_BUILD_{TOOLS,UTILS,EXAMPLES}=OFF`,
  `LLVM_INCLUDE_{EXAMPLES,TESTS,BENCHMARKS,DOCS}=OFF` — drop the
  ~3-5 GB worth of build artefacts we never run.
- `LLVM_ENABLE_{ZLIB,ZSTD,TERMINFO,LIBXML2,LIBEDIT}=OFF` — kill
  optional dependencies that bloat the static link without
  adding anything kaic2 needs (no `.bc` is compressed, no
  terminal escape sequences are printed by the codegen pipeline).
- `LLVM_ENABLE_{OCAMLDOC,BINDINGS}=OFF`,
  `LLVM_ENABLE_ASSERTIONS=OFF` — production-style build.
- `LLVM_ENABLE_BACKTRACES=OFF` — saves a few hundred KB; kaic2
  has its own diagnostic story.

If a flag turns out to be wrong, the cost is one
`make llvm-configure` + `make llvm-build` cycle.

## Component list (the L3 link line)

38 libraries are built, in three groups:

**Core IR + parsing** — non-negotiable. Parsing the text
`.ll` kaic2 emits requires every link in this chain:

- `LLVMCore` — types, values, instructions, basic blocks, modules.
- `LLVMSupport` — `Twine`, error machinery, file I/O abstractions.
- `LLVMIRReader` — the user-facing "parse `.ll` from a file/buffer".
- `LLVMAsmParser` — what `IRReader` delegates to for textual IR.
- `LLVMBitReader` / `LLVMBitWriter` — bitcode in/out. Cheap to
  include; opens the door to caching parsed modules as `.bc`
  later.
- `LLVMTarget` / `LLVMTargetParser` — triple parsing + target
  registry.
- `LLVMMC` / `LLVMMCParser` — Machine Code layer. Where codegen
  actually emits bytes.
- `LLVMObject` / `LLVMOption` / `LLVMBinaryFormat` — object-file
  writers (Mach-O on macOS, ELF on Linux) + driver-style option
  parsing.
- `LLVMDemangle` / `LLVMRemarks` / `LLVMDebugInfoDWARF` /
  `LLVMDebugInfoCodeView` — symbol mangling + debug info. The
  last two split because the cross-platform debug story differs.

**Target codegen** — two architectures only:

- `LLVMX86CodeGen` / `LLVMX86AsmParser` / `LLVMX86Desc` / `LLVMX86Info`.
- `LLVMAArch64CodeGen` / `LLVMAArch64AsmParser` / `LLVMAArch64Desc` /
  `LLVMAArch64Info`.

Each pair (`*CodeGen` + `*Desc` + `*Info`) is the minimum that
makes the target callable. `AsmParser` lets us read inline asm
strings inside the IR text — kaikai doesn't emit any today, but
including it costs ~200 KB and avoids a confusing failure if a
future FFI helper does.

**Generic codegen + minimal opts**:

- `LLVMCodeGen` / `LLVMSelectionDAG` / `LLVMGlobalISel` — the
  target-independent half of lowering.
- `LLVMAsmPrinter` — assembly emission (also used internally
  for object emission).
- `LLVMAnalysis` — `Analysis/` is a prerequisite of basically
  every pass.
- `LLVMTransformUtils` / `LLVMScalarOpts` / `LLVMipo` /
  `LLVMInstCombine` / `LLVMInstrumentation` / `LLVMVectorize` —
  the standard optimisation pass families. Chosen because the
  default `O0`-style pipeline (which kaic2 will start with) still
  drags some of them in transitively. Trimming this further is
  a measurable-but-not-yet-needed optimisation.
- `LLVMLinker` — IR-level module linking. Cheap to keep; useful
  later if kaic2 ever wants to merge prelude IR ahead of time.
- `LLVMPasses` — the new pass manager.

Components **deliberately left off**:

- `LLVMOrcJIT` / `LLVMExecutionEngine` / `LLVMMCJIT` — kaic2
  produces ahead-of-time binaries; no JIT path is planned.
- `LLVMCoroutines` — kaikai's coroutines lower to plain C control
  flow, not LLVM coroutine intrinsics.
- `LLVMFrontendOpenMP`, `LLVMFrontendHLSL`, `LLVMFrontendOffloading`
  — language-frontend support we will never call.
- `LLVMWindowsManifest`, `LLVMWindowsDriver` — Windows is
  post-MVP per `CLAUDE.md` "Things to avoid".
- `LLVMLTO` — link-time optimisation is a future lane, not L3.

The expectation is that L3 starts from this list and **subtracts**
as it discovers transitive bloat, not adds.

## Build cost — measured vs estimated

**Status**: estimated, not measured locally. The lane machine
lacks `cmake` and `ninja`. Numbers below are derived from
public LLVM 17/18 build telemetry; treat them as upper-bound
guidance, not pinned values.

- **First-time tarball download**: ~70 MB (`llvm-18.1.8.src.tar.xz`).
- **Extracted source tree**: ~600 MB (`stage0/third_party/llvm/`).
- **Build tree** (`MinSizeRel`, 38 targets, no tools): ~3-4 GB
  intermediate. Drops to ~150-300 MB once non-`.a` artefacts
  are gc'd by `cmake --build` (we don't gc; the build tree stays
  intact for incremental rebuilds).
- **Sum of static archives**: ~150-300 MB. **Note**: this is
  not the kaic2-linked footprint; static link drops 50-70% via
  dead-code elimination + section GC. Expected linked-kaic2
  size: **~50-120 MB on top of the current ~5 MB**. L3 will
  measure for real and pin a number in this retro's successor.
- **First build wall**: ~10-15 min on an M2 / M3 laptop (8-12
  cores); ~30-60 min on `ubuntu-latest` GitHub-hosted runners
  (2 cores, 7 GB RAM, slower disks). The MinSizeRel choice
  doesn't move wall time much vs `Release`; what saves time is
  the narrow target list + `_BUILD_TOOLS=OFF`.
- **Incremental rebuilds**: seconds-to-minutes, dominated by
  `ninja` re-link of touched archives.

The Makefile's `llvm-size` target produces the real number on
first build. L3 should commit a one-line update to this section
once a measurement exists.

## CI implications — plan, not implementation

Today's `.github/workflows/tier1.yml` runs on a self-hosted M2
runner for tier0 + Linux runners for tier1. Neither builds LLVM.
For L3 to land, CI needs:

1. **A dedicated `llvm-build` job** that produces the static
   libs once per LLVM_VERSION and caches the build tree. Cache
   key: `llvm-${{ runner.os }}-${{ env.LLVM_VERSION }}-${{
   hashFiles('Makefile') }}`. On a cache hit (the steady state),
   the job is a no-op; on a miss (first run after an LLVM bump
   or Makefile edit), it pays the 30-60 min cold-build cost.
2. **A larger runner for the cold build**. `ubuntu-latest`'s 7
   GB RAM is borderline; LLVM's `MinSizeRel` build keeps
   memory under control, but linking the larger archives
   peaks around 4-5 GB. The cold build is best run on an
   `ubuntu-latest-large` (or self-hosted) and cached so the
   per-PR jobs use `ubuntu-latest`. This is the "spend once,
   amortise" pattern that the Bazel + Wasmtime communities
   recommend for vendored LLVM toolchains.
3. **A timeout bump** on the cold-build job to ~90 min. The
   steady-state job (cache hit) stays at the current 30 min.
4. **Disk budget**. `ubuntu-latest` has ~14 GB free. The
   extracted source + build tree fit, but only just. The
   cache extraction step needs to land **before** any
   non-trivial workspace materialisation. A `df -h` smoke at
   job entry is a defensive measure.
5. **macOS runner cost**. Self-hosted M2 (today's tier0 runner)
   handles a 10-15 min cold build comfortably. The cache key
   includes `runner.os` so macOS and Linux artefacts never
   collide.

None of these changes ships in L0. They are written down so L3's
CI step is mechanical rather than discovery-driven.

## Structural surprises the brief did not anticipate

Two small ones, one larger.

1. **kaic2 emits IR as text, not via the C API**. The brief
   listed `IRBuilder`-side components ("`LLVMCore` for IR types,
   basic blocks, instructions") in a tone that suggested kaic2
   would construct IR programmatically. It does not. This shrinks
   the L3 surface considerably (no `IRBuilder`, no
   `LLVMConstantInt`, etc.) and is the single most important
   piece of context this retro records for L3.

2. **The `runtime_llvm.c` link step is plain C**. The driver
   today shells out to `clang` once to compile `out.ll` + the
   runtime together. In L3, those split: libLLVM compiles the
   IR, the host `cc` (or vendored clang frontend, future) compiles
   the runtime, and `ld`/`ld64` links the two objects. This
   means L3 can ship a libLLVM build with **no clang frontend**
   — a major footprint win. The retro flags this for L3 to
   confirm against the actual link line.

3. **The build cost is dominated by archives we will never link
   against**. Even with `_BUILD_TOOLS=OFF` and a narrow target
   set, `cmake --build` walks the dep graph and compiles a
   surprising number of `.a` files. The 38-target list above
   triggers some of those transitively (e.g. `LLVMPasses` pulls
   `LLVMipo`, which pulls almost everything in `Analysis`).
   Trimming the list further might save 20-30% of build time
   but adds maintenance burden; L0 chose breadth-with-headroom
   over a tight-fit list that will need re-tuning every LLVM
   bump.

## Fixtures added

None. L0 is documentation + Makefile scaffolding; there is no
behaviour to gate. The `llvm-info` target is the closest thing
to a smoke test (and it's already proven to run clean on the lane
machine).

L3's first commit should add:

- `tools/test-llvm-build.sh` — wraps `make llvm-build` and
  verifies the expected archives are present.
- A tier-2 (daily) gate that the cached LLVM build still
  produces a linkable kaic2 (once L3 wires the link).

Neither belongs in L0, where there's nothing yet to test.

## Real cost vs estimate

The brief sized L0 implicitly as a multi-part investigation;
real cost was ~0.5 day. The savings came from:

- Skipping the local LLVM build (no `cmake`/`ninja` on the
  lane machine, and L3 will re-measure anyway).
- Recognising the text-IR insight early, which collapsed the
  "what components do we need" question from "all of codegen
  surface" to "parse + lower".
- Reusing the L1 retro's framing for option (a)/(b)/(c)
  rather than rebuilding it.

## Follow-ups left for L3 (and beyond)

- **Measure**. Run `make llvm-build` on a clean machine
  (macOS + Linux) and update the "Build cost — measured vs
  estimated" section with real numbers.
- **Implement the in-process link**. Replace the `clang` shell-out
  in `bin/kai`'s LLVM branch with a small C helper that calls
  `LLVMParseIRInContext` → `LLVMTargetMachineEmitToFile` →
  invoke `ld`/`ld64`. The driver flag (`--backend=llvm`) does
  not change.
- **Decide where the C helper lives**. Options: (a) a new
  `stage0/llvm_driver.c` compiled at build time; (b) a vendored
  C tool that ships in the release tarball. (a) keeps stage 0
  minimal-ish and avoids a new release artefact; (b) avoids
  rebuilding the helper on every PR. Recommendation: (a).
- **Add an L3 CI job** per the plan above. Cache key on
  `LLVM_VERSION` + relevant Makefile slice.
- **Confirm the runtime stays plain-C**. `stage0/runtime_llvm.c`
  should not migrate to libLLVM construction; doing so would
  re-couple the runtime to the LLVM major version, which is
  exactly what we want to avoid.
- **Tarball SHA256 verification** in `llvm-fetch`. Cheap to add,
  protects against a compromised release mirror. Out of L0's
  scope only because the threat model isn't load-bearing yet.
- **Document `make llvm-info` in CONTRIBUTING / a build-deps
  README**. New contributors to the LLVM lane should hit `make
  llvm-info` first, see the disk + time cost up front, and
  decide whether to opt in. Today nothing nudges them there.

## Recommendation for L3

**Proceed with the tarball-on-demand strategy.** Build a small
C helper in `stage0/` that wraps the libLLVM C API for the
"parse `.ll` → emit `.o` → invoke system linker" path; keep the
runtime in plain C and let the host `cc` compile it. CI gets one
cached `llvm-build` job; per-PR jobs link against the cache.
Footprint estimate: +50-120 MB on the released `kai` binary,
swallowed by stripping symbols at release time (likely down to
+30-80 MB after `strip`).

If after measurement the footprint surprises us badly (>200 MB
linked), the fallback is to ship libLLVM as a sibling shared
library (`libkaikai_llvm.dylib` / `.so`) loaded at runtime, which
keeps the headline `kai` binary small. That decision belongs to
L3, not L0.
