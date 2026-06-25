# Build system — Makefile structure & how to invoke the compiler

Practical map of the build. Read this before running the compiler or touching a Makefile — it removes the two recurring time-sinks: not knowing how to invoke the compiler, and getting lost in the recursive Makefiles.

## TL;DR — the commands you actually need

| You want to… | Command (from repo root) |
|---|---|
| Run / build a `.kai` program | `./bin/kai run <file.kai>` · `./bin/kai build <file.kai> -o <out>` |
| Rebuild the compiler after editing `stage2/compiler/*.kai` | `make kaic2` (C) · `make KAI_LLVM=1 kaic2` (native) |
| Full bootstrap from scratch | `make all` (→ `kaic0` → `kaic1` → `kaic2` → `bin/kai`) |
| Verify a change | `make tier0` (fast) · `make tier1` (full, CI gate) |
| One backend-parity fixture | `tools/test-backend-parity.sh` (env-driven; see §parity) |

**Do NOT** call `kaic2` raw, pass `--path ../stdlib` by hand, or reconstruct a `cc … -I ../stage0` line from Makefile recipes. `bin/kai` does all of that. **Do NOT** compile `stage2/main.kai` as "the compiler" — it is a 33-line stub (see §bundle).

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

A C-only `kaic2` prints `note: native backend unavailable … using the C backend` and falls back — harmless. Subcommands: `build`, `run`, `test`, `bench`, `check`, plus `--holes-json` / `--diags-json` / `--effects-json` for structured output.

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

Each stage's compiler builds the next. `make kaic2` triggers the whole chain if earlier stages are stale. After editing `stage2/compiler/*.kai`, `make kaic2` is the one command to rebuild — it reassembles the bundle and compiles. `make KAI_LLVM=1 kaic2` does the same with the in-process libLLVM backend linked (needed for native parity; on mac first `export PATH=/opt/homebrew/opt/llvm@18/bin:$PATH` and `export LIBRARY_PATH=/opt/homebrew/opt/zstd/lib:$LIBRARY_PATH`).

## The bundle — why `main.kai` is a stub

stage1/kaic1 is kaikai-minimal: it does **not** resolve `import`. So the stage2 compiler, which is split across ~55 modules under `stage2/compiler/`, is fed to kaic1 as a single **concatenated bundle**, not via imports.

- `BUNDLE_SRCS` in `stage2/Makefile` is the explicit, **ordered** list of those ~55 files (`util.kai`, `chars.kai`, `lex.kai`, … `emit_c.kai`, `driver.kai`, `main.kai`). Order matters (helpers before users).
- The `kaic2` recipe concatenates them into one bundle and compiles that. **You never concat `BUNDLE_SRCS` by hand — the Makefile does it.**
- `stage2/main.kai` is a 33-line entry stub at the END of the bundle, not the compiler. Compiling `main.kai` alone does nothing useful.

When you add a NEW `stage2/compiler/*.kai` module, it must be added to `BUNDLE_SRCS` in dependency order, or the bundle won't see it.

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

- **`stage2/main.kai` is a stub**, not the compiler. The compiler is the `BUNDLE_SRCS` bundle.
- **`kai fmt` is in-place destructive** — never run it on compiler/stdlib sources; redirect output to `/tmp`.
- **`runtime.h` has TWO copies** (`stage0/runtime.h` + `stage2/runtime.h`). A runtime prim/handler added to one must be added to BOTH; they change together.
- **mtime trap**: make decides rebuilds by timestamps, which don't survive a checkout or artifact download. After such, the binary chain may look stale; `make` rebuilds what it thinks is needed.
- **Doc-only changes** (diff confined to `docs/`, root `*.md`, `LICENSE`) skip every tier locally and in CI (`paths-ignore`). Code paths always trigger tiers.
