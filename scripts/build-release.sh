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
#       stdlib/...
#       include/runtime.h
#       demos/baseline.txt
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
         "$STAGE/share/kaikai/demos"

# Bootstrap chain. kaic0 (C) → kaic1 (kaikai-from-C) → kaic2 (self-hosted).
echo "==> bootstrapping kaic0 → kaic1 → kaic2"
make -C stage0 kaic0 >&2
make -C stage1 kaic1 >&2
make -C stage2 kaic2 >&2

# Verify byte-identical selfhost before packaging — mandatory per CLAUDE.md.
echo "==> verifying selfhost byte-identical"
make -C stage1 selfhost >&2
make -C stage2 selfhost >&2

# Copy artifacts.
echo "==> assembling installed layout at $STAGE"
cp bin/kai             "$STAGE/bin/kai"
chmod +x               "$STAGE/bin/kai"
cp stage2/kaic2        "$STAGE/libexec/kaikai/kaic2"
chmod +x               "$STAGE/libexec/kaikai/kaic2"

# Stdlib: copy the whole tree preserving structure.
(cd stdlib && tar -cf - .) | (cd "$STAGE/share/kaikai/stdlib" && tar -xf -)

# Runtime header.
cp stage0/runtime.h    "$STAGE/share/kaikai/include/runtime.h"

# Metadata.
cp VERSION             "$STAGE/share/kaikai/VERSION"
if [ -f demos/baseline.txt ]; then
  cp demos/baseline.txt "$STAGE/share/kaikai/demos/baseline.txt"
fi

# User-visible docs.
cp README.md           "$STAGE/README.md"
cp LICENSE             "$STAGE/LICENSE" 2>/dev/null || true

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
PATH="$STAGE/bin:/usr/bin:/bin" "$STAGE/bin/kai" run "$SMOKE_DIR/hello.kai" \
  > "$SMOKE_DIR/out" 2>&1
if grep -q 'hola installed mode' "$SMOKE_DIR/out"; then
  echo "    OK"
else
  echo "build-release.sh: smoke test failed" >&2
  cat "$SMOKE_DIR/out" >&2
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
