#!/bin/sh
# scripts/llvm-prebuilt.sh — the vendored static libLLVM as a release asset.
#
#   name          print the asset name for this host and configuration
#   tag           print the release tag the asset lives under
#   pack <dir>    archive a finished `make llvm-build` into <dir>
#   fetch         download, verify and unpack the asset
#
# fetch exits 0 with the archives in place, 3 when there is nothing to fetch
# (the caller builds from source), 1 when a downloaded asset is not what its
# checksum or its name says.
#
# Env: KAI_LLVM_PREBUILT=0 skips the download; KAI_LLVM_PREBUILT_REPO names
# the repository holding the release.

set -eu

SCRIPT="$0"
case "$SCRIPT" in
  /*) ;;
  *)  SCRIPT="$(pwd)/$SCRIPT" ;;
esac
ROOT="$(cd "$(dirname "$SCRIPT")/.." && pwd)"
cd "$ROOT"

LLVM_DIR=stage0/third_party/llvm
BUILD="$LLVM_DIR/build"
STAMP="$BUILD/.prebuilt"
REPO="${KAI_LLVM_PREBUILT_REPO:-lnds/kaikai}"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum; else shasum -a 256; fi | cut -d' ' -f1
}

VERSION="${LLVM_VERSION:-$(sed -n 's/^LLVM_VERSION ?= *//p' mk/llvm.mk)}"
# The two files that decide what the asset contains: how libLLVM is built
# and how it is packed. Editing either names a new asset.
CFG_HASH="$(cat mk/llvm.mk scripts/llvm-prebuilt.sh | sha256 | cut -c1-12)"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"
TAG="llvm-static-$VERSION"
NAME="libllvm-static-$VERSION-$CFG_HASH-$OS-$ARCH.tar.xz"

pack() {
  out="$1"
  if [ ! -x "$BUILD/bin/llvm-config" ] || [ ! -d "$LLVM_DIR/include/llvm-c" ]; then
    echo "llvm-prebuilt: no source build under $LLVM_DIR — run \`make llvm-build\` first" >&2
    exit 1
  fi
  mkdir -p "$out"
  stage="$(mktemp -d)"
  trap 'rm -rf "$stage"' EXIT
  mkdir -p "$stage/build/bin" "$stage/build/lib" "$stage/build/include/llvm"
  cp "$BUILD/bin/llvm-config" "$stage/build/bin/"
  cp "$BUILD"/lib/*.a "$stage/build/lib/"
  # llvm-config run from any path but the one it was built at answers as an
  # installed prefix, whose only include dir is build/include. The C API
  # headers go there, next to the generated llvm/Config.
  cp -R "$LLVM_DIR/include/llvm-c" "$stage/build/include/"
  cp -R "$BUILD/include/llvm/Config" "$stage/build/include/llvm/"
  printf '%s\n' "$NAME" > "$stage/build/.prebuilt"
  tar -cJf "$out/$NAME" -C "$stage" build
  printf '%s  %s\n' "$(sha256 < "$out/$NAME")" "$NAME" > "$out/$NAME.sha256"
  ls -l "$out/$NAME" "$out/$NAME.sha256"
}

fetch() {
  if [ "${KAI_LLVM_PREBUILT:-1}" = 0 ]; then
    echo "llvm-prebuilt: KAI_LLVM_PREBUILT=0, not fetching"
    return 3
  fi
  if [ -x "$BUILD/bin/llvm-config" ]; then
    if [ "$(cat "$STAMP" 2>/dev/null)" = "$NAME" ]; then
      echo "llvm-prebuilt: $NAME already in place"
      return 0
    fi
    if [ ! -f "$STAMP" ]; then
      echo "llvm-prebuilt: a source build is in place under $BUILD, keeping it"
      return 3
    fi
  fi
  url="https://github.com/$REPO/releases/download/$TAG/$NAME"
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  echo "llvm-prebuilt: fetching $url"
  if ! curl -fsSL --retry 3 -o "$tmp/$NAME" "$url" \
     || ! curl -fsSL --retry 3 -o "$tmp/$NAME.sha256" "$url.sha256"; then
    echo "llvm-prebuilt: $NAME is not published under $REPO $TAG"
    return 3
  fi
  want="$(cut -d' ' -f1 "$tmp/$NAME.sha256")"
  got="$(sha256 < "$tmp/$NAME")"
  if [ "$want" != "$got" ]; then
    echo "llvm-prebuilt: checksum mismatch for $NAME (want $want, got $got)" >&2
    return 1
  fi
  rm -rf "$BUILD"
  mkdir -p "$LLVM_DIR"
  tar -xJf "$tmp/$NAME" -C "$LLVM_DIR"
  have="$("$BUILD/bin/llvm-config" --version 2>/dev/null || true)"
  if [ "$have" != "$VERSION" ]; then
    echo "llvm-prebuilt: $NAME carries llvm-config '$have', expected $VERSION" >&2
    rm -rf "$BUILD"
    return 1
  fi
  echo "llvm-prebuilt OK — $NAME unpacked under $BUILD (sha256 $got)"
}

case "${1:-}" in
  name)  echo "$NAME" ;;
  tag)   echo "$TAG" ;;
  pack)  pack "${2:-dist}" ;;
  fetch) fetch ;;
  *)
    echo "usage: llvm-prebuilt.sh name | tag | pack [<dir>] | fetch" >&2
    exit 2
    ;;
esac
