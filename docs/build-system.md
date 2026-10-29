# Build system — Makefile structure & how to invoke the compiler

Practical map of the build. Read this before running the compiler or touching a Makefile — it removes the two recurring time-sinks: not knowing how to invoke the compiler, and getting lost in the recursive Makefiles.

## TL;DR — the commands you actually need

| You want to… | Command (from repo root) |
|---|---|
| Run / build a `.kai` program | `./bin/kai run <file.kai>` · `./bin/kai build <file.kai> -o <out>` |
| Rebuild the compiler after editing `stage2/compiler/*.kai` | `make kaic2-fast` (dev, needs an existing kaic2) · `make kaic2` (boots from the previous release, C) · `make KAI_LLVM=1 kaic2` (native) · `make kaic2 KAIC_BOOT=seed` (offline, `cc` only; see §KAIC_BOOT) |
| Full bootstrap from scratch | `make all KAIC_BOOT=seed` (`cc` → seed → `kaic2` → `bin/kai`; also builds the frozen `kaic0` → `kaic1`) |
| A compiler in a fresh worktree, in seconds | `tools/prebuilt-kaic2.sh seed` (see §Prebuilt kaic2) |
| Verify a change | the gates of §Before the first push · `make tier0` (fast) · `make tier1` (full, CI gate) |
| One backend-parity fixture | `tools/test-backend-parity.sh` (env-driven; see §parity) |
| Diff the backends over generated programs | `tools/progen-diff.sh <first-seed> <count>` (see `docs/testing-tiers.md` §Generated-program differential test) |

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

`./bin/kai typecheck <file.kai>` is the fast edit-loop answer to "does this compile?": it runs the full front-end (resolve + infer + protocol/kind/effect checks) and monomorphisation, then stops — no codegen, no `cc`, no link, so it behaves identically on a C-only and a native `kaic2`. Diagnostics and exit code are identical to a build's, a bound violated at a concrete instantiation included (gate: `make test-check-parity`); errors that only surface in a backend subset gap are out of its scope by design, and the other report flags stop before monomorphisation. The JSON report flags ride it; `kai typecheck f.kai --diags-json` reports the same run as JSON, gated against the negative corpus by `make test-check-parity` (`tools/diags-json-parity.py`).

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
stage2 kaic2`, which boots it as §KAIC_BOOT says; an existing `kaic2` is
used as is. Gate: `make test-kai-cli`.

### The shared core cache

Every `bin/kai` build passes `--core-cache-dir` + `--toolchain-id` to
kaic2, enabling two persistent, content-addressed cache layers for the
auto-loaded stdlib core under `~/Library/Caches/kai/core/<toolchain-id>/`
(Linux: `~/.cache/kai/core/…`; `XDG_CACHE_HOME` honoured):

- **post-parse blobs** (KAB2, one per core module) — a warm build skips
  the core's lex+parse entirely;
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

### Cache lifetime — recency, trim, `kai clean`

The shared caches under `~/Library/Caches/kai` (`$XDG_CACHE_HOME/kai`,
else `~/.cache/kai`) — `cmodular`, `lto`, `nmodular`, `nativedep`, `core`, `pkg`
— are content-addressed and keyed by compiler identity, so every new
`kaic2` mints new keys. They are bounded by recency, as in Go's build
cache:

- **A hit refreshes its entry's mtime** (a `utimensat`, skipped when the
  mtime is under an hour old): a c-modular or nativedep object on a hit,
  the objects a native-modular link uses, the `core/<toolchain id>`
  directory on every compile verb, and the checkout of each fetched
  dependency a build resolves.
- **The trim runs at most once per 24 h.** The first compile-verb `kai`
  after the stamp `<cache root>/trim-stamp` ages out forks a detached,
  low-priority child once its own work is done; the child removes
  entries unused for 5 days. The unit is a file for `cmodular`,
  `lto`, `nmodular` and `nativedep`, a toolchain directory for `core`, a
  checkout for `pkg`. Entries are published by write-to-temp + rename
  and a fresh temp file is never old enough to go; a path another
  process removed first is skipped.
- `KAI_CACHE_TRIM=0` disables the trim; `KAI_CACHE_TRIM_DAYS=<n>` changes
  the age (default 5).
- `kai clean` deletes the cwd package's `.kai-cache`; `kai clean --cache`
  deletes the shared caches. Both print what they freed.

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
kaic2:          $(MAKE) -C stage2 kaic2        # the boot compiles stage2 → kaic2 (§KAIC_BOOT)
```

Stage 0 and stage 1 are frozen, not retired: they build from `cc`, tier 1 tests them, and the release self-hosts stage 1, but they do not compile stage 2 — kaikai-minimal cannot keep up with the language stage 2 is written in. `kaic2` depends on `kaic1` only under `KAIC_BOOT=kaic1`, which holds only for trees stage 1 can still parse. After editing `stage2/compiler/*.kai`, `make kaic2` is the one command to rebuild. `make KAI_LLVM=1 kaic2` does the same with the in-process libLLVM backend linked (needed for native parity; on mac either put the keg on PATH first — `export PATH=/opt/homebrew/opt/llvm@22/bin:$PATH` — or pass `LLVM_CONFIG=$(brew --prefix llvm@22)/bin/llvm-config`; any LLVM major works, and `tools/gen-runtime-bc.sh` writes the runtime bitcode with the clang matching whichever one resolves). If `KAI_LLVM=1` is forced and llvm-config does not resolve, make stops immediately with an error naming the fix; the Homebrew lib dir needed by llvm-config's `-lzstd` is added to the link line automatically.

### `KAIC_BOOT` — the compiler that emits `stage2.c`

`stage2/build/stage2.c` is the whole compiler as C — one translation unit, or one per module under `KAIC_BOOT_MODULAR=1` (below) — emitted by a *boot* compiler (Go's `GOROOT_BOOTSTRAP`, Rust's stage0). `KAIC_BOOT` selects it and `tools/kaic-boot.sh` implements it; every mode goes through the identity record below.

| `KAIC_BOOT` | Boot | An existing `stage2.c` is reused when |
|---|---|---|
| `release` (or unset) | the `kaic2` of the newest published release up to `VERSION` (`VERSION`'s own once published), fetched once into `stage2/build/boot/` and verified against the release's published sha256 | its inputs match and its record names a release up to `VERSION` |
| `seed` | the newest `bootstrap-seed-v*` tag up to `VERSION` (§seed), linked by `cc` against its own `runtime.h` into `stage2/build/boot/<tag>/kaic2`, cached per tag | its inputs match and its record names a seed up to `VERSION` |
| `kaic1` | `kaic1` (the frozen chain) | its identity record matches exactly |
| `auto` | `stage2/kaic2` if this tree sealed it, else `release`, else `seed`, else `kaic1` | its inputs are unchanged (any boot) |
| `<path>` | that binary; kaic1- or kaic2-class by its `--version` | its identity record matches exactly |

The default needs the network once per release (then the cached tarball) and a platform the release ships a tarball for; `seed` needs only `cc` and the seed tags. Both are the same compiler: the seed of release N is the C release N's `kaic2` emits for itself.

**Two hops for a kaic2-class boot** (Rust's stage2). The boot emits `stage2/build/stage2-a.c`, linked C-only into `stage2/build/kaic2-a`; `kaic2-a` emits `stage2/build/stage2.c`, the C `kaic2` is linked from. The delivered `kaic2` is therefore compiled by this tree's codegen, not the boot's: a codegen fix in the tree reaches the binary in the same build even when the boot still carries the bug. The price is one more self-compile and one more `cc` of the whole compiler. A kaic1-class boot takes one hop. `stage2-a.c` and `kaic2-a` are scratch: rebuilt on every emit, never recorded, never a boot.

**`KAIC_BOOT_MODULAR=1` — one translation unit per module.** By default each hop's C is a single ~200k-line translation unit, which one `cc -O2` compiles on one core. With the knob set, a kaic2-class boot emits both hops through `--emit=c-modular`: `stage2-a.c` and `stage2.c` are then the marker-delimited stream of every module's `.c` and header slices, still one file each, still covered by the identity record. `tools/kaic-link.sh` links either form and reads which one it has from the file's first line, never from the environment: a stream is split under `stage2/build/tus/<binary>/`, its translation units compiled in parallel (`KAIC_LINK_JOBS`, default every core) and linked with the runtime owner (`runtime_owner_c.c`). The knob shapes a new emit only — a fresh `stage2.c` is reused in whichever form it has, so a job that consumes a CI artifact need not set it. It is opt-in: the default, the seed's own C, a kaic1-class boot, the release build and `kaic-boot-verify` stay single-unit, and the rescue path never depends on a modular emit.

**Objects are cached by what `cc` reads.** Each object of a stream is stored in `stage2/build/obj-cache/<binary>/` (`KAIC_OBJ_CACHE` moves it) under a key folding the compiler's identity, every flag, and the content of the translation unit and of every file it includes, as `cc -M` lists them for those flags — `runtime.h`, the unit's header slices, the system and LLVM headers. The key is recomputed on every link from what is on disk, so an object is linked only when `cc` would read exactly what it read to produce it; an edited module recompiles its own unit and those whose slices changed, a `runtime.h` edit recompiles everything. A unit whose key cannot be computed is compiled and not cached; objects the last link did not use are pruned. Gate: `make test-kaic-boot` (`tools/test-kaic-link.sh`).

**A release boot is a release that exists.** `VERSION` names the release in progress between a version bump and its publication, so an unpublished `VERSION` resolves to the newest published release (read from its `latest.json` manifest) when that precedes `VERSION`, and to nothing otherwise. Freshness never reaches the network, so it accepts any recorded release up to `VERSION`; `tools/kaic-boot.sh release-id` prints the resolved release (name and tarball sha256) without downloading it.

**Identity, never mtime.** Every emit writes `stage2.c.id`: the boot (kind + sha256 of the binary; for `release` the version and tarball sha256), its class, the hop count, the content hash of every input the boot read, and the sha256 of the C itself. A kaic2-class boot also reads the stdlib core modules — their builtin-effect declarations land in the C — so its inputs include those files and the edition. A switched boot, an edited input, an edited `stage2.c`, or a kaic2-class C that did not take two hops regenerates; a tarball's archived mtimes cannot make an old C look fresh. Linking `kaic2` seals it (`build/kaic2.id` adds the binary's sha256): `auto` boots from the current `kaic2` only while the seal matches, so a binary copied from another checkout — which bakes that checkout's stdlib path — is never a boot. A tree built before records existed, or a CI artifact restored by exact cache key, has no record: `auto` then defers to mtime, every other mode regenerates.

Traps:

- **Two hops repair the boot's codegen only while `kaic2-a` still emits correct C.** The boot's codegen compiles `kaic2-a`; a boot bug that breaks the compiler's own C emission corrupts `stage2.c` itself, and no number of hops recovers from it — the way out is an earlier sound release or seed. `make kaic-boot-verify` detects it: each boot's `kaic2-a` and its `kaic2` must emit the same C for the compiler.
- **A kaic2-class boot reads this tree's stdlib, never the tarball's.** The C is compiled against this tree's `runtime.h`, which pairs with this stdlib; a release's own stdlib declares that release's builtin effects, and a changed effect-op signature makes its C a hard `cc` error against the current runtime.
- **The boot's codegen meets this tree's runtime.** A kaic2 boot emits the runtime calls its own codegen knows, and `stage2-a.c` is compiled against this tree's `runtime.h` — for the seed too: only the seed's own C is linked against the seed's `runtime.h`. A runtime change that drops or re-signs a function an older codegen still emits breaks the release and seed boots until the next release; an additive runtime keeps every recent release a valid boot. CI boots every PR from the release, so it gates the seed's codegen against this runtime as well.
- **A builtin core calls is declared in core.** The boot compiles this tree's stdlib core, and a `builtin_table` row (`stage2/compiler/builtins.kai`) exists only in the compiler that carries it, so core may call a row only once a published release (and its seed) knows it. A core module can instead declare the builtin itself, which any boot that understands the form compiles (outside the core modules and `seq`, the same declaration is a compile error):

  ```kaikai-neg
  extern "kai"("kai_core_name") pub fn name[T](xs: [T]) : T / Mutable
  ```

  The ABI `"kai"` passes boxed values: every argument enters owned and the result leaves owned, as a row's do, and any kaikai type is allowed. The signature is the builtin's scheme and its row is taken as written — no `Ffi`. The symbol is always explicit. A call binds `<symbol>` on the C backend and `kaix_core_<name>` on the native one, exactly as a row's call does. A `kai_core_` entry emits no fn: the builtin as a value rides the runtime's `_kai_core_<name>_thunk` (C) / `kaix_<name>_thunk` (native), as a row does; another entry (`seq`'s `kai_seq_*`) emits a forwarding fn and its thunk. Only the core modules and `stdlib/seq.kai` may declare one — `seq` because its `Seq` is a pure type whose cell the runtime owns; decided by the file's resolved path among those module paths (`kai_builtin_module_paths` in `driver_core_load.kai`, read by the check and by `--kai-builtins` alike), never by its module name — and a declaration shadows a row of the same name, so a row migrates to core one builtin at a time. Adding one is the declaration plus `kai_core_<name>` and its `_kai_core_<name>_thunk` in `stage2/runtime.h`, and `kaix_core_<name>` and `kaix_<name>_thunk` in `stage0/runtime_llvm.c`. `make test-kai-builtins` (tier0) diffs every declaration against `tools/kai-builtins.txt` and compiles each entry, with `-Werror`, into a function pointer of the declared arity against both runtimes; `make test-builtin-table` keeps checking the rows. `core/string.kai` declares `string_find` this way, and `string.index_of` calls it; `seq.kai` declares `defer` and `force`, bound to `kai_seq_defer` / `kai_seq_force`. A builtin a pass singles out by its row (`CoreBuiltin`, the Array/Vec forms, the `Mutable`-masking writers) stays a row: a declaration is a different identity, so the pass would stop seeing it.
- **Unavailable falls through, failing does not.** `auto` moves to the next boot only when one is absent (no sealed `kaic2`, no tarball for the platform, no network). A boot that fails to compile the source stops the build with its own error, and a checksum mismatch is fatal in every mode.
- **A compiler linked per module shares its runtime state through the owner.** `runtime.h` state that one module writes and another reads — the RC pools, the tag tables, the native emit pool and LLVM context — must be `extern` under `KAI_SEPARATE_COMPILATION` and defined once by the `KAI_RUNTIME_OWNER` unit. A file-scope `static` there links green and gives every unit its own copy: the native emit pool queued an object in one unit, the exit drained another unit's empty queue, and the compiler exited 0 without writing it. `tools/native-probe.sh` runs the pool with a core object for that reason.
- **Different boots can yield different binaries.** `kaic2-a` is compiled by the boot's codegen, so its behaviour — not its source — may differ between boots. What must agree is the C each resulting `kaic2` emits — `make kaic-boot-verify`.

### `make kaic-boot-verify` — convergence gate

Builds one `kaic2` from the `release` boot and one from the `seed` boot (two hops each) under `stage2/build/boot-verify/`, and requires byte-identical emitted C from both for the compiler itself and for a sample program — the `kaic2-fast-verify` contract across boots. It also requires each boot's `kaic2-a` and its `kaic2` to emit the same C for the compiler: the fixed point showing the boot's codegen did not miscompile `kaic2-a`. Each boot's `stage2.c` is reused on an exact identity match. Does not touch `stage2/kaic2`; fetches the release on first use and needs the seed tags.

It runs in two halves: `make kaic-boot-verify-one BOOT=release|seed` builds one boot's `kaic2` and checks its fixed point, independently of the other boot; `make kaic-boot-verify-cross` compares the two halves' `self.c` and `sample.c`. `make kaic-boot-verify` runs both halves in series, then the cross check.

### Which boot CI uses

Every CI workflow that builds or consumes a `kaic2` sets `KAIC_BOOT=release` workflow-wide. The `build` jobs of `tier1.yml` and `tier1-native.yml` also set `KAIC_BOOT_MODULAR=1` and carry `stage2/build/obj-cache` across runs (restored by prefix, which the per-object keys make sound; saved from `main`), so a PR compiles only the objects its change reaches. The consumers need the mode too: the artifact ships `stage2.c.id`, and a job whose mode disagrees with it rebuilds `stage2.c`. The build jobs still build `kaic1`, because tier1 tests stage 1 itself and the artifact carries it; the bootstrap caches fold the resolved release (`release-id`), `EDITION` and `tools/kaic-boot.sh` into their key.

The `cc`-only route runs in three places. The daily `bootstrap-boot` jobs run one `kaic-boot-verify-one` half each, in parallel on clean runners with their own `cc` (the seed job fetches the seed tags), and `bootstrap-from-scratch` runs `kaic-boot-verify-cross` on their outputs. The release build (`scripts/build-release.sh`) sets `KAIC_BOOT=seed`, so every shipped `kaic2` is booted by C that `cc` alone compiles, and it emits the next seed (§seed). It also builds `kaic0` → `kaic1` and self-hosts stage 1, so the frozen chain stays verified. `linux-aarch64.yml` boots from the seed on an aarch64 Linux runner with the system GCC (path-gated on pull requests, nightly otherwise), then self-hosts and runs the C backend over the effect, segment, RC and actor fixtures at one thread and at four.

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

## Bootstrap seed — the `cc` anchor

The seed is the C a released `kaic2` emits for its own source, frozen under a `bootstrap-seed-v<release>` tag (OCaml's `boot/ocamlc`). With `cc` alone it yields a C-backend `kaic2`: `cc` → seed `kaic2` → `kaic2-a` → the tree's `kaic2` → fixed point. Stage 0 and stage 1 are not on this path. Every release boots from it, so the chain of trust stays anchored in `cc` while stage 1 stays frozen. A native-capable `kaic2` also needs libLLVM for the last hop (`KAI_LLVM=1`, see the bootstrap chain above).

- **Where it lives.** The tag points at a commit off `main` whose parent is the release that emitted it (`bootstrap-seed-v0.125.0` → `v0.125.0`). That commit adds `bootstrap/stage2.c` (the seed) and `bootstrap/runtime.h` (the parent's `stage2/runtime.h`, the only project header the seed includes). `main` never carries the seed; the tag's hash pins its content and its parent pins the source it reproduces. Both `lnds/kaikai` and the public mirror `kaikailang-org/kaikai` carry the tags; on the mirror the parent is the mirror's snapshot of the release, and `bootstrap/` is byte-identical. `KAIC_BOOT=seed` fetches them from `origin` (`KAIC_BOOT_SEED_REMOTE` names another remote, `KAIC_BOOT_SEED_FETCH=0` skips the fetch) and warns when the fetch fails; to fetch them by hand,  `git fetch https://github.com/kaikailang-org/kaikai.git 'refs/tags/bootstrap-seed-v*:refs/tags/bootstrap-seed-v*'`.
- **Edition.** Emitted under the parent's `EDITION`, the edition `make selfhost` compiles the compiler under, so the seed is byte-identical to that commit's `stage2/build/kaic2b.c` and to what the published release's `kaic2` emits. A bare `kaic2` runs the oldest edition; always pass `--edition`.

Rescue, from the root of a checkout with the seed tags:

```sh
make kaic2 KAIC_BOOT=seed KAI_LLVM=1   # without libLLVM, drop KAI_LLVM=1: C backend only
(cd stage2 && ./kaic2 --edition $(cat ../EDITION) main.kai | cmp - build/stage2.c) && echo "fixed point"
./bin/kai build --backend=native examples/portfolio/portfolio.kai -o stage2/build/portfolio
stage2/build/portfolio | diff examples/portfolio/portfolio.out.expected - && echo "native OK"
```

- **The seed links against its own `runtime.h`.** `cc` compiles `bootstrap/stage2.c` with only the seed's directory on the include path; the tree's or stage 0's `runtime.h` need not match the seed's codegen.
- **Two hops.** `stage2-a.c` is the seed's codegen applied to this tree, compiled against this tree's `runtime.h` exactly as a release boot's is (see Traps above). The `make` hop has `kaic2-a` emit `stage2/build/stage2.c` with the tree's codegen, so the final `kaic2` carries no seed codegen defect — while `kaic2-a` still emits correct C.
- **The seed bakes no stdlib path.** `tools/kaic-boot.sh` points it at the tree's `stdlib/` through `KAIKAI_STDLIB_PATH`, and does the same for `kaic2-a`; the final `kaic2` bakes the path.
- **Native is checked twice.** The `KAI_LLVM=1` link refuses a `kaic2` that cannot emit a native object (see Traps); the portfolio build checks a real program against its golden.
- **The leap is guaranteed for the trees CI gates.** The newest seed is the newest release's compiler, and CI boots every PR from that release, so every tree on `main` compiles from it. An older seed reaches a later tree only while that tree's compiler sources and core stay inside the seed's language; past that, rescue each intervening release in turn.

Publishing. `scripts/build-release.sh` emits `dist/bootstrap-seed-v<VERSION>/bootstrap/` from the release's own selfhost (`stage2/build/kaic2b.c`, `stage2/runtime.h` and `stage2/runtime/`), links it against its own `runtime.h` and requires it to reproduce itself before the release proceeds. The release workflow's `seed` job requires the seeds of every platform to be byte-identical — a difference means stage 2 emits platform-dependent C and fails the job, publishing nothing — then tags the commit and pushes the tag, with the seed's sha256 in the tag message. The push uses the job's `GITHUB_TOKEN`. GitHub checks a new tag's tree against the default branch, so once a workflow changes on `main` after the release, `GITHUB_TOKEN` (which can never hold the workflows permission) is refused the seed tag; that refusal, and only that one, retries with the `SEED_PUSH_TOKEN` secret, a fine-grained token on `lnds/kaikai` with Contents and Workflows write. The retry logs the token's shape and what `GET /user` and `GET /repos/lnds/kaikai` answer with it, never the token, so a rejected secret says why. `publish-source` then copies that seed onto the mirror's tag of the release under the same tag name. An existing seed tag is never moved, on either repository: a rerun with the same seed is a no-op, one with a different seed fails. Dispatching the workflow with `mirror_seed_only` copies an already published seed tag to the mirror and runs nothing else; dispatching it with `seed_run_id` publishes the seed and the mirror from that earlier run's seed artifacts, without building or touching the release.

## Vendored static libLLVM — prebuilt, per platform

The shipped `kaic2` links libLLVM statically, from archives under `stage0/third_party/llvm/build` (gitignored). `mk/llvm.mk` owns how they are built: `make llvm-build` downloads LLVM's source and compiles the narrow X86+AArch64+WebAssembly `MinSizeRel` set, ~25 min cold. A release does not pay that: `make llvm-prebuilt` (what `scripts/build-release.sh` calls) downloads the same archives as a release asset, verifies the sha256 published next to it and unpacks it; `make llvm-build` is then a no-op.

The asset is `libllvm-static-<LLVM version>-<hash>-<os>-<arch>.tar.xz` under the prerelease `llvm-static-<LLVM version>` on `lnds/kaikai`. The hash covers `mk/llvm.mk` and `scripts/llvm-prebuilt.sh`, so a changed flag, target list or packed layout names a new asset and never reuses an old one. It carries only what the `KAI_LLVM=1` link reads: `llvm-config`, the `.a` archives, the `llvm-c` and `llvm` headers with the generated `llvm/Config`. WebAssembly is built so the native backend can emit `wasm32`; `stage2/Makefile` links its codegen and asm-parser components and defines `KAI_LLVM_WEBASSEMBLY` only where `llvm-config --targets-built` lists it, so a libLLVM without the target (an older asset, a distro build) still links and the runtime reports the target as unavailable. The `llvm` headers ship in the asset for the small C++ shim the C API cannot replace; `build-release.sh` refuses a vendored libLLVM that lacks the target. Where no asset matches — a fork that edited the flags, a new LLVM version, an unsupported host — `make llvm-prebuilt` falls back to the source build; `KAI_LLVM_PREBUILT=0` forces it, and a checksum mismatch stops instead of falling back.

`.github/workflows/llvm-prebuilt.yml` publishes the asset for each release platform when either hashed file changes, on the pull request as well as on `main`, so the asset exists before the change merges. An asset already published is not rebuilt. Its `verify` job then runs `scripts/build-release.sh` from the published asset on a clean runner with no `cmake`.

## `make kaic2-fast` — dev rebuild via modular self-compile

`make kaic2` always re-bootstraps: the boot reads the whole package and emits one ~212k-line C file, `cc -O2` compiles that giant TU (or, under `KAIC_BOOT_MODULAR=1`, one TU per module in parallel — twice, once per hop). That is the **trust chain** (a fresh machine needs it), but as a dev rebuild it is all-or-nothing. `make kaic2-fast` is the rebuild path when a working `kaic2` already exists:

1. The existing `kaic2` compiles `stage2/main.kai` **directly** — no boot, no `cc -O2` of a whole-program TU — through `bin/kai`'s `KAI_MODULAR=1 --backend=c` path: ~86 per-module TUs compiled in parallel with the `.o` content-hash cache, so a one-module edit recompiles one TU.
2. The result lands in a **staging binary** (`stage2/build/kaic2-fast.bin`) and is sanity-gated (`--version` + a golden demo compiled with no flags, exercising the baked stdlib path) before being swapped into `stage2/kaic2`. A broken build never clobbers the working compiler.

Selection is **explicit, never automatic**: `make kaic2` stays pure bootstrap, `make kaic2-fast` is additive and opt-in. Auto-preferring the fast path was rejected — a stale `kaic2` silently building a wrong `kaic2` is the failure mode to avoid; typing `-fast` is the acknowledgment that you trust the binary currently in place.

Caveats:

- **C backend only.** The fast-built `kaic2` has no native (libLLVM) backend even if the previous one did. For a native-capable compiler run `make KAI_LLVM=1 kaic2`.
- **The first `kaic2` still comes from the bootstrap**, as does anything CI or the selfhost oracle trusts.

### `make kaic2-fast-verify` — equivalence gate

Checks that the fast path builds the *same compiler* as the one in place: the fast-built staging binary and `./stage2/kaic2` must emit **byte-identical C** for the compiler itself (`main.kai`, single-TU) and for a sample program. Run it from a fixed point — `kaic2` built from the current source (right after `make kaic2` it proves bootstrap ≡ fast). It does not touch `kaic2`. Byte-identity of the *binaries* is not expected (multi-`.o` parallel link vs one `-O2` TU); identical emitted C is the functional-parity gate, consistent with how `selfhost` pins determinism.

## `tools/cost-ab.sh` — compile-cost A/B

A change to the compiler is held to its compile cost: an increase over noise blocks the merge until recovered. `tools/cost-ab.sh` is the one way to measure it. Both binaries compile **the same inputs** — the base tree's `stage2/main.kai` (self-compile, run from that tree's `stage2/`), an empty program, and any `.kai` files given — with the base tree's stdlib on an explicit `--path` (a `kaic2` bakes its stdlib path at build time, and a binary that compiled its own tree would charge the branch's source growth to the compiler). `KAI_THREADS=1`, one warm-up, then N alternating rounds (default 3).

```sh
tools/cost-ab.sh --base origin/main                      # base built from the ref, branch = ./stage2/kaic2
tools/cost-ab.sh --base origin/main --rounds 5 prog.kai  # more rounds, one extra probe
tools/cost-ab.sh <base-kaic2> <branch-kaic2> [prog.kai ...]
tools/cost-ab.sh --cache warm --base origin/main prog.kai  # per-project cache on; cold empties it before each run
```

`--base <ref>` builds the ref, and the branch's HEAD, in worktrees of the run's own temp dir (`$TMPDIR/cost-ab.XXXXXX`, removed on exit; concurrent runs do not share it) with two `make kaic2-fast` hops, each seeded by the stored build (`tools/prebuilt-kaic2.sh`) of its merge-base with `origin/main`, so the base source is compiled by a compiler of the base's own code. Run on a quiet machine (load < 3).

The cost is instructions retired (`/usr/bin/time -l` on macOS, `perf stat` on Linux), reported in millions (M); where a host exposes no counters (a virtualised runner) it falls back to CPU milliseconds and the header says so. Per probe it prints min/mean for both sides, the delta in units and %, and a verdict from the stated band: `up` when every branch round is above every base round, `down` when every one is below, else `noise`.

CI runs it on PRs that touch the compiler (the `compiler-touch` filter of `tier1.yml`) as the required `cost-ab` job: the PR's `kaic2-c` artifact against the base commit's bootstrap-cache `kaic2`, posted as one PR comment that later pushes update in place. The job fails when self-compile or the empty program rises by more than 1 %.

### Where the cost goes — `KAI_PASS_CLOCK`

`KAI_PASS_CLOCK=1 kaic2 …` prints one `pass-clock <pass> <monotonic ns>` line per driver pass to stderr (`core` is the core load, then `open` … `emit`); a pass costs the difference from the previous line. It reads the runtime's monotonic clock (the `PassClock` effect), so it is wall time, not instructions: use it to see which pass a cost sits in, and `cost-ab` to size it. The variable is left out of the native-core pre-key. Without it each pass boundary costs one `Env.get`.

## The compile-slot budget

Every place `kai` runs a C or LLVM compile (the c-modular fan-out, the single-TU C compile, runtime owner objects, `[native]` objects, the link lines, and the native backend's per-module emit threads inside `kaic2`) draws a slot from one budget shared by all `kai` processes of the user, so harness workers x per-build fan-out cannot multiply: 14 workers each fanning out 14 compiles is how a 24 GB machine ran out of memory.

- **Under `make -j`** (`MAKEFLAGS` carries `--jobserver-auth=fifo:PATH` or `R,W`; recipes need the `+` prefix or `$(MAKE)` for make to pass the pipe), `kai` is a jobserver client: a process's first compile uses make's implicit token, each further one reads a token and writes it back when the compile is reaped, on exit, and on SIGINT/SIGTERM/SIGHUP.
- **Otherwise** the pool is `N` `flock`ed files `<cache>/jobs/slot-<i>`. The kernel drops the lock when the holder dies, so a crash never leaks a slot. `N` is `KAI_JOBS` when set, else `min(CPUs, RAM / 1 GiB)`. `KAI_MODULAR_JOBS` stays the per-build cap below it.
- **No deadlock:** a process blocks for a slot only while it holds none; with compiles in flight it reaps the oldest first. A `kaic2` native build takes one slot blocking plus up to `KAI_NATIVE_JOBS` - 1 more without waiting, and runs with `KAI_NATIVE_JOBS` set to the number it got; the slots return before the link asks for its own.
- A lone `kai build` on an idle machine gets every slot. One slot per GiB of RAM comes from the measured peaks (serial, `/usr/bin/time -l`): a runtime owner object 68-103 MiB (host C `-O0`/`-O2`) and 80 MiB (wasm32), a cold hello 146 MiB, the largest compiler module TU 492 MiB, a native build of the `kai` CLI 598 MiB (511 MiB on one emit thread). The `kaic2` front end is the one heavy process, 1.8-2.6 GiB on the compiler's own sources; each emit run holds a single slot.

Gate: `make -C stage2 test-job-budget` (`tools/test-job-budget.sh`) starts several cold `kai build` at once and asserts, by sampling the process table, that compilers running together never exceed `KAI_JOBS`, that two builds starting at the same instant under `KAI_JOBS=2` both finish, and that three builds under `make -j3` never exceed 3.

## ThinLTO on the c-modular link

One TU per module keeps cc from inlining across modules, so a call through a forwarder in another module costs what whole-program C does not. A ship build (`kai build --release`) therefore compiles each TU with `-flto=thin` and links with the linker's incremental LTO cache, under `<cache root>/lto` (trimmed with the other shared caches), so a warm relink re-optimises only what changed. `cli_lto.plan` turns it on only when a probe of the toolchain shows both: the compiler is clang (`__clang__`) and a trivial program links with the cache option — `-Wl,-cache_path_lto` on a Mach-O target (ld64), `-fuse-ld=lld -Wl,--thinlto-cache-dir` elsewhere. gcc, or a linker without a cache option, gets the plain modular build: gcc's LTO has no incremental cache, so every relink would re-optimise the whole program. The probe's answer is kept per compiler identity (`probe-<key>` in the same directory); the flags enter the object-cache key through `cflags_compile`, so a toolchain change never reuses stale objects.

A dev build (`kai build`, `kai run`, `kai test`) links without LTO and never runs the probe: the thin link costs about 80 ms on a 41-module edit loop. `KAI_LTO=1` asks for it in any mode (`make kaic2-fast` does, so the compiler it builds keeps its cross-module inlining), `KAI_LTO=0` refuses it; a `CFLAGS` that carries its own `-flto*` or `-O0`, and a debug build, leave it off. Gate: `examples/cache/wrapper_modular_lto.sh` (in `make test-core-cache`) builds as dev (no bitcode, no probe), as `--release` and as `--release` with `KAI_LTO=0` under the default `cc` and `clang`, and asserts equal output, bitcode objects exactly when the probe found a cache, and a warm relink that recompiles nothing and links the same code.

## The pruning differential — `KAI_CORE_PRUNE=off`

Every layer of core pruning (the early cut, demand-driven typing, the typed cut, native-modular partition pruning, lazily completed core impl bodies) is off when kaic2 runs with `--full-core` or with `KAI_CORE_PRUNE=off` in the environment. The variable is a test knob, not a user flag: it reaches kaic2 through `bin/kai` unchanged, and the wrapper's binary memo and the core-object pre-key both fold every `KAI_*` variable, so a whole-core build never shares a cached artefact with a pruned one.

`tools/prune-differential.sh` compiles and runs each program both ways and fails on any difference in exit code, stdout or stderr (paths normalised), and on any `references a core function pruning dropped` in the pruned build even when the whole-core build fails too. Sets: `corpus` (every `examples/**` program with an `.out.expected`, run, tested and under `--json --coverage` where it has test blocks), `combos` (every pair of demand routes from `tools/prune-combos.sh`: operators on primitives, united values, lists, tuples and Option; `sum`/`product`/`max`/`min`/`sort`/`contains`; interpolation; protocol calls; derived `Eq`/`Show`/`Ord`; closures over operators) and `combos-subset`. `make test-prune-differential` runs the subset; `make prune-differential-combos` sweeps every pair on both backends and runs in `daily-tail` as a diagnostic; `make prune-differential` adds the whole corpus and runs in the nightly `prune-differential` workflow, which uploads its report. A program that cannot be compared (nondeterministic output) is listed in `tools/prune-differential-skips.txt`.

## Prebuilt kaic2 — one build per commit, shared by worktrees

A fresh worktree has no compiler, and `make kaic2` boots one in minutes (more for a native-capable one). `tools/prebuilt-kaic2.sh` builds kaic2 + `bin/kai` once per commit and hands the build to every worktree of the repo:

```sh
tools/prebuilt-kaic2.sh build            # once per origin/main commit (native-capable where llvm-config is found)
tools/prebuilt-kaic2.sh seed             # in a worktree: the build of its HEAD, in seconds
tools/prebuilt-kaic2.sh has || tools/prebuilt-kaic2.sh build
tools/prebuilt-kaic2.sh path <ref>   # a stored build's root; <root>/bin/kai runs on its own
```

`seed` restores the build of the commit the worktree branched from `origin/main` at, building it first if it is missing (concurrent seeds wait for one build); commits on top of that base are rebuilt by `make kaic2-fast`, which is C-backend only. An older commit's kaic2 is never used: it may not compile a newer tree. After seeding, edit and rebuild as usual (`make kaic2-fast`). The store is `$KAI_PREBUILT_STORE`, else `${XDG_CACHE_HOME:-~/.cache}/kaikai-prebuilt`, and keeps the newest three builds. A build is keyed `<sha>-<boot release>` (the same source booted from another release is another binary) and is stored only once one more hop of its kaic2 over its own source reproduces its C; an entry keyed by the sha alone is never looked up. A stored root is a dev layout (`bin/kai`, `stage2/kaic2`, and the commit's `stage0/`, `stdlib/`, `stage2/runtime.h`), so a lane can run another commit's compiler from it to compare outputs. Nothing CI or the selfhost oracle trusts comes from it: it is a dev convenience over the same `make kaic2`.

## Before the first push — the gates that turn CI red

Each runs in seconds with a built kaic2. Run the ones a change can touch; a red that one of these would have caught costs a whole CI round.

| Gate | Catches | The fix |
|---|---|---|
| `make test-symid-names-static` | a name's text kept beside an id; a `written_text` read not in `tools/baselines/symid-written-readers/` | key by the id; list a legitimate read with its reason (the file header defines them) |
| `tools/compiler-lint-ratchet.sh` | a new `kai lint` finding in the compiler | fix the finding; its entry file goes with it |
| `make test-walker-catchall-audit` | a new `match` over many AST constructors closed by `_` | explicit arms for a walker; a pure recognizer gets an entry in `tools/baselines/walker-catchall/` with its reason |
| `make test-diag-channel-audit` | a function printing an `error:`/`warning:` line itself instead of building the diagnostic with `mk_diag` and printing it with `render_diag` | return the `Diagnostic`; a function that must print by hand is pinned in `tools/baselines/diag-channel/` with its reason |
| `tools/test-check-parity.sh` | `--diags-json` disagreeing with `kai build` on a negative fixture | report the same diagnostic on both paths |
| `make test-km-new-files` | a new file below `km score` C- | simplify or split along a cohesive seam |
| `examples/cache/*.sh`, `tests/*.sh` | cache keys, memo and trim behaviour of the wrapper | run the ones next to the code touched |
| `tools/baselines/rc-leak/<fixture>` | an RC leak in a new `examples/perceus` fixture | pin the measured ledger line with the fixture |

## Verification targets (root)

- `make tier0` — fast pre-commit sanity (`selfhost` + `demos-no-regression` + arena + heap-limit). Run before committing compiler changes.
- `make tier1` — full suite, the CI merge gate (sharded as `tier1-shard-1` … `tier1-shard-8` in CI; `make test` + demos + fmt + negatives + stdlib-modules + audits + …).
- `make selfhost` — the byte-identity fixed point: `kaic2` compiles its own source to `kaic2b.c`, that compiles to `kaic2b`, which recompiles the source to `kaic2c.c`; asserts `kaic2b.c == kaic2c.c`. The definitive "did I break the compiler" check.
- **Trust CI for the full battery.** Locally run the minimum gate (`make selfhost` + the smoke of your change); leave `tier1`/`tier1-native` to CI.

## Adding a test target or a pin — files that do not conflict

**A fixture needs no wiring.** Drop `<name>.kai` (plus `<name>.out.expected`, or `<name>.err.expected` for a negative) into an `examples/<dir>` that has a `FIXTURES` manifest and CI runs it: `tools/fixture-dir.sh` enumerates the directory, and the directory is one `test-dir-<slug>` unit of the tier1 light pool (`/` in the path written `-`). The stdlib path is always passed.

- **`FIXTURES`** — two keys, both required: `backends:` (`c`, `native`, or `c native`) and `diagnostics: fixed|regex` (how `.err.expected` lines match stderr). An optional `opt: 0` builds every fixture of the directory at -O0 on each backend (`-O0` for the C compile, `KAI_NATIVE_OPT=0` for native), the level at which an optimiser cannot hide a missing tail call, and runs each binary on an 8 MiB main stack (`KAI_MAIN_STACK_SIZE=8388608`) so a missing loop overflows on every host. A new directory of plain run-and-diff fixtures is one such file; nothing else is edited. A directory that lists `native` is also one `test-dir-native-<slug>` unit of the `native-wrapper` pool, run by tier1-native through `bin/kai` with `KAI_BACKEND=native`; native supports no in-file marks.
- **In-file marks** — a line `# fixture: test` compiles the fixture with `--test` and diffs stdout+stderr; `# fixture: no-stdlib` drops the stdlib path for a fixture that tests its absence; `# fixture: threads=<n>` runs it with `KAI_THREADS=<n>`, for output that holds only on one scheduler thread. `# fixture: negative` and `.fmt-class` (`negative` / `nonsubject`, per directory) classify a file for `tests/fmt_property.sh`; `.err.expected` already marks a negative.
- **Per-fixture facts** live in a sibling env file (`<name>.env`, or `main.env` beside a package `main.kai`), one `KEY=value` per line: `KAI_THREADS=<n>` pins the scheduler, `KAI_NATIVE_OPT=<n>` builds the fixture natively at that level, `CFLAGS=<path>...` links root-relative C shims. `kai_corpus_meta` in `tools/lib/corpus.sh` is the one reader (`tools/fixture-meta.sh` for a Makefile recipe); backend parity, the M:N corpus, the effects goldens and `tools/prune-differential.sh` all go through it.
- **Backend gates** (`-native`, `-asan`: one fixture, or a list, built on that backend, run and diffed against its golden, optionally with an RC-ledger bound) are rows of `stage2/gate-variants.txt` run by `tools/fixture-gate.sh`; the target name stays the one the registry lists. The script's header documents the options.
- **Anything that is not a plain fixture directory** (a script, a gate, a bespoke recipe) is one row in `stage2/test-registry.txt`: `<target> <pool>`, pools `light`, `native-wrapper`, `native-gates-1`, `native-gates-2`. Order is irrelevant; `test-light-partition` (tier0) fails on a repeated row or an unknown pool.
- **Shards** — `tools/tier1-light-plan.sh` assigns units to shards by measured cost from `tools/tier1-light-costs.txt` (generated by `tools/tier1-light-costs-refresh.sh`; an unmeasured unit gets the default cost, and every `test-dir-*` unit is elastic).
- **`.PHONY`** — registry targets are phony through `.PHONY: $(TEST_LIGHT_TARGETS) $(TEST_NATIVE_WRAPPER_TARGETS)`. Any other new target declares its own `.PHONY:` line next to its recipe; never extend the long `.PHONY: all …` line.
- **Audit** — `tools/audit-test-wiring.sh` (tier0) fails on a `test-*` target no tier runs, a fixture directory with neither a manifest nor a harness mention, and a recipe that runs the compiler over an `examples/` glob without `--path ../stdlib`.
- **Gate pins** — `tools/baselines/<gate>/<key>`, one file per entry (see `tools/baselines/README.md`). Add a file to pin, edit it to lower, delete it to retire. A baseline, allowlist or ratchet that more than one open change is likely to edit is a directory of one-row files, never a single shared file: two changes touching different entries then never conflict. A ratchet reads the directory and is strict per entry: a finding with no file fails, a file with no finding fails.

## Backend parity — one fixture

To diff a fixture's output between backends, use the harness, do not hand-roll native-vs-C:

```
TARGET_BACKEND=native ORACLE_BACKEND=c BACKEND_PARITY_JOBS=1 \
  BACKEND_PARITY_DIRS="examples/perceus" tools/test-backend-parity.sh
```

`BACKEND_PARITY_JOBS=1` = serial (the parallel ratchet is false-green; serial is authoritative). `BACKEND_PARITY_DIRS` scopes the corpus. The ratchet gates against `tools/native-parity-baseline.txt` (must stay empty = full parity).

## Host facilities — the `KAI_HOST_*` macros

`stage2/runtime.h` puts each facility a small host cannot provide behind one macro, defined to 1 unless the build says otherwise:

| macro | facility | at 0 |
|---|---|---|
| `KAI_HOST_FIBERS` | context switch by `ucontext` | one thread, one fiber; a spawn stops the program |
| `KAI_HOST_STACK_GUARD` | mapped stacks, the guard page, the overflow handler | no stack segment: a handler that keeps `resume` stops with the message a target without the switch already prints |
| `KAI_HOST_REACTOR` | self-pipes, `poll`, the file pool | a wait (`Clock.sleep_ns`, a `File` read, `Stdin.read_line`) stops the program |
| `KAI_HOST_SOCKETS` | sockets and DNS | every `NetTcp` / `NetUdp` / `NetUnix` / `NetDns` op stops the program |
| `KAI_HOST_PROCESS` | child processes | every `Process` op but `exit` stops the program |
| `KAI_HOST_SIGNALS` | signal subscription | every `Signal` op stops the program |
| `KAI_HOST_BACKTRACE` | the stack trace of a debug build's panic | the panic prints, then one line saying there is no trace |
| `KAI_HOST_UNWIND` | the frame unwinder | a contained trap lands at once; the frames it crosses keep their references |

A stop is `kai: this host has no <facility>` on stderr and status 1. With a macro at 0 the runtime includes none of the facility's headers and imports none of its libc entry points, so nothing is left for the host to stub. Three combinations are refused at compile time: fibers without mapped stacks, sockets / processes / signals without the reactor, and `KAI_MAIN_STACK_SIZE` without fibers.

The macros are a C-backend configuration, passed through `CFLAGS` (the wrapper's own flags must then be repeated, since `CFLAGS` replaces them):

```
CFLAGS="-std=c99 -Wno-unused-function -Wno-unused-variable -O2 -DKAI_HOST_FIBERS=0 -DKAI_HOST_STACK_GUARD=0" \
  bin/kai build --backend=c app.kai -o app
```

The native runtime is two halves — the owner object, compiled per build with `CFLAGS`, and the hot bitcode, generated when the compiler is built — and only the first sees the macros; the two would disagree on the layout of a fiber.

- **Gate**: `make -C stage2 test-host-minimal` (tier 1) runs `tools/test-host-minimal.sh`: the `examples/runtime/host_minimal_*` fixtures with every macro at 0, the import check on each binary, and the runtime compiled with each facility off on its own.
- **A change to `runtime.h` that must not reach existing platforms** is checked with `tools/runtime-preprocess-diff.sh`: `cc -E -P` of the runtime at the base against the working tree, in every shape a build instantiates it, equal up to blank lines. Equal text is equal object code, so such a change needs no runtime or compile-cost measurement. The text depends on the host's headers; run it on each platform. A PR labelled `runtime-text-identical` is held to it in CI on Linux x86_64 (`tier1`, under GCC and clang) and Linux aarch64 (`linux-aarch64`); the label is read when the step runs, so one set after the PR opened counts on a re-run.

## WebAssembly — a module and its host

Under `__wasm__` the eight `KAI_HOST_*` macros default to 0 (a build may still define one); `File` runs its work in place, since there is no reactor and one fiber has nothing to hold. WebAssembly is reached through the C backend and a cross compiler, or through the native backend (below); wasi-sdk 34 is pinned in `tier1.yml` by URL and checksum. The wrapper honours `CC` and `CFLAGS`; the cache and the link flags of a macOS host do not apply to wasm, so the build is single-unit:

```
CC=$WASI_SDK/bin/clang \
CFLAGS="--target=wasm32-wasip1 -O2 -mexception-handling -mtail-call -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false \
        -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID \
        -lwasi-emulated-signal -lwasi-emulated-mman -lwasi-emulated-process-clocks -lwasi-emulated-getpid -lsetjmp" \
KAI_MODULAR=0 KAI_BACKEND=c bin/kai build app.kai -o app.wasm
node stage2/web/kai_host.mjs app.wasm
```

`stage2/web/kai_host.mjs` instantiates the module with the `wasi_snapshot_preview1` imports it uses, line-buffers fd 1 and 2 to the console (and to `#kai-stdout` when the page has one) and resolves `run(wasm, opts)` with the exit status. It needs a node with exnref (24.15 or later; an older node takes `--experimental-wasm-exnref`, set through `WASM_NODE_FLAGS`). A module that spawns fibers or keeps continuations also needs JSPI; the host refuses it at load on an engine without, and the gate retries such a program with `--experimental-wasm-jspi`.

- **Gate**: `TARGET_BACKEND=wasm WASI_SDK=<dir> NATIVE_PARITY_RATCHET=1 tools/test-backend-parity.sh` builds each corpus fixture for the oracle (C backend on the host) and for wasm, runs the second under node at `KAI_THREADS=1`, and compares output and exit status. `tools/wasm-parity-baseline.txt` lists the fixtures allowed to differ: a new difference fails, a closed one must be removed. CI runs it as `tier1-wasm` (not a required check), on the corpus plus `examples/web/`, one fixture per class of program.
- The host grants no filesystem and no stdin: fixtures that read files, stdin, `PATH` or `/tmp` differ and sit in the baseline.

### wasm32-wasi

`kai build --target=wasm32-wasi` compiles the same way with `-DKAI_HOST_JS=0`: no `kai` or `kai_js` import, no stack exports, and `KAI_HOST_FIBERS` defaults to 0, so the module is one file a WASI runtime loads. `kai run --target=wasm32-wasi` runs it on `wasmtime` from `PATH` with the current directory preopened (`KAI_WASI_DIRS` replaces the list).

- **Gate**: `TARGET_BACKEND=wasi NATIVE_PARITY_RATCHET=1 tools/test-backend-parity.sh` builds each corpus fixture with `bin/kai build --target=wasm32-wasi` and runs it under `wasmtime` with `.` and `/tmp` preopened. A fixture whose `main` the target refuses (`Spawn`, sockets, signals, `Js`) is counted `refused`, not as a failure; `tools/wasi-parity-baseline.txt` lists only real differences. CI runs it as `tier1-wasi` (not a required check) together with `tools/test-wasi.sh`, which checks `kai run`'s arguments, environment, stdin, exit status, preopens and diagnostics.

### The native backend for WebAssembly

`KAI_NATIVE_TARGET=wasm32-wasi|wasm32-browser` (set by `kai` from `--target`) makes kaic2 build the module for `wasm32-unknown-wasip1` / `wasm32-unknown-unknown` instead of the host: the triple and target machine come from the target, relocation is static, `_setjmp` becomes `setjmp` for the wasm sjlj lowering, and cleanup pads take `__gxx_wasm_personality_v0`. LLVM keeps `invoke` only under the wasm exception model, which the C API cannot select, so `stage2/runtime/llvm_wasm_eh.cpp` (compiled by `stage2/Makefile` when `llvm-config` lists WebAssembly, `-DKAI_LLVM_WEBASSEMBLY`) sets it on the target machine. A kaic2 without that target stops with "this kai was built without the WebAssembly LLVM target".

The layout follows 32-bit pointers through two rules: the evidence blob and node are `i64` cells, indexed by `kaix_ev_op_at` / `kaix_ev_set_op` at an 8-byte stride; and `cons` cells keep an 8-byte stride between `head` and `tail`, because the emitter reads them as variant slots.

`tools/gen-runtime-bc.sh` writes `stage0/runtime_{llvm,inline}.<target>.bc` for both targets when a wasi-sdk sysroot is found (`WASI_SYSROOT`, `KAI_WASI_SDK`, `WASI_SDK`, or the `kai` cache), using the clang that matches the libLLVM (wasi-sdk's own clang is newer and cannot write them). The `.macros` file beside each records its `-DKAI_*` macros; `kai` stops a build whose `CFLAGS` set others, and kaic2 refuses bitcode of another architecture than the target.

- **Gate**: `TARGET_BACKEND=wasi|wasm WASM_BACKEND=native NATIVE_PARITY_RATCHET=1 tools/test-backend-parity.sh` against `tools/{wasi,wasm}-native-parity-baseline.txt`; it prints `SKIP` when the kaic2 has no WebAssembly target. CI runs it as `native-wasm` in `tier1-native.yml` (not a required check).

## Traps (verified, recurring)

- **`stage2/main.kai` is a stub**, not the compiler. The compiler is the ~200 modules it reaches through `import compiler.driver`.
- **`kai fmt` is in-place destructive** — never run it on compiler/stdlib sources; redirect output to `/tmp`.
- **`runtime.h` has TWO copies** (`stage0/runtime.h` + `stage2/runtime.h`). A runtime prim/handler added to one must be added to BOTH; they change together.
- **`stage2/runtime.h` is an umbrella over `stage2/runtime/*.h`.** The parts are fragments with no guards of their own; anything that reads the runtime by path (tarball, bootstrap seed, bitcode staleness key, `runtime-global-audit`, build-cache keys) takes the umbrella and every part, and `cli_runtime_headers` is the one helper the `kai` caches key through. The emitter defines `KAI_NO_LLVM_BRIDGE` and `KAI_NO_BENCH_CHECK` ahead of `#include "runtime.h"` in a translation unit that calls neither the libLLVM forwarders nor the bench/check runtime, so those parts (`llvm_bridge.h`, `harness_extra.h`) stay out of user programs; `KAI_LLVM` builds always include the bridge. `tests/runtime_gates.sh` pins it.
- **Header prerequisites are hand-declared.** Stage 1 and stage 2 compile a *generated* `.c` whose `#include "runtime.h"` make cannot see, so each rule names the header it actually binds under its own `-I` order (`stage2` → `stage2/runtime.h`, `stage1` → `stage0/runtime.h`). Adding a rule that compiles either translation unit means adding that prerequisite too, and only that one — naming a header the TU never reaches turns unrelated edits into a ~70 s `-O2` rebuild. `make test-header-deps` (tier 0) asserts both directions.
- **The native paths compile a COPY of the entry file**, not the file the user named — the whole-program path under the run's `$tmp`, the native-modular path under the content-addressed cache dir — because `kaic2` derives the object path and the cache keys from the path it is handed. Anything that renders that path back to the user (diagnostics, DWARF) must map it to the real source: `kai` corrects the captured stderr (`cli_plan.rewrite_paths`), and `KAI_DEBUG_SRC` does the same for the DIFile. The C path never copies. Gate: `make test-native-diag-path` (tier 1, and the tier1-native shard 2).
- **A `KAI_LLVM=1` link does not prove native works.** A `kaic2` compiled by `kaic2` can link libLLVM and still crash emitting native, and neither `selfhost` nor a C-backend run notices. The link recipe runs `tools/native-probe.sh` (a one-line `--emit=native`) on the fresh binary and deletes it when the probe fails; `KAI_NATIVE_PROBE=0` skips the probe to keep a broken binary for diagnosis. Gate: `make test-native-probe` (tier 0).
- **mtime trap**: make decides rebuilds by timestamps, which don't survive a checkout or artifact download. After such, the binary chain may look stale; `make` rebuilds what it thinks is needed.
- **Doc-only changes** (diff confined to `docs/`, root `*.md`, `LICENSE`) skip every tier locally and in CI (`paths-ignore`). Code paths always trigger tiers.
