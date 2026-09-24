# Build system — Makefile structure & how to invoke the compiler

Practical map of the build. Read this before running the compiler or touching a Makefile — it removes the two recurring time-sinks: not knowing how to invoke the compiler, and getting lost in the recursive Makefiles.

## TL;DR — the commands you actually need

| You want to… | Command (from repo root) |
|---|---|
| Run / build a `.kai` program | `./bin/kai run <file.kai>` · `./bin/kai build <file.kai> -o <out>` |
| Rebuild the compiler after editing `stage2/compiler/*.kai` | `make kaic2-fast` (dev, needs an existing kaic2) · `make kaic2` (bootstrap, C) · `make KAI_LLVM=1 kaic2` (native) · `make kaic2 KAIC_BOOT=auto` (no kaic1; see §KAIC_BOOT) |
| Full bootstrap from scratch | `make all` (→ `kaic0` → `kaic1` → `kaic2` → `bin/kai`) |
| Verify a change | `make tier0` (fast) · `make tier1` (full, CI gate) |
| One backend-parity fixture | `tools/test-backend-parity.sh` (env-driven; see §parity) |

**Do NOT** call `kaic2` raw, pass `--path ../stdlib` by hand, or reconstruct a `cc … -I ../stage0` line from Makefile recipes. `bin/kai` does all of that. **Do NOT** compile `stage2/main.kai` as "the compiler" — it is a 33-line stub (see §package).

## `bin/kai` — the entry point

`bin/kai` is the `kai` binary, written in kaikai under `tools/kai/` and built by `make` (`make kaic2` and `make bin/kai` build it too; it is not checked in). It is the ONLY thing you should use to run/build kaikai code. It resolves, automatically:
- the stdlib path (`--path`),
- the backend (native by default since the Lane 1.5 flip; force with `--backend=c` or `KAI_BACKEND=c`),
- the `cc`/link step.

```
./bin/kai run hello.kai                  # build + execute, zero manual flags
./bin/kai build app.kai -o build/app     # just build
./bin/kai test ./...                     # walk every kai.toml, compile --test, run
```

A C-only `kaic2` prints `note: native backend unavailable … using the C backend` and falls back — harmless. Subcommands: `build`, `run`, `test`, `bench`, `check`, `typecheck`, plus `--holes-json` / `--diags-json` / `--effects-json` for structured output.

`./bin/kai typecheck <file.kai>` is the fast edit-loop answer to "does this compile?": it runs the full front-end (resolve + infer + protocol/kind/effect checks) and stops — no monomorph, no codegen, no `cc`, no link, so it behaves identically on a C-only and a native `kaic2`. Front-end diagnostics and exit code are identical to a build's (gate: `make test-check-parity`); errors that only surface at monomorphisation or in a backend subset gap are out of its scope by design. The JSON report flags ride it (`kai typecheck f.kai --diags-json`).

### The kai binary (`tools/kai`)

Every command lives in the binary. It finds its installation from its own
path — `argv[0]`, searched on `PATH` when bare, symlinks followed — as
`<root>/bin/kai`: a checkout when `<root>` holds `stage0/` and `stdlib/`,
an installed prefix when it holds `libexec/kaikai/kaic2` and
`share/kaikai/stdlib/`. An unknown verb runs as a `kai-<verb>` plugin
from `tools/kai/plugins/` (installed: `libexec/kaikai/plugins/`; `upgrade`
lives there) or from `PATH` (`kai info install`). `tools/kai/Makefile`
builds it by driving `kaic2` directly; a checkout rebuilds it with `make
bin/kai` after editing `tools/kai/`, a release ships it as `bin/kai`. The
bootstrap never goes through it: the Makefiles keep invoking `kaic2` with
their own flags, and CI builds it from the restored `kaic2` in
`tools/ci-touch-build.sh`. In a checkout with no `kaic2` it runs `make -C
stage2 kaic2`, which builds kaic0/kaic1 only if the boot needs them
(§KAIC_BOOT); an existing `kaic2` is used as is. Gate: `make test-kai-cli`.

### The shared core cache

Every `bin/kai` build passes `--core-cache-dir` + `--toolchain-id` to
kaic2, enabling two persistent, content-addressed cache layers for the
auto-loaded stdlib core under `~/Library/Caches/kai/core/<toolchain-id>/`
(Linux: `~/.cache/kai/core/…`; `XDG_CACHE_HOME` honoured):

- **post-parse blobs** (KAB2, one per core module) — a warm build skips
  the core's lex+parse entirely;
- **emitted-C core TU bodies** (KCT1, c-modular backend, plain builds
  only) — a warm build splices the 13 core `.c` bodies into the marker
  stream instead of re-emitting them;
- **the native core object** (`ncore-<key>.o`, native backend) — the
  whole-program native build splits into a user object plus one prebuilt
  object carrying the entire auto-loaded core (fns, their thunks, their
  boxed proto adapters, core-lifted lambdas); a warm build links the
  stored object and feeds LLVM only the user partition. The key folds
  the projected core KIR, the toolchain id, the edition, the target
  triple, the opt level, the runtime-bitcode ids, and the core source
  hashes. `KAI_NATIVE_CORE_OBJ=0` disables just this layer; `--debug`
  (non-default opt level) skips it automatically. Test:
  `make -C stage2 KAI_LLVM=1 test-native-core-obj`.

The directory embeds the toolchain id (kaic2 mtime+size), so a rebuilt
compiler never reads another build's entries; entries are additionally
keyed on core source content + edition, so a stale hit is impossible —
any mismatch is a miss that falls back to the full compile. The first
build populates the cache (lazy warm); `make warm-core` pre-warms it
explicitly (useful as an install or CI-init step). Knobs:
`KAI_CORE_CACHE=0` disables, `KAI_CORE_CACHE_DIR` moves the root,
`KAI_CORE_CACHE_STATS=1` prints per-layer hit/miss lines. Raw `kaic2`
invocations (Makefile gates, selfhost, parity) pass no cache flags and
are byte-for-byte unaffected. Fixtures: `examples/cache/emitc_*.sh` +
`corec_*.sh` (kaic2 invoked directly) and `wrapper_backend_hit_stats.sh`
(same gate through `bin/kai build`, C and native backends), run by
`make -C stage2 test-core-cache` (tier 1).

**Stats line contract.** With `KAI_CORE_CACHE_STATS=1`, kaic2 prints
one line per cache layer to stderr, prefixed `kaic2: <layer>: `. The
only layer wired to `--core-cache-stats` today is the core-parse
cache: `kaic2: core-parse-cache: hit (N modules)`, `kaic2:
core-parse-cache: miss (parsed N of M modules)`, or `kaic2:
core-parse-cache: off` (cache dir empty/unavailable). This line format
is a stable contract other tooling may grep for. `bin/kai` forwards
`--core-cache-stats` on every backend, including native and
native-modular, whose kaic2 stderr it otherwise captures to a file and
only surfaces on failure — the stats line is re-emitted to the user's
stderr on success too so it is never silently swallowed there.

## The five Makefiles — recursive delegation

```
Makefile            (root)   — façade: delegates to the stages via `$(MAKE) -C`
├── stage0/Makefile          — kaic0: minimal C compiler, zero deps
├── stage1/Makefile          — kaic1: intermediate compiler (kaikai-minimal)
├── stage2/Makefile          — kaic2: the real compiler (147 targets) + all its tests
└── demos/Makefile           — demo programs
```

**The root Makefile is a thin façade.** Almost every root target is `$(MAKE) -C <stage> <target>`. The real work for the compiler lives in `stage2/Makefile`. So:
- Invoke from the **root** for the common verbs (`make kaic2`, `make tier0`, `make tier1`, `make selfhost`, `make clean`). The root wires the bootstrap order and delegates.
- The **stage2** Makefile is where the compiler's own build + its ~147 test targets live (`test-tokens`, `test-ast`, `test-infer`, `test-dump-mono`, `test-run`, `test-perceus-*`, the parity ratchet, …). Reach into it directly only for a specific stage2 target: `make -C stage2 <target>`.

### The bootstrap chain (root targets)

```
kaic0:          $(MAKE) -C stage0 kaic0        # cc *.c → kaic0   (zero deps)
kaic1: kaic0    $(MAKE) -C stage1 kaic1        # kaic0 compiles stage1 → kaic1
kaic2: kaic1    $(MAKE) -C stage2 kaic2        # kaic1 compiles stage2 → kaic2
```

(`kaic2` depends on `kaic1` only while the boot is kaic1 — the default; see §KAIC_BOOT.)

Each stage's compiler builds the next. `make kaic2` triggers the whole chain if earlier stages are stale. After editing `stage2/compiler/*.kai`, `make kaic2` is the one command to rebuild. `make KAI_LLVM=1 kaic2` does the same with the in-process libLLVM backend linked (needed for native parity; on mac either put the keg on PATH first — `export PATH=/opt/homebrew/opt/llvm@18/bin:$PATH` — or pass `LLVM_CONFIG=$(brew --prefix llvm@18)/bin/llvm-config`; any LLVM major works, and `tools/gen-runtime-bc.sh` writes the runtime bitcode with the clang matching whichever one resolves). If `KAI_LLVM=1` is forced and llvm-config does not resolve, make stops immediately with an error naming the fix; the Homebrew lib dir needed by llvm-config's `-lzstd` is added to the link line automatically.

### `KAIC_BOOT` — the compiler that emits `stage2.c`

`stage2/build/stage2.c` is the whole compiler as one C file, emitted by a *boot* compiler (Go's `GOROOT_BOOTSTRAP`, Rust's stage0). `KAIC_BOOT` selects it; `tools/kaic-boot.sh` implements every mode but the default.

| `KAIC_BOOT` | Boot | An existing `stage2.c` is reused when |
|---|---|---|
| unset | `kaic1` (the chain above) | make's mtime rule says so, as always |
| `kaic1` | `kaic1` | its identity record matches exactly |
| `release` | the `kaic2` of the release named by `VERSION`, fetched once into `stage2/build/boot/` and verified against the release's published sha256 | its identity record matches exactly |
| `auto` | `stage2/kaic2` if this tree sealed it, else `release`, else `kaic1` | its inputs are unchanged (any boot) |
| `<path>` | that binary; kaic1- or kaic2-class by its `--version` | its identity record matches exactly |

**Two hops for a kaic2-class boot** (Rust's stage2; the seed rescue below is the same shape). The boot emits `stage2/build/stage2-a.c`, linked C-only into `stage2/build/kaic2-a`; `kaic2-a` emits `stage2/build/stage2.c`, the C `kaic2` is linked from. The delivered `kaic2` is therefore compiled by this tree's codegen, not the boot's: a codegen fix in the tree reaches the binary in the same build even when the release boot still carries the bug. The price is one more self-compile and one more `cc` of the whole compiler. A kaic1-class boot takes one hop and pays nothing extra — the default, and `scripts/build-release.sh`, stay on kaic1. `stage2-a.c` and `kaic2-a` are scratch: rebuilt on every emit, never recorded, never a boot.

**Identity, never mtime.** Every emit writes `stage2.c.id`: the boot (kind + sha256 of the binary; for `release` the version and tarball sha256), its class, the hop count, the content hash of every input the boot read, and the sha256 of the C itself. A kaic2-class boot also reads the stdlib core modules — their builtin-effect declarations land in the C — so its inputs include those files and the edition. A switched boot, an edited input, an edited `stage2.c`, or a kaic2-class C that did not take two hops regenerates; a tarball's archived mtimes cannot make an old C look fresh. Linking `kaic2` seals it (`build/kaic2.id` adds the binary's sha256): `auto` boots from the current `kaic2` only while the seal matches, so a binary copied from another checkout — which bakes that checkout's stdlib path — is never a boot. A tree built before records existed, or a CI artifact restored by exact cache key, has no record: the unset and `auto` modes then defer to mtime, an explicit boot regenerates.

Traps:

- **Two hops repair the boot's codegen only while `kaic2-a` still emits correct C.** The boot's codegen compiles `kaic2-a`; a boot bug that breaks the compiler's own C emission corrupts `stage2.c` itself, and no number of hops recovers from it — the way out is the kaic1 boot or a sound release. `make kaic-boot-verify` detects it: the release boot's `kaic2-a` and its `kaic2` must emit the same C for the compiler.
- **A kaic2-class boot reads this tree's stdlib, never the tarball's.** The C is compiled against this tree's `runtime.h`, which pairs with this stdlib; a release's own stdlib declares that release's builtin effects, and a changed effect-op signature makes its C a hard `cc` error against the current runtime.
- **The boot's codegen meets this tree's runtime.** A kaic2 boot emits the runtime calls its own codegen knows, and `stage2-a.c` is compiled against this tree's `runtime.h`. A runtime change that drops or re-signs a function an older codegen still emits breaks the release boot until the next release; an additive runtime keeps every recent release a valid boot.
- **Unavailable falls through, failing does not.** `auto` moves to the next boot only when one is absent (no sealed `kaic2`, no tarball for the platform, no network). A boot that fails to compile the source stops the build with its own error, and a checksum mismatch is fatal in every mode.
- **A non-kaic1 boot yields a different binary.** The kaic1 boot's `kaic2` is compiled by stage 1's codegen, a kaic2-class boot's by this tree's (through `kaic2-a`), so the binaries differ byte-for-byte. What must agree is the C each resulting `kaic2` emits — `make kaic-boot-verify`.

### `make kaic-boot-verify` — convergence gate

Builds one `kaic2` from the `kaic1` boot and one from the `release` boot (two hops) under `stage2/build/boot-verify/`, and requires byte-identical emitted C from both for the compiler itself and for a sample program — the `kaic2-fast-verify` contract across boots. It also requires the release boot's `kaic2-a` and its `kaic2` to emit the same C for the compiler: the fixed point showing the release's codegen did not miscompile `kaic2-a`. Each boot's `stage2.c` is reused on an exact identity match. Does not touch `stage2/kaic2`; fetches the release on first use.

## The package — why `main.kai` is a stub

The stage 2 compiler is a kaikai package rooted at `stage2/`: `main.kai` is the
entry point and the ~200 modules under `stage2/compiler/` are reached from it
through `import compiler.<mod>`.

- **Both kaic1 and kaic2 resolve imports.** kaic1 loads a module's dependencies
  before the module itself (post-order over the import graph), so compilation
  order is derived, never hand-maintained.
- Each module is lexed into a line range disjoint from every other. Lambda
  identity and the `__list_rest_<line>_<col>__` sentinels are keyed on
  (line, col); shared ranges make two lambdas at the same position in different
  files collide.
- `stage2/main.kai` is a 33-line entry stub, not the compiler. Compiling
  `main.kai` is compiling the whole package.

A NEW `stage2/compiler/*.kai` module needs only its own `import` lines and one
`import` of it from a module already in the graph. There is no ordered source
list to update. `make test-stage2-graph` (tier 0) asserts every module is
reachable from `main.kai`, that no `import` dangles, and that the graph stays
acyclic -- an unreachable module is silently absent from the compiler, not a
build error.

## Bootstrap seed — rescue from a bare `cc`

The seed is the C a released `kaic2` emits for its own source, frozen under a `bootstrap-seed-v<release>` tag. With `cc` alone it yields a C-backend `kaic2`: `cc` → seed `kaic2` → `kaic2-a` → the tree's `kaic2` → fixed point. Stage 0 and stage 1 are not on this path. A native-capable `kaic2` also needs libLLVM for the last hop (`KAI_LLVM=1`, see the bootstrap chain above).

- **Where it lives.** The tag points at a commit off `main` whose parent is the release that emitted it (`bootstrap-seed-v0.124.1` → `v0.124.1`). That commit adds `bootstrap/stage2.c` (the seed) and `bootstrap/runtime.h` (the parent's `stage2/runtime.h`, the only project header the seed includes). `main` never carries the seed; the tag's hash pins its content and its parent pins the source it reproduces. A clone fetches it with the other tags; a `--no-tags` clone needs `git fetch origin tag <seed>`.
- **Edition.** Emitted under the parent's `EDITION`, the edition `make selfhost` compiles the compiler under, so the seed is byte-identical to that commit's `stage2/build/kaic2b.c` and to what the published release's `kaic2` emits. A bare `kaic2` runs the oldest edition; always pass `--edition`.

Rescue, from the root of the checkout to build:

```sh
SEED=bootstrap-seed-v0.124.1 ROOT=$PWD ED=$(cat EDITION)
mkdir -p stage2/build/seed
git archive $SEED bootstrap | tar -x -C stage2/build/seed
cc -std=c99 -O2 stage2/build/seed/bootstrap/stage2.c -o stage2/build/seed/kaic2 -lm
(cd stage2 && KAIKAI_STDLIB_PATH=$ROOT/stdlib build/seed/kaic2 --edition $ED main.kai > build/stage2-a.c)
cc -std=c99 -O2 -I stage2/build/seed/bootstrap stage2/build/stage2-a.c -o stage2/build/kaic2-a -lm   # seed runtime; -I stage0 breaks it
make kaic2 KAI_LLVM=1 KAIC_BOOT=$ROOT/stage2/build/kaic2-a   # without libLLVM, drop KAI_LLVM=1: C backend only
(cd stage2 && ./kaic2 --edition $ED main.kai | cmp - build/stage2.c) && echo "fixed point"
./bin/kai build --backend=native examples/portfolio/portfolio.kai -o stage2/build/portfolio
stage2/build/portfolio | diff examples/portfolio/portfolio.out.expected - && echo "native OK"
```

- **The seed compiles against its own `runtime.h`.** `-I stage0` binds `#include "runtime.h"` to the stage 0 runtime and the seed does not compile.
- **Two hops.** `stage2-a.c` is the seed's codegen, so it compiles against the seed's runtime. The `make` hop has `kaic2-a` emit `stage2/build/stage2.c` with the tree's codegen and compiles it against the tree's `runtime.h`, so the final `kaic2` carries no seed codegen defect. That holds only while `kaic2-a`, compiled by the seed's codegen, still emits correct C: a defect that breaks C emission survives any number of hops, and the way out is kaic1 or a sound release.
- **`KAIC_BOOT` takes the path, not `auto`.** `auto` only boots from a `stage2/kaic2` this tree sealed, so it would skip `kaic2-a` and fall through to the release or kaic1.
- **The seed bakes no stdlib path.** `KAIKAI_STDLIB_PATH` points it at the tree's `stdlib/`; the `make` hop does the same for `kaic2-a` and the final `kaic2` bakes the path.
- **Native is checked twice.** The `KAI_LLVM=1` link refuses a `kaic2` that cannot emit a native object (see Traps); the portfolio build checks a real program against its golden.
- **The leap is guaranteed only for the seed's parent.** A later tree compiles only while its compiler sources and core stay inside the seed's language; past that, rescue each intervening release in turn, or refresh the seed.

Refreshing the seed after release `vX.Y.Z`:

```sh
git worktree add --detach ../seed vX.Y.Z && cd ../seed && make kaic2
mkdir bootstrap && cp stage2/runtime.h bootstrap/
(cd stage2 && ./kaic2 --edition $(cat ../EDITION) main.kai) > bootstrap/stage2.c
git add bootstrap && git commit -m "build(bootstrap): freeze the stage2.c rescue seed for vX.Y.Z"
git tag -a bootstrap-seed-vX.Y.Z -m "Bootstrap rescue seed emitted by vX.Y.Z"
```

Before `git push origin bootstrap-seed-vX.Y.Z`, run the rescue against the new tag in a clean clone of `vX.Y.Z` with libLLVM available: the fixed point and the native portfolio golden must both pass.

## `make kaic2-fast` — dev rebuild via modular self-compile

`make kaic2` always re-bootstraps: kaic1 reads the whole package and emits one ~212k-line C file, `cc -O2` compiles that giant TU. That is the **trust chain** (a fresh machine needs it), but as a dev rebuild it is all-or-nothing. `make kaic2-fast` is the rebuild path when a working `kaic2` already exists:

1. The existing `kaic2` compiles `stage2/main.kai` **directly** — no kaic1, no `cc -O2` of a whole-program TU — through `bin/kai`'s `KAI_MODULAR=1 --backend=c` path: ~86 per-module TUs compiled in parallel with the `.o` content-hash cache, so a one-module edit recompiles one TU.
2. The result lands in a **staging binary** (`stage2/build/kaic2-fast.bin`) and is sanity-gated (`--version` + a golden demo compiled with no flags, exercising the baked stdlib path) before being swapped into `stage2/kaic2`. A broken build never clobbers the working compiler.

Selection is **explicit, never automatic**: `make kaic2` stays pure bootstrap, `make kaic2-fast` is additive and opt-in. Auto-preferring the fast path was rejected — a stale `kaic2` silently building a wrong `kaic2` is the failure mode to avoid; typing `-fast` is the acknowledgment that you trust the binary currently in place.

Caveats:

- **C backend only.** The fast-built `kaic2` has no native (libLLVM) backend even if the previous one did. For a native-capable compiler run `make KAI_LLVM=1 kaic2`.
- **The first `kaic2` still comes from the bootstrap**, as does anything CI or the selfhost oracle trusts.

### `make kaic2-fast-verify` — equivalence gate

Checks that the fast path builds the *same compiler* as the one in place: the fast-built staging binary and `./stage2/kaic2` must emit **byte-identical C** for the compiler itself (`main.kai`, single-TU) and for a sample program. Run it from a fixed point — `kaic2` built from the current source (right after `make kaic2` it proves bootstrap ≡ fast). It does not touch `kaic2`. Byte-identity of the *binaries* is not expected (multi-`.o` parallel link vs one `-O2` TU); identical emitted C is the functional-parity gate, consistent with how `selfhost` pins determinism.

## Verification targets (root)

- `make tier0` — fast pre-commit sanity (`selfhost` + `demos-no-regression` + arena + heap-limit). Run before committing compiler changes.
- `make tier1` — full suite, the CI merge gate (sharded as `tier1-shard-1/2/3` in CI; `make test` + demos + fmt + negatives + stdlib-modules + audits + …).
- `make selfhost` — the byte-identity fixed point: `kaic2` compiles its own source to `kaic2b.c`, that compiles to `kaic2b`, which recompiles the source to `kaic2c.c`; asserts `kaic2b.c == kaic2c.c`. The definitive "did I break the compiler" check.
- **Trust CI for the full battery.** Locally run the minimum gate (`make selfhost` + the smoke of your change); leave `tier1`/`tier1-native` to CI.

## Backend parity — one fixture

To diff a fixture's output between backends, use the harness, do not hand-roll native-vs-C:

```
TARGET_BACKEND=native ORACLE_BACKEND=c BACKEND_PARITY_JOBS=1 \
  BACKEND_PARITY_DIRS="examples/perceus" tools/test-backend-parity.sh
```

`BACKEND_PARITY_JOBS=1` = serial (the parallel ratchet is false-green; serial is authoritative). `BACKEND_PARITY_DIRS` scopes the corpus. The ratchet gates against `tools/native-parity-baseline.txt` (must stay empty = full parity).

## Traps (verified, recurring)

- **`stage2/main.kai` is a stub**, not the compiler. The compiler is the ~200 modules it reaches through `import compiler.driver`.
- **`kai fmt` is in-place destructive** — never run it on compiler/stdlib sources; redirect output to `/tmp`.
- **`runtime.h` has TWO copies** (`stage0/runtime.h` + `stage2/runtime.h`). A runtime prim/handler added to one must be added to BOTH; they change together.
- **Header prerequisites are hand-declared.** Stage 1 and stage 2 compile a *generated* `.c` whose `#include "runtime.h"` make cannot see, so each rule names the header it actually binds under its own `-I` order (`stage2` → `stage2/runtime.h`, `stage1` → `stage0/runtime.h`). Adding a rule that compiles either translation unit means adding that prerequisite too, and only that one — naming a header the TU never reaches turns unrelated edits into a ~70 s `-O2` rebuild. `make test-header-deps` (tier 0) asserts both directions.
- **The native paths compile a COPY of the entry file**, not the file the user named — the whole-program path under the run's `$tmp`, the native-modular path under the content-addressed cache dir — because `kaic2` derives the object path and the cache keys from the path it is handed. Anything that renders that path back to the user (diagnostics, DWARF) must map it to the real source: `kai` corrects the captured stderr (`cli_plan.rewrite_paths`), and `KAI_DEBUG_SRC` does the same for the DIFile. The C path never copies. Gate: `make test-native-diag-path` (tier 1, and the tier1-native shard 2).
- **A `KAI_LLVM=1` link does not prove native works.** A `kaic2` compiled by `kaic2` can link libLLVM and still crash emitting native, and neither `selfhost` nor a C-backend run notices. The link recipe runs `tools/native-probe.sh` (a one-line `--emit=native`) on the fresh binary and deletes it when the probe fails; `KAI_NATIVE_PROBE=0` skips the probe to keep a broken binary for diagnosis. Gate: `make test-native-probe` (tier 0).
- **mtime trap**: make decides rebuilds by timestamps, which don't survive a checkout or artifact download. After such, the binary chain may look stale; `make` rebuilds what it thinks is needed.
- **Doc-only changes** (diff confined to `docs/`, root `*.md`, `LICENSE`) skip every tier locally and in CI (`paths-ignore`). Code paths always trigger tiers.
