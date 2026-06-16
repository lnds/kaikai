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
| `c`      | `cc`             | Default — portable C with the in-tree runtime header. The bootstrap path and the parity oracle, supported across the whole corpus. |
| `native` | in-process libLLVM | Opt-in. Builds the LLVM module via the C API and emits a native object directly (no `.ll` text, no `clang`). The intended default destination; opt-in until native-vs-C parity is complete. Needs a `kaic2` built with libLLVM (`make -C stage2 KAI_LLVM=1`). |

`kai` resolves the backend at every invocation:

1. The `--backend=<c|native>` CLI flag wins, if passed.
2. Otherwise, the `KAI_BACKEND` environment variable wins (must be
   `c` or `native`; anything else is an error).
3. Otherwise the default is `c`.

A `kaic2` built without libLLVM rejects `native` with an actionable
error — it never silently falls back to C.

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
| `KAI_NO_STDLIB` | If `1`, skip the auto-loaded stdlib preludes. |

## Getting clang

The LLVM backend needs `clang` on `PATH`. The brew formula does not
declare `depends_on "llvm"` — installation is opportunistic: if clang
is around kai uses it, otherwise the C path keeps working.

### macOS

clang ships with the Xcode Command Line Tools and is on `PATH` by
default once they are installed:

```sh
xcode-select --install
```

No further action needed — `kai build hello.kai` will auto-detect
clang and pick the LLVM backend.

### Linux

Install clang from your distro:

```sh
# Debian / Ubuntu
sudo apt install clang

# Fedora / RHEL
sudo dnf install clang

# Arch
sudo pacman -S clang
```

The C path is always available regardless; clang is the speed-up, not
a prerequisite.

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
