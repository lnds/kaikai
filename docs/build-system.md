# Build system — Makefile structure & how to invoke the compiler

Practical map of the build. Read this before running the compiler or touching a Makefile — it removes the two recurring time-sinks: not knowing how to invoke the compiler, and getting lost in the recursive Makefiles.

## TL;DR — the commands you actually need

| You want to… | Command (from repo root) |
|---|---|
| Run / build a `.kai` program | `./bin/kai run <file.kai>` · `./bin/kai build <file.kai> -o <out>` |
| Rebuild the compiler after editing `stage2/compiler/*.kai` | `make kaic2-fast` (dev, needs an existing kaic2) · `make kaic2` (bootstrap, C) · `make KAI_LLVM=1 kaic2` (native) |
| Full bootstrap from scratch | `make all` (→ `kaic0` → `kaic1` → `kaic2` → `bin/kai`) |
| Verify a change | `make tier0` (fast) · `make tier1` (full, CI gate) |
| One backend-parity fixture | `tools/test-backend-parity.sh` (env-driven; see §parity) |

**Do NOT** call `kaic2` raw, pass `--path ../stdlib` by hand, or reconstruct a `cc … -I ../stage0` line from Makefile recipes. `bin/kai` does all of that. **Do NOT** compile `stage2/main.kai` as "the compiler" — it is a 33-line stub (see §package).

## `bin/kai` — the entry point

`bin/kai` is a checked-in POSIX shell wrapper. It is the ONLY thing you should use to run/build kaikai code. It resolves, automatically:
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

Each stage's compiler builds the next. `make kaic2` triggers the whole chain if earlier stages are stale. After editing `stage2/compiler/*.kai`, `make kaic2` is the one command to rebuild. `make KAI_LLVM=1 kaic2` does the same with the in-process libLLVM backend linked (needed for native parity; on mac either put the keg on PATH first — `export PATH=/opt/homebrew/opt/llvm@18/bin:$PATH` — or pass `LLVM_CONFIG=$(brew --prefix llvm@18)/bin/llvm-config`; any LLVM major works, and `tools/gen-runtime-bc.sh` writes the runtime bitcode with the clang matching whichever one resolves). If `KAI_LLVM=1` is forced and llvm-config does not resolve, make stops immediately with an error naming the fix; the Homebrew lib dir needed by llvm-config's `-lzstd` is added to the link line automatically.

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
- **The native paths compile a COPY of the entry file**, not the file the user named — the whole-program path under the run's `$tmp`, the native-modular path under the content-addressed cache dir — because `kaic2` derives the object path and the cache keys from the path it is handed. Anything that renders that path back to the user (diagnostics, DWARF) must map it to the real source: `bin/kai` corrects the captured stderr with `diag_rewrite_entry_path`, and `KAI_DEBUG_SRC` does the same for the DIFile. The C path never copies. Gates: `make test-diag-path-rewrite` (tier 0), `make test-native-diag-path` (tier 1).
- **mtime trap**: make decides rebuilds by timestamps, which don't survive a checkout or artifact download. After such, the binary chain may look stale; `make` rebuilds what it thinks is needed.
- **Doc-only changes** (diff confined to `docs/`, root `*.md`, `LICENSE`) skip every tier locally and in CI (`paths-ignore`). Code paths always trigger tiers.
