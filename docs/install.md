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

| Backend | Toolchain | When it runs       |
|---------|-----------|--------------------|
| `c`     | `cc`      | Always available — portable C with the in-tree runtime header. |
| `llvm`  | `clang`   | Faster build line; emits LLVM IR directly from `kaic2`. |

`kai` picks the backend automatically at every invocation:

1. The `--backend=<c|llvm>` CLI flag wins, if passed.
2. Otherwise, the `KAI_BACKEND` environment variable wins (must be
   `c` or `llvm`; anything else is an error).
3. Otherwise, `kai` auto-detects: if `clang` is on `PATH`, the LLVM
   backend is used; if not, the C backend is used.

Both backends compile the full stdlib as of the L2 LLVM-default lane
(2026-05-11). Earlier notes about `KAI_NO_STDLIB=1` being required
for `--backend=llvm` no longer apply.

### Forcing the C path

If a build under LLVM misbehaves and you want to bisect:

```sh
kai build src/app.kai --backend=c -o app.c
# or, for an entire shell session:
export KAI_BACKEND=c
```

### Environment knobs

| Variable        | Effect |
|-----------------|--------|
| `KAI_BACKEND`   | Default backend (`c` or `llvm`); overridden by `--backend`. |
| `CC`            | C compiler invoked by the C backend (default: `cc`). |
| `CFLAGS`        | Extra flags appended to `CC`. |
| `CLANG`         | `clang` binary used by `--backend=llvm` (default: `clang`). |
| `CLANGFLAGS`    | Extra flags appended to `CLANG` (default: `-w -O2`). |
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
# kaikai 0.52.0 (stage 2, self-hosted)

kai run examples/minimal/hello.kai
# Hello, kaikai

kai build --backend=c    examples/minimal/hello.kai -o /tmp/hello-c
kai build --backend=llvm examples/minimal/hello.kai -o /tmp/hello-llvm
diff <(/tmp/hello-c) <(/tmp/hello-llvm) && echo "backends match"
```

The driver smoke test (`tools/test-llvm-driver.sh`) cross-builds a
handful of fixtures with both backends and diffs their stdout — run it
locally to confirm the toolchain is wired correctly.
