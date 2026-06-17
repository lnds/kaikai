#!/bin/sh
# scripts/build-release.sh — package a kaikai release tarball.
#
# Builds the full bootstrap chain (kaic0 → kaic1 → kaic2), assembles the
# installed-layout directory tree, and produces a gzipped tarball plus a
# SHA-256 checksum file. Designed to be invoked from CI on a tag push or
# locally for smoke-testing.
#
# Usage:
#   scripts/build-release.sh [<output-dir>]
#
# Default output: dist/  (created if missing).
#
# Outputs:
#   dist/kaikai-v<VERSION>-<os>-<arch>.tar.gz
#   dist/kaikai-v<VERSION>-<os>-<arch>.tar.gz.sha256
#
# Layout inside the tarball:
#   kaikai-v<VERSION>-<os>-<arch>/
#     bin/kai
#     libexec/kaikai/kaic2
#     share/kaikai/
#       VERSION
#       EDITION
#       stdlib/...
#       include/runtime.h
#       demos/baseline.txt
#       info/...                   # `kai info` reference pages
#     README.md
#     LICENSE

set -eu

# Resolve repo root (script lives at <root>/scripts/).
SCRIPT="$0"
case "$SCRIPT" in
  /*) ;;
  *)  SCRIPT="$(pwd)/$SCRIPT" ;;
esac
SCRIPT_DIR="$(cd "$(dirname "$SCRIPT")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT"

VERSION="$(cat VERSION)"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"   # darwin / linux
ARCH="$(uname -m)"                                # arm64 / x86_64

case "$OS-$ARCH" in
  darwin-arm64|darwin-x86_64|linux-x86_64|linux-aarch64) ;;
  *)
    echo "build-release.sh: unsupported platform $OS-$ARCH" >&2
    exit 2
    ;;
esac

DIST="${1:-$ROOT/dist}"
mkdir -p "$DIST"

NAME="kaikai-v$VERSION-$OS-$ARCH"
STAGE="$DIST/$NAME"

echo "==> building kaikai $VERSION for $OS-$ARCH"

# Wipe any prior staging tree (clean slate per run).
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" \
         "$STAGE/libexec/kaikai" \
         "$STAGE/share/kaikai/stdlib" \
         "$STAGE/share/kaikai/include" \
         "$STAGE/share/kaikai/demos" \
         "$STAGE/share/kaikai/info"

# Static libLLVM for the in-process native backend (Lane 1.5 flip: native
# is the DEFAULT `kai build` destination, so the shipped kaic2 MUST carry
# it). The release links libLLVM STATICALLY (Rust/Zig/Julia model) — the
# distributed binary runs `kai build` out-of-the-box with no system LLVM
# and the brew formula needs no `depends_on llvm`. The vendored build is
# produced on-demand by the top-level Makefile (`make llvm-build`:
# download + cmake MinSizeRel + compile the narrow X86+AArch64 static
# archive set, gitignored under stage0/third_party/llvm). It is
# idempotent — a populated build/ is a no-op, so a cached tree (the CI
# llvm cache, see release.yml) skips the long cold compile.
#
# LLVM_CONFIG points at the vendored STATIC llvm-config so the stage2
# link resolves --link-static --libs against the archives we built, not a
# system/Homebrew dynamic libLLVM. The stage2 Makefile already picks
# -lc++ (macOS) vs -lstdc++ (Linux) per UNAME_S and threads --link-static
# through; this just aims it at the vendored prefix.
echo "==> building vendored static libLLVM (idempotent; cached build/ is a no-op)"
make llvm-build >&2
LLVM_CONFIG="$ROOT/stage0/third_party/llvm/build/bin/llvm-config"
if [ ! -x "$LLVM_CONFIG" ]; then
  echo "build-release.sh: vendored llvm-config missing at $LLVM_CONFIG after make llvm-build" >&2
  exit 2
fi
export LLVM_CONFIG
echo "    using $LLVM_CONFIG ($("$LLVM_CONFIG" --version))"

# Bootstrap chain. kaic0 (C) → kaic1 (kaikai-from-C) → kaic2 (self-hosted).
# kaic0/kaic1 stay cc-only (Tier 1: the bootstrap never couples to
# libLLVM). Only kaic2 is built KAI_LLVM=1 — FORCE native-capable so a
# missing/broken vendored libLLVM breaks the release loudly instead of
# silently shipping a C-only binary that cannot honour the native default.
echo "==> bootstrapping kaic0 → kaic1 → kaic2 (kaic2: KAI_LLVM=1, static libLLVM)"
make -C stage0 kaic0 >&2
make -C stage1 kaic1 >&2
make -C stage2 KAI_LLVM=1 LLVM_CONFIG="$LLVM_CONFIG" kaic2 >&2

# Verify byte-identical selfhost before packaging — mandatory per CLAUDE.md.
# selfhost goes through the C path (the bootstrap oracle), so it is
# unaffected by the native link; build the selfhost-comparison kaic2 the
# same way the gate does (default Makefile rules).
echo "==> verifying selfhost byte-identical"
make -C stage1 selfhost >&2
make -C stage2 selfhost >&2

# Build kai-pkg (the package-manifest helper). bin/kai dev-mode auto-builds
# it on first use, but installed mode expects it pre-shipped at
# libexec/kaikai/kai-pkg — without it, any project with a kai.toml fails
# with "installation is corrupt" (issue #512). Use the freshly-built kaic2
# via bin/kai to compile its sources. Force the C backend explicitly:
# kai-pkg currently segfaults at runtime under the LLVM backend on
# subcommands that read deps (`paths`, `install`, …) — root cause not
# yet diagnosed, tracked as a follow-up to issue #513 (which only
# closed the static "undefined `@kai_file_read_bytes`" link failure
# in the LLVM IR emitter). The C path is the safe shipping backend
# until the runtime gap is fixed.
echo "==> building kai-pkg"
KAI_BACKEND=c ./bin/kai build tools/kai-pkg/main.kai -o tools/kai-pkg/kai-pkg >&2

# Build kai-lsp (the Language Server, issue #447). Same rationale as
# kai-pkg: installed mode looks for libexec/kaikai/kai-lsp, dev mode
# auto-builds. Force the C backend for the same reason — LLVM path
# is opt-in until the runtime gap closes.
echo "==> building kai-lsp"
KAI_BACKEND=c ./bin/kai build tools/kai-lsp/main.kai -o tools/kai-lsp/kai-lsp >&2

# Copy artifacts.
echo "==> assembling installed layout at $STAGE"
cp bin/kai             "$STAGE/bin/kai"
chmod +x               "$STAGE/bin/kai"
cp stage2/kaic2        "$STAGE/libexec/kaikai/kaic2"
chmod +x               "$STAGE/libexec/kaikai/kaic2"
cp tools/kai-pkg/kai-pkg "$STAGE/libexec/kaikai/kai-pkg"
chmod +x               "$STAGE/libexec/kaikai/kai-pkg"
cp tools/kai-lsp/kai-lsp "$STAGE/libexec/kaikai/kai-lsp"
chmod +x               "$STAGE/libexec/kaikai/kai-lsp"

# Stdlib: copy the whole tree preserving structure.
(cd stdlib && tar -cf - .) | (cd "$STAGE/share/kaikai/stdlib" && tar -xf -)

# Runtime header + LLVM-path runtime shim (L1+ LLVM-direct).
# The shipped compiler is stage2/kaic2, whose emitted C calls kai_intf,
# kai_is_int and the reuse-token helpers that ONLY live in
# stage2/runtime.h (the tagged-Int Koka runtime). bin/kai documents the
# installed layout as carrying "a single runtime.h (the stage 2 one)".
# Shipping stage0/runtime.h here was a latent bug from the script's first
# commit: the C smoke test passes whenever the smoke source never touches
# the tagged-Int path, and fails (undeclared kai_intf) the moment a
# kai.toml project pulls in stdlib that does — which is why releases have
# failed intermittently since stage2 became the compiler.
#
# The llvm-c-parity lane unified the LLVM shim (runtime_llvm.c) onto the
# same stage2 runtime.h, so this one header now serves BOTH backends when
# installed: bin/kai points RUNTIME_INC (LLVM) and RUNTIME_INC_C (C) at
# share/kaikai/include, and the shim's `#include <runtime.h>` binds here.
cp stage2/runtime.h        "$STAGE/share/kaikai/include/runtime.h"
cp stage0/runtime_llvm.c   "$STAGE/share/kaikai/include/runtime_llvm.c"

# P2 (docs/native-codegen-perf-plan.md §P2): ship the native-runtime bitcode
# so the installed `kai build --backend=native` links it before O2 and
# inlines the runtime ops. The `make -C stage2 KAI_LLVM=1 kaic2` above
# already generated stage0/runtime_llvm.bc (via tools/gen-runtime-bc.sh,
# clang 18). The bitcode encodes THIS build host's data layout, so it is
# correct only for a tarball of the SAME platform — which is exactly what a
# release runner produces (the macOS-arm64 runner builds the macOS-arm64
# tarball). bin/kai resolves it next to runtime_llvm.c under
# share/kaikai/include in the installed layout. ASSERT it is present and
# active: a release that silently fell back to the legacy cc-link path would
# ship the slow native default. (build-release runs on a clang-18 runner;
# the assert turns a dropped clang-18 into a red release, not a silent perf
# regression — asu guard #2.)
if ! "$ROOT/tools/assert-runtime-bc.sh" >&2; then
  echo "build-release.sh: native P2 bitcode not generated — clang 18 missing on the release host." >&2
  echo "  Install clang 18 (brew install llvm@18 / apt-get install clang-18) before releasing," >&2
  echo "  or the shipped native backend would run the slow legacy path." >&2
  exit 2
fi
cp stage0/runtime_llvm.bc  "$STAGE/share/kaikai/include/runtime_llvm.bc"

# Metadata.
cp VERSION             "$STAGE/share/kaikai/VERSION"
cp EDITION             "$STAGE/share/kaikai/EDITION"
if [ -f demos/baseline.txt ]; then
  cp demos/baseline.txt "$STAGE/share/kaikai/demos/baseline.txt"
fi

# `kai info` reference pages. Source lives at docs/info/*.md; the
# installed layout puts them under share/kaikai/info/ where bin/kai
# looks for them (see `cmd_info` in bin/kai). Distributing through the
# tarball means brew tap users get them automatically — the formula
# only bumps version+url+sha and never touches contents.
if [ -d docs/info ]; then
  (cd docs/info && tar -cf - .) | (cd "$STAGE/share/kaikai/info" && tar -xf -)
fi

# User-visible docs.
cp README.md           "$STAGE/README.md"
cp LICENSE             "$STAGE/LICENSE" 2>/dev/null || true

# Assert the staged kaic2 actually carries the native backend (static
# libLLVM linked IN). The whole point of the L3 static link is that the
# shipped binary runs the native DEFAULT with no system LLVM; a kaic2
# that silently fell back to C-only would pass the C smoke below and ship
# a binary that degrades on every user's first `kai build`. Probe the
# capability directly: `--emit=native` on a C-only kaic2 aborts with the
# known sentinel, on a native-capable one it lowers (and fails only for a
# missing input, which we don't hit since we pass none → usage error, not
# the sentinel). We grep for the sentinel and FAIL if present.
echo "==> verifying staged kaic2 carries the native backend (static libLLVM)"
CAP_OUT="$(printf 'fn main():Unit=()\n' > "$DIST/.cap.kai"; \
  "$STAGE/libexec/kaikai/kaic2" --emit=native "$DIST/.cap.kai" 2>&1 || true)"
rm -f "$DIST/.cap.kai" "$DIST/.cap.o" 2>/dev/null || true
if printf '%s' "$CAP_OUT" | grep -q "not built into this compiler"; then
  echo "build-release.sh: staged kaic2 is C-only — static libLLVM did NOT link in." >&2
  echo "  The release must ship a native-capable kaic2 (native is the default backend)." >&2
  echo "  Check 'make llvm-build' produced the vendored archives and KAI_LLVM=1 was honoured." >&2
  exit 2
fi
echo "    staged kaic2: native backend present"

# Smoke test: assert the staged tree resolves itself correctly without
# falling back to the dev checkout.
echo "==> smoke test: hello.kai through staged bin/kai"
SMOKE_DIR="$(mktemp -d)"
trap 'rm -rf "$SMOKE_DIR"' EXIT
cat > "$SMOKE_DIR/hello.kai" <<'HEREDOC'
fn main() : Unit / Console = print("hola installed mode")
HEREDOC
# Run the staged kai with KAI_NO_STDLIB unset and PATH pointing nowhere
# else, to make sure the script's own auto-detection logic works.
# We exercise BOTH backends explicitly: `native` (the DEFAULT since the
# Lane 1.5 flip — what real users hit on their first `kai build`, served
# by the static libLLVM linked into the staged kaic2) and `c` (the
# portable oracle, always available). If the native default is broken in
# the staged layout, the release must fail; we will not ship a binary
# that breaks on its advertised default. The `|| true` lets us always log
# the staged kai output before deciding pass/fail.
run_smoke() {
  backend="$1"
  out="$SMOKE_DIR/out.$backend"
  echo "==> smoke test: $backend backend"
  PATH="$STAGE/bin:/usr/bin:/bin" KAI_BACKEND="$backend" \
    "$STAGE/bin/kai" run "$SMOKE_DIR/hello.kai" > "$out" 2>&1 || true
  echo "--- staged kai output ($backend) ---" >&2
  cat "$out" >&2 || true
  echo "--- end staged kai output ($backend) ---" >&2
  if grep -q 'hola installed mode' "$out"; then
    echo "    $backend backend OK"
    return 0
  else
    echo "build-release.sh: smoke test failed ($backend backend)" >&2
    return 1
  fi
}
smoke_failed=0
run_smoke native || smoke_failed=1
run_smoke c      || smoke_failed=1

# Smoke test #2 — kai.toml project (issue #512). A release that ships
# without kai-pkg breaks every multi-file project; the previous smoke
# test only exercises single-file `kai run` which doesn't touch kai-pkg.
echo "==> smoke test: kai.toml project (manifest resolution)"
PROJ_DIR="$SMOKE_DIR/proj"
mkdir -p "$PROJ_DIR"
printf 'name = "smoke"\nversion = "0.1.0"\n' > "$PROJ_DIR/kai.toml"
cat > "$PROJ_DIR/main.kai" <<'HEREDOC'
fn main() : Unit / Console = print("hola manifest mode")
HEREDOC
PATH="$STAGE/bin:/usr/bin:/bin" KAI_BACKEND=c \
  "$STAGE/bin/kai" run "$PROJ_DIR/main.kai" > "$SMOKE_DIR/out.proj" 2>&1 || true
echo "--- staged kai output (manifest) ---" >&2
cat "$SMOKE_DIR/out.proj" >&2 || true
echo "--- end staged kai output (manifest) ---" >&2
if grep -q 'hola manifest mode' "$SMOKE_DIR/out.proj"; then
  echo "    manifest project OK"
else
  echo "build-release.sh: smoke test failed (kai.toml project) — issue #512?" >&2
  smoke_failed=1
fi

if [ "$smoke_failed" -ne 0 ]; then
  exit 2
fi

# Pack the tarball.
echo "==> packing $NAME.tar.gz"
(cd "$DIST" && tar -czf "$NAME.tar.gz" "$NAME")
(cd "$DIST" && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256")

# Drop the staging tree — the tarball is the artifact.
rm -rf "$STAGE"

echo
echo "==> release artifact:"
ls -lh "$DIST/$NAME.tar.gz"
echo "==> sha256:"
cat "$DIST/$NAME.tar.gz.sha256"
