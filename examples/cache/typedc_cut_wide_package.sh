#!/bin/sh
# The per-module typed cut on a package wide enough that the inherited
# interface is no longer trivial.
#
# Each module keys on every export folded before it, so a cut that
# rebuilds that hash per module pays for the whole accumulation once per
# module — quadratic in the module count, and invisible on the two- and
# three-module fixtures. This one is wide enough that a regression back
# to a per-module rebuild of the accumulated interface shows up as wall
# time rather than as a wrong answer.
#
# The gates are still correctness, not timing (a timing gate would be
# flaky in CI):
#
# 1. Cold and warm both emit C byte-identical to a plain build across
#    the full width.
# 2. A body-only edit deep in the package republishes exactly one blob,
#    so the width did not cost hit granularity.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the cache keys carry the
# edition, so this fixture must exercise the one the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

N=10
i=0
while [ "$i" -lt "$N" ]; do
  cat > "$PROJ/m$i.kai" <<EOF
pub type M${i}Rec = { a: Int, b: String }
pub fn m${i}_mk(n: Int) : M${i}Rec = M${i}Rec { a: n, b: "m$i" }
pub fn m${i}_use(r: M${i}Rec) : Int = r.a + string_length(r.b)
pub fn m${i}_step(n: Int) : Int = m${i}_use(m${i}_mk(n)) + $i
EOF
  i=$((i + 1))
done

{
  i=0
  while [ "$i" -lt "$N" ]; do echo "import m$i"; i=$((i + 1)); done
  printf 'fn main() : Unit / Console {\n  let t = 0'
  i=0
  while [ "$i" -lt "$N" ]; do printf ' + m%s.m%s_step(1)' "$i" "$i"; i=$((i + 1)); done
  printf '\n  println(int_to_string(t))\n}\n'
} > "$PROJ/main.kai"

mkdir -p "$PROJ/.kai-cache"

build() {
  out="$1"; shift
  if ! "$KAIC2" $EDITION_FLAG "$@" --path "$PROJ" "$PROJ/main.kai" > "$out" 2>"$PROJ/err"; then
    echo "typedc_cut_wide_package: FAIL — build exited non-zero ($*)"
    cat "$PROJ/err"
    exit 1
  fi
}

blob_count() { ls "$PROJ/.kai-cache"/tm-*.kab 2>/dev/null | wc -l | tr -d ' '; }

same() {
  if ! cmp -s "$1" "$2"; then
    echo "typedc_cut_wide_package: FAIL — $3"
    diff "$1" "$2" | head -20
    exit 1
  fi
}

build "$PROJ/plain.c"
build "$PROJ/cold.c" --user-cache
build "$PROJ/warm.c" --user-cache

same "$PROJ/plain.c" "$PROJ/cold.c" "cold --user-cache C differs from plain C"
same "$PROJ/plain.c" "$PROJ/warm.c" "warm --user-cache C differs from plain C"

cold_blobs="$(blob_count)"
if [ "$cold_blobs" -lt "$N" ]; then
  echo "typedc_cut_wide_package: FAIL — cut published $cold_blobs blobs for $N modules (cut inactive?)"
  exit 1
fi

# A body edit in the middle of the package: one module re-infers, the
# rest are served from cache even though each carries a wide inherited
# interface.
cat > "$PROJ/m5.kai" <<'EOF'
pub type M5Rec = { a: Int, b: String }
pub fn m5_mk(n: Int) : M5Rec = M5Rec { a: n, b: "m5" }
pub fn m5_use(r: M5Rec) : Int = r.a + string_length(r.b)
pub fn m5_step(n: Int) : Int = m5_use(m5_mk(n)) + 5 + 0
EOF

build "$PROJ/edit.c" --user-cache
build "$PROJ/edit_plain.c"
same "$PROJ/edit_plain.c" "$PROJ/edit.c" "body-edit --user-cache C differs from plain C"

edit_blobs="$(blob_count)"
if [ "$edit_blobs" -ne $((cold_blobs + 1)) ]; then
  echo "typedc_cut_wide_package: FAIL — body edit published $((edit_blobs - cold_blobs)) blobs, expected 1"
  exit 1
fi

echo "typedc_cut_wide_package: OK — $N modules, cold/warm/edit == plain, one blob per edited module"
exit 0
