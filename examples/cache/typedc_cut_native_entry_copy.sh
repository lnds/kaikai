#!/bin/sh
# The typed rebuild cut must reach the native build path.
#
# The native backends compile a *copy* of the entry file from an
# ephemeral object-cache directory, so anything the driver derives from
# the entry's own directory lands in scratch. A publish failure is
# swallowed by design (a cache that cannot be written is a performance
# loss, not a correctness one), so this regresses silently: the build
# stays correct and simply stops caching. `--user-cache-dir` pins the
# blobs to the project regardless of where the entry is compiled from.
#
# Gates, on the copied-entry shape the wrapper actually uses:
#
# 1. The cut publishes into the project cache, not beside the entry copy.
# 2. Cold and warm emit byte-identical output (#1298 holds through the
#    restored artifacts).

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the cache keys carry the
# edition, so this fixture must exercise the one the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
ENTRYDIR="$(mktemp -d)"
trap 'rm -rf "$PROJ" "$ENTRYDIR"' EXIT INT TERM

N=6
i=0
while [ "$i" -lt "$N" ]; do
  cat > "$PROJ/m$i.kai" <<EOF
pub type M${i}Rec = { a: Int, b: String }
pub fn m${i}_mk(n: Int) : M${i}Rec = M${i}Rec { a: n, b: "m$i" }
pub fn m${i}_step(n: Int) : Int = m${i}_mk(n).a + string_length(m${i}_mk(n).b) + $i
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
# The wrapper's shape: the entry is compiled from a directory that is
# not the project, with the sibling modules reached via --path.
cp "$PROJ/main.kai" "$ENTRYDIR/main.kai"

blob_count() { find "$PROJ/.kai-cache" -maxdepth 1 -name 'tm-*.kab' 2>/dev/null | wc -l | tr -d ' '; }

build() {
  out="$1"; shift
  if ! "$KAIC2" $EDITION_FLAG "$@" --path "$PROJ" "$ENTRYDIR/main.kai" > "$out" 2>"$PROJ/err"; then
    echo "typedc_cut_native_entry_copy: FAIL — build exited non-zero ($*)"
    cat "$PROJ/err"
    exit 1
  fi
}

build "$PROJ/plain.c"
build "$PROJ/cold.c" --user-cache --user-cache-dir "$PROJ/.kai-cache"

cold_blobs="$(blob_count)"
if [ "$cold_blobs" -lt "$N" ]; then
  echo "typedc_cut_native_entry_copy: FAIL — cut published $cold_blobs blobs for $N modules;"
  echo "  the copied entry means the cut is publishing where nothing reads it."
  exit 1
fi

# Nothing may be written beside the entry copy — that is the scratch dir
# whose contents the next build never names.
if [ -d "$ENTRYDIR/.kai-cache" ]; then
  echo "typedc_cut_native_entry_copy: FAIL — blobs published beside the entry copy"
  exit 1
fi

build "$PROJ/warm.c" --user-cache --user-cache-dir "$PROJ/.kai-cache"

if ! cmp -s "$PROJ/plain.c" "$PROJ/cold.c"; then
  echo "typedc_cut_native_entry_copy: FAIL — cold C differs from plain C"
  exit 1
fi
if ! cmp -s "$PROJ/plain.c" "$PROJ/warm.c"; then
  echo "typedc_cut_native_entry_copy: FAIL — warm C differs from plain C"
  exit 1
fi

echo "typedc_cut_native_entry_copy: OK — $cold_blobs blobs in the project cache, cold/warm == plain"
