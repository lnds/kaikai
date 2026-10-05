#!/bin/sh
# The cached native core object versus the build mode. A debug build emits
# DWARF naming the program's own source file, so its core object is never
# shared: a debug build must not link a release core object (it would lose
# the core's debug info), and must not leave one for a release build to
# link (it would carry DWARF it never asked for).

set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM
TID="fixture-$(cksum < "$KAIC2" | cut -d' ' -f1)"

fail() { echo "ncoreobj_build_mode: FAIL — $1"; exit 1; }

printf 'fn main() : Unit / Stdout = Stdout.print("n=#{[3, 1, 2] |> list.reverse |> list.length}")\n' > "$WORK/main.kai"
if ! "$KAIC2" $EDITION_FLAG --emit=native "$WORK/main.kai" > /dev/null 2>&1; then
  echo "ncoreobj_build_mode: SKIP — native backend unavailable"; exit 0
fi

# $1: cache dir, $2: build mode, $3: objects list out
build() {
  (cd "$WORK" && KAI_BUILD_MODE="$2" "$KAIC2" $EDITION_FLAG --emit=native \
     --core-cache-dir "$1" --toolchain-id "$TID" main.kai > "$3" 2> "$3.err") \
    || { cat "$3.err"; fail "$2 build failed"; }
}

has_dwarf() { (cd "$WORK" && objdump -h "$1" 2>/dev/null | grep -q "debug_info"); }
core_obj() { grep "/ncore-" "$1" || true; }

# Release then debug in one cache dir.
mkdir -p "$WORK/c1"
build "$WORK/c1" default "$WORK/r1"
rel="$(core_obj "$WORK/r1")"
[ -n "$rel" ] || fail "a release build cached no core object"
has_dwarf "$rel" && fail "the release core object carries DWARF"
build "$WORK/c1" debug "$WORK/d1"
[ -z "$(core_obj "$WORK/d1")" ] || fail "a debug build linked the cached release core object"
has_dwarf "$(head -1 "$WORK/d1")" || fail "the debug build's object carries no DWARF"

# Debug then release in a fresh cache dir.
mkdir -p "$WORK/c2"
build "$WORK/c2" debug "$WORK/d2"
[ -z "$(ls "$WORK/c2"/*/ncore-*.o "$WORK/c2"/ncore-*.o 2>/dev/null)" ] || fail "a debug build cached a core object"
build "$WORK/c2" default "$WORK/r2"
rel2="$(core_obj "$WORK/r2")"
[ -n "$rel2" ] || fail "a release build after a debug build cached no core object"
has_dwarf "$rel2" && fail "the release core object written after a debug build carries DWARF"

echo "ncoreobj_build_mode OK — debug builds neither link nor leave a shared core object"
