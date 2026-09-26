#!/bin/sh
# tools/kaic-boot.sh — the boot compiler that turns the stage 2 package
# into C, and the identity record that decides when that C is reusable.
#
#   kaic-boot.sh emit  <out.c>         compile stage2/main.kai into <out.c>
#   kaic-boot.sh fresh <out.c>         exit 0 iff <out.c> may be reused
#   kaic-boot.sh seal  <out.c> <bin>   record the kaic2 linked from <out.c>
#   kaic-boot.sh release-id            print the release boot's name and sha256
#
# The boot comes from $KAIC_BOOT (docs/build-system.md §KAIC_BOOT):
#   release (or unset)  the kaic2 of the newest published release up to VERSION
#   seed                the newest bootstrap-seed-v* tag up to VERSION, built by cc
#   kaic1               the stage0 -> stage1 chain
#   auto                a kaic2 this tree sealed, else release, else seed, else kaic1
#   <path>              any kaic1- or kaic2-class binary
#
# A kaic2-class boot takes two hops: the boot emits <dir>/stage2-a.c, linked
# into <dir>/kaic2-a, and kaic2-a emits <out.c>, so the C that is linked is
# this tree's codegen. A kaic1-class boot takes one.
#
# <out.c>.id records the boot, the hop count and the content hash of every
# input the boot read. A C file is reused only on an exact match — mtimes
# decide nothing. The hop-a files are scratch, never recorded or reused.

set -eu

ROOT="${KAIC_BOOT_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
STAGE2="$ROOT/stage2"
BOOT_DIR="$STAGE2/build/boot"
RELEASE_URL="${KAIC_BOOT_URL:-https://github.com/kaikailang-org/kaikai/releases/download}"
LATEST_URL="${KAIC_BOOT_LATEST_URL:-https://github.com/kaikailang-org/kaikai/releases/latest/download/latest.json}"
MODE="${KAIC_BOOT:-release}"
LC_ALL=C
export LC_ALL

die() { echo "kaic-boot: error: $*" >&2; exit 2; }
say() { echo "kaic-boot: $*" >&2; }

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}
sha_of() { sha256 "$1" | awk '{print $1}'; }

# Field $1 of record file $2, empty when absent.
field() { sed -n "s/^$1=//p" "$2" 2>/dev/null | head -n 1; }

# version_le <a> <b>: dotted version a is at most b.
version_le() {
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | head -n 1)" = "$1" ]
}

# Must match the tarball names scripts/build-release.sh publishes
# (asserted by tools/test-release-platforms.sh).
boot_platform() {
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64|Darwin-aarch64) echo darwin-arm64 ;;
    Linux-x86_64)                echo linux-x86_64 ;;
    *) return 1 ;;
  esac
}

# The stdlib core modules a kaic2 auto-loads; their effect declarations are
# emitted into the C, so they are inputs of a kaic2-class boot.
core_files() {
  files="$(sed -n '/^fn core_module_files()/,/^]/s/^ *"\([^"]*\.kai\)",*$/\1/p' \
    "$STAGE2/compiler/driver.kai")"
  [ -n "$files" ] || die "core module set not found in stage2/compiler/driver.kai"
  echo "$files"
}

edition() { cat "$ROOT/EDITION"; }

# Content hash of everything a boot of class $1 reads.
src_key() {
  core=""
  [ "$1" != kaic2 ] || core="$(core_files)"
  {
    (cd "$STAGE2" && sha256 main.kai compiler/*.kai)
    if [ -n "$core" ]; then
      echo "edition $(edition)"
      (cd "$ROOT/stdlib" && sha256 $core)
    fi
  } | sha256 | awk '{print $1}'
}

# ---- boot resolution: each sets BOOT_BIN, BOOT_CLASS and BOOT_ID -------

boot_kaic1() {
  "${MAKE:-make}" -s -C "$ROOT/stage1" kaic1 >&2
  BOOT_BIN="$ROOT/stage1/kaic1"
  BOOT_CLASS=kaic1
  BOOT_ID="kaic1 $(sha_of "$BOOT_BIN")"
}

# A kaic2 is a self boot only when its seal proves this tree linked it: a
# binary copied in from another checkout bakes that checkout's stdlib path.
boot_self() {
  bin="$STAGE2/kaic2"
  seal="$STAGE2/build/kaic2.id"
  [ -x "$bin" ] && [ -f "$seal" ] || return 1
  sum="$(sha_of "$bin")"
  [ "$(field bin "$seal")" = "$sum" ] || return 1
  BOOT_BIN="$bin"
  BOOT_CLASS=kaic2
  BOOT_ID="self $sum"
}

# A release boot is fetched once per version into build/boot/ and verified
# against the release's published sha256. The cached copy is reused only
# while its kaic2 still hashes to what was verified at fetch time.
release_cached() {
  [ -f "$1/boot.id" ] \
    && [ "$(field kaic2 "$1/boot.id")" = "$(sha_of "$1/libexec/kaikai/kaic2" 2>/dev/null)" ]
}

# release_fetch <name> <url> <dir>: 1 when unreachable, fatal when corrupt.
release_fetch() {
  command -v curl >/dev/null 2>&1 || { say "curl not found"; return 1; }
  mkdir -p "$BOOT_DIR"
  tmp="$(mktemp -d "$BOOT_DIR/.fetch.XXXXXX")"
  say "fetching $2"
  if ! curl -fsSL --retry 2 -o "$tmp/t.tar.gz" "$2" \
     || ! curl -fsSL --retry 2 -o "$tmp/t.sha256" "$2.sha256"; then
    rm -rf "$tmp"; say "download failed: $2"; return 1
  fi
  want="$(awk 'NR==1{print $1}' "$tmp/t.sha256")"
  got="$(sha_of "$tmp/t.tar.gz")"
  [ "$want" = "$got" ] || { rm -rf "$tmp"; die "$1.tar.gz sha256 $got, release publishes $want"; }
  tar -xzf "$tmp/t.tar.gz" -C "$tmp"
  [ -x "$tmp/$1/libexec/kaikai/kaic2" ] || { rm -rf "$tmp"; die "$1 has no libexec/kaikai/kaic2"; }
  printf 'tarball=%s\nkaic2=%s\n' "$got" "$(sha_of "$tmp/$1/libexec/kaikai/kaic2")" > "$tmp/$1/boot.id"
  rm -rf "$3"
  mv "$tmp/$1" "$3"
  rm -rf "$tmp"
}

# release_sha <version> <platform>: the published tarball sha256, empty when unpublished.
release_sha() {
  command -v curl >/dev/null 2>&1 || return 0
  curl -fsSL --retry 2 "$RELEASE_URL/v$1/kaikai-v$1-$2.tar.gz.sha256" 2>/dev/null | awk 'NR==1{print $1}'
}

latest_version() {
  command -v curl >/dev/null 2>&1 || return 0
  curl -fsSL --retry 2 "$LATEST_URL" 2>/dev/null \
    | tr -d ' \n\r\t' | sed -n 's/.*"version":"\([0-9][0-9.]*\)".*/\1/p'
}

# The boot is a published release; VERSION names one only once it is published.
release_version() {
  want="$(cat "$ROOT/VERSION")"
  if release_cached "$BOOT_DIR/kaikai-v$want-$1" || [ -n "$(release_sha "$want" "$1")" ]; then
    echo "$want"; return
  fi
  latest="$(latest_version)"
  [ -n "$latest" ] || { say "v$want is unpublished and no release manifest is reachable"; return 1; }
  version_le "$latest" "$want" \
    || { say "v$want is unpublished and the newest release, v$latest, follows it"; return 1; }
  say "v$want is unpublished; the newest published release is v$latest"
  echo "$latest"
}

boot_release() {
  plat="$(boot_platform)" || { say "no release tarball for $(uname -s)-$(uname -m)"; return 1; }
  ver="$(release_version "$plat")" || return 1
  name="kaikai-v$ver-$plat"
  dir="$BOOT_DIR/$name"
  release_cached "$dir" \
    || release_fetch "$name" "$RELEASE_URL/v$ver/$name.tar.gz" "$dir" \
    || return 1
  BOOT_BIN="$dir/libexec/kaikai/kaic2"
  BOOT_CLASS=kaic2
  BOOT_ID="release $name $(field tarball "$dir/boot.id")"
}

# The seed is C, so cc alone builds it, against the runtime.h it was frozen
# with. The linked seed is cached per tag and reused only while the tag still
# names the commit it was built from and the binary still hashes as recorded.
seed_version() {
  want="$(cat "$ROOT/VERSION")"
  git -C "$ROOT" tag -l 'bootstrap-seed-v*' 2>/dev/null | sed 's/^bootstrap-seed-v//' \
    | while read -r v; do version_le "$v" "$want" && echo "$v"; done \
    | sort -t. -k1,1n -k2,2n -k3,3n | tail -n 1
}

seed_cached() {
  [ -f "$1/boot.id" ] && [ "$(field commit "$1/boot.id")" = "$2" ] \
    && [ "$(field kaic2 "$1/boot.id")" = "$(sha_of "$1/kaic2" 2>/dev/null)" ]
}

boot_seed() {
  command -v git >/dev/null 2>&1 || { say "git not found"; return 1; }
  ver="$(seed_version)"
  [ -n "$ver" ] || { say "no bootstrap-seed-v* tag up to v$(cat "$ROOT/VERSION"); fetch them with" \
    "git fetch origin 'refs/tags/bootstrap-seed-v*:refs/tags/bootstrap-seed-v*'"; return 1; }
  tag="bootstrap-seed-v$ver"
  dir="$BOOT_DIR/$tag"
  commit="$(git -C "$ROOT" rev-parse "$tag^{commit}")"
  if ! seed_cached "$dir" "$commit"; then
    rm -rf "$dir"
    mkdir -p "$dir"
    git -C "$ROOT" archive "$commit" bootstrap | tar -x -C "$dir"
    say "cc $tag -> $dir/kaic2"
    "${MAKE:-make}" -s -C "$STAGE2" boot-hop HOP_C="$dir/bootstrap/stage2.c" HOP_BIN="$dir/kaic2" \
      HOP_INC="$dir/bootstrap" >&2
    printf 'commit=%s\nkaic2=%s\n' "$commit" "$(sha_of "$dir/kaic2")" > "$dir/boot.id"
  fi
  BOOT_BIN="$dir/kaic2"
  BOOT_CLASS=kaic2
  BOOT_ID="seed $tag $commit"
}

boot_path() {
  [ -x "$1" ] || die "KAIC_BOOT=$1 is not an executable file"
  BOOT_BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
  if "$BOOT_BIN" --version 2>/dev/null | grep -q "self-hosted"; then
    BOOT_CLASS=kaic2
  else
    BOOT_CLASS=kaic1
  fi
  BOOT_ID="path $(sha_of "$BOOT_BIN")"
}

resolve() {
  case "$MODE" in
    kaic1)   boot_kaic1 ;;
    release) boot_release || die "KAIC_BOOT=release: no release boot available (KAIC_BOOT=seed needs only cc)" ;;
    seed)    boot_seed || die "KAIC_BOOT=seed: no seed available" ;;
    auto)    boot_self || boot_release || boot_seed || { say "falling back to the kaic1 chain"; boot_kaic1; } ;;
    *)       boot_path "$MODE" ;;
  esac
}

# ---- commands ----------------------------------------------------------

# A kaic2 boot reads this tree's stdlib, never its own: the emitted C is
# compiled against this tree's runtime.h, which pairs with this stdlib.
run_boot() {
  if [ "$BOOT_CLASS" = kaic2 ]; then
    (cd "$STAGE2" && KAIKAI_STDLIB_PATH="$ROOT/stdlib" "$BOOT_BIN" --edition "$(edition)" main.kai)
  else
    (cd "$STAGE2" && "$BOOT_BIN" main.kai)
  fi
}

hops() { if [ "$1" = kaic2 ]; then echo 2; else echo 1; fi; }

# emit_to <out.c>: run BOOT_BIN, replacing <out.c> only on success.
emit_to() {
  run_boot > "$1.tmp" || { rm -f "$1.tmp"; exit 1; }
  mv "$1.tmp" "$1"
}

# Sound only while kaic2-a, built by the boot's codegen, still emits correct C.
hop_a() {
  dir="$(cd "$(dirname "$1")" && pwd)"
  say "boot $BOOT_ID -> $dir/stage2-a.c"
  emit_to "$dir/stage2-a.c"
  say "cc $dir/stage2-a.c -> $dir/kaic2-a"
  "${MAKE:-make}" -s -C "$STAGE2" boot-hop HOP_C="$dir/stage2-a.c" HOP_BIN="$dir/kaic2-a" >&2
  BOOT_BIN="$dir/kaic2-a"
  say "kaic2-a -> $1"
}

# The input key is taken before the boot runs: an edit landing mid-compile
# must leave the record describing what the boot actually read, or stale.
cmd_emit() {
  out="$1"
  resolve
  mkdir -p "$(dirname "$out")"
  rm -f "$out.id"
  key="$(src_key "$BOOT_CLASS")"
  if [ "$BOOT_CLASS" = kaic2 ]; then hop_a "$out"; else say "boot $BOOT_ID -> $out"; fi
  emit_to "$out"
  printf 'boot=%s\nclass=%s\nhops=%s\nsrc=%s\nout=%s\n' \
    "$BOOT_ID" "$BOOT_CLASS" "$(hops "$BOOT_CLASS")" "$key" "$(sha_of "$out")" > "$out.id.tmp"
  mv "$out.id.tmp" "$out.id"
}

# The record's C and inputs are byte-for-byte what is on disk now, and the
# C took the hops its boot class takes.
inputs_match() {
  [ -f "$1" ] && [ "$(field out "$2")" = "$(sha_of "$1")" ] || return 1
  class="$(field class "$2")"
  [ "$(field hops "$2")" = "$(hops "$class")" ] || return 1
  key="$(src_key "$class")"
  [ "$(field src "$2")" = "$key" ]
}

# The recorded boot is the one $MODE selects; auto accepts any. Freshness
# never reaches the network, so release and seed accept any up to VERSION.
boot_matches() {
  case "$MODE" in
    auto)    return 0 ;;
    kaic1)   kaic1="$ROOT/stage1/kaic1"
             [ -x "$kaic1" ] && want="kaic1 $(sha_of "$kaic1")" || want="kaic1 "
             case "$1" in "$want"*) return 0 ;; esac ;;
    release) plat="$(boot_platform)" || return 1
             ver="${1#release kaikai-v}"; ver="${ver%%-*}"
             case "$1" in "release kaikai-v$ver-$plat "*)
               version_le "$ver" "$(cat "$ROOT/VERSION")" && return 0 ;; esac ;;
    seed)    ver="${1#seed bootstrap-seed-v}"; ver="${ver%% *}"
             case "$1" in "seed bootstrap-seed-v$ver "*)
               version_le "$ver" "$(cat "$ROOT/VERSION")" && return 0 ;; esac ;;
    *)       [ -x "$MODE" ] && [ "$1" = "path $(sha_of "$MODE")" ] && return 0 ;;
  esac
  return 1
}

# Without a record only auto defers to make's mtime rule (a tree built before
# records existed, or a CI artifact restored by exact cache key); every other
# mode requires an exact match.
cmd_fresh() {
  rec="$1.id"
  if [ ! -f "$rec" ]; then
    [ "$MODE" = auto ]
    return
  fi
  inputs_match "$1" "$rec" && boot_matches "$(field boot "$rec")"
}

cmd_seal() {
  out="$1"; bin="$2"
  seal="$(dirname "$out")/$(basename "$bin").id"
  if [ -f "$out.id" ] && [ "$(field out "$out.id")" = "$(sha_of "$out")" ]; then
    { cat "$out.id"; echo "bin=$(sha_of "$bin")"; } > "$seal"
  else
    rm -f "$seal"
  fi
}

# Identifies the release boot without downloading it, for a cache key.
cmd_release_id() {
  plat="$(boot_platform)" || die "no release tarball for $(uname -s)-$(uname -m)"
  ver="$(release_version "$plat")" || die "no release boot available"
  name="kaikai-v$ver-$plat"
  if release_cached "$BOOT_DIR/$name"; then
    sha="$(field tarball "$BOOT_DIR/$name/boot.id")"
  else
    sha="$(release_sha "$ver" "$plat")"
  fi
  [ -n "$sha" ] || die "$name has no published sha256"
  echo "$name $sha"
}

case "${1:-}" in
  emit)  [ $# -eq 2 ] || die "usage: kaic-boot.sh emit <out.c>";        cmd_emit "$2" ;;
  fresh) [ $# -eq 2 ] || die "usage: kaic-boot.sh fresh <out.c>";       cmd_fresh "$2" ;;
  seal)  [ $# -eq 3 ] || die "usage: kaic-boot.sh seal <out.c> <bin>";  cmd_seal "$2" "$3" ;;
  release-id) [ $# -eq 1 ] || die "usage: kaic-boot.sh release-id";     cmd_release_id ;;
  *)     die "usage: kaic-boot.sh emit|fresh|seal|release-id ..." ;;
esac
