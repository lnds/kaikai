# Installing kaikai

`kai` is a single shell-script driver that wraps the bootstrap chain
(`kaic0` → `kaic1` → `kaic2`) and the active codegen backend. From a
git checkout:

```sh
git clone https://github.com/lnds/kaikai
cd kaikai
make tier0       # builds stage 0, stage 1, stage 2; runs fast tests
./bin/kai run examples/minimal/hello.kai
```

The first `bin/kai` invocation auto-builds any missing stage binary;
subsequent calls reuse them. Stages 0 and 1 only need a C99 `cc`; the
user-facing path is stage 2.

## Backends

kaikai targets native binaries via one of two backends:

| Backend  | Toolchain        | When it runs       |
|----------|------------------|--------------------|
| `native` | in-process libLLVM | **Default.** Builds the LLVM module via the C API and emits a native object directly (no `.ll` text, no `clang`). libLLVM is linked into `kaic2` (statically in a release), so it runs with no system LLVM. Needs a `kaic2` built with libLLVM (`make -C stage2 KAI_LLVM=1`; release tarballs ship one). |
| `c`      | `cc`             | Portable C with the in-tree runtime header. The bootstrap path and the parity oracle, supported across the whole corpus. Select with `--backend=c`. |

`kai` resolves the backend at every invocation:

1. The `--backend=<c|native>` CLI flag wins, if passed.
2. Otherwise, the `KAI_BACKEND` environment variable wins (must be
   `c` or `native`; anything else is an error).
3. Otherwise the default is `native`.

A `kaic2` built without libLLVM (the cc-only bootstrap) cannot run
`native`. When `native` is the *implicit* default, such a `kaic2`
degrades to `c` with a note, so a checkout without LLVM keeps building.
An *explicit* `--backend=native` / `KAI_BACKEND=native` on that `kaic2`
errors instead — it never silently falls back on an explicit request.

### Forcing the C path

```sh
kai build src/app.kai --backend=c -o app
# or, for an entire shell session:
export KAI_BACKEND=c
```

### Environment knobs

| Variable        | Effect |
|-----------------|--------|
| `KAI_BACKEND`   | Default backend (`c` or `native`); overridden by `--backend`. |
| `CC`            | C compiler invoked by the C backend / the native object link (default: `cc`). |
| `CFLAGS`        | Extra flags appended to `CC`. |
| `KAI_NATIVE_OPT`| Optimisation level for the native backend's in-process LLVM pipeline (`0|1|2|3|s|z`; default `2`). |
| `KAI_NO_STDLIB` | If `1`, skip the auto-loaded stdlib core modules. |

## The native backend and libLLVM

The native backend does **not** shell out to `clang` and does **not**
need a system LLVM on `PATH`. It builds the LLVM module in-process via
the C API, and libLLVM is linked directly into `kaic2` — statically in a
release tarball / brew install. The brew formula therefore declares no
`depends_on "llvm"`: a `kai` installed from a release runs the native
default out of the box.

A C compiler (`cc`) is still needed to link the emitted native object
against the runtime and to drive the C backend; the Xcode Command Line
Tools (macOS) or your distro's `build-essential` / `gcc` (Linux) provide
it.

If you build `kaic2` yourself from a checkout, native capability is
auto-detected from `llvm-config`: present → native-capable, absent →
a cc-only `kaic2` whose implicit default degrades to the C backend.
Force it on with `make -C stage2 KAI_LLVM=1`.

### Windows

Out of scope for the MVP. WSL with the Linux instructions above is the
supported path.

## Verifying the install

```sh
kai --version
# kaikai 0.79.0 - hanga-roa (stage 2, self-hosted)

kai run examples/minimal/hello.kai
# Hello, kaikai

kai build --backend=c      examples/minimal/hello.kai -o /tmp/hello-c
kai build --backend=native examples/minimal/hello.kai -o /tmp/hello-native
diff <(/tmp/hello-c) <(/tmp/hello-native) && echo "backends match"
```

The backend-parity harness (`tools/test-backend-parity.sh`) cross-builds
every fixture with the native backend and the C oracle and diffs their stdout — run it
locally to confirm the toolchain is wired correctly.
