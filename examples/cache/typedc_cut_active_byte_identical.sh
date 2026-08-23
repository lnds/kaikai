#!/bin/sh
# The per-module typed cut, ACTIVE. Restoring a module's typed blob
# instead of re-inferring it must be invisible in the output: a
# `--user-cache` build must emit C byte-identical to a plain build,
# whether every module missed (cold) or every module hit (warm).
#
# Three properties, each a hard gate:
#
# 1. cold == plain and warm == plain. A restored `TypecheckedModule`
#    feeds whole-program monomorph exactly what a fresh infer would.
# 2. The cut actually cuts. A warm build over an edited body publishes
#    exactly one new blob — the edited module's — so the untouched
#    modules were served from cache, not silently re-inferred.
# 3. A signature change propagates. Editing an exported signature
#    invalidates every importer, and the result still matches plain.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the cache keys carry the
# edition, so this fixture must exercise the one the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/a.kai" <<'EOF'
pub fn shout(s: String) : Unit / Stdout = print(s)
EOF
cat > "$PROJ/b.kai" <<'EOF'
import a
pub fn twice(s: String) : Unit / Stdout = { a.shout(s) a.shout(s) }
EOF
cat > "$PROJ/main.kai" <<'EOF'
import a
import b
fn main() : Unit / Console { b.twice("hi") }
EOF

mkdir -p "$PROJ/.kai-cache"

build() {
  # $1: output file, $2..: extra flags
  out="$1"; shift
  if ! "$KAIC2" $EDITION_FLAG "$@" --path "$PROJ" "$PROJ/main.kai" > "$out" 2>"$PROJ/err"; then
    echo "typedc_cut_active_byte_identical: FAIL — build exited non-zero ($*)"
    cat "$PROJ/err"
    exit 1
  fi
  # Only stderr signals a compiler-runtime panic; the emitted C
  # legitimately contains `kai_core_panic("non-exhaustive match")` as a
  # match default.
  if grep -q panic "$PROJ/err" 2>/dev/null; then
    echo "typedc_cut_active_byte_identical: FAIL — build panicked ($*)"
    cat "$PROJ/err"
    exit 1
  fi
}

blob_count() { ls "$PROJ/.kai-cache"/tm-*.kab 2>/dev/null | wc -l | tr -d ' '; }

same() {
  if ! cmp -s "$1" "$2"; then
    echo "typedc_cut_active_byte_identical: FAIL — $3"
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
if [ "$cold_blobs" -lt 2 ]; then
  echo "typedc_cut_active_byte_identical: FAIL — cut published $cold_blobs typed blobs (cut inactive?)"
  exit 1
fi

# A body-only edit re-infers exactly its own module: one new blob.
cat > "$PROJ/b.kai" <<'EOF'
import a
pub fn twice(s: String) : Unit / Stdout = { a.shout(s) a.shout(s) a.shout(s) }
EOF
build "$PROJ/edit.c" --user-cache
build "$PROJ/edit_plain.c"
same "$PROJ/edit_plain.c" "$PROJ/edit.c" "body-edit --user-cache C differs from plain C"

edit_blobs="$(blob_count)"
if [ "$edit_blobs" -ne $((cold_blobs + 1)) ]; then
  echo "typedc_cut_active_byte_identical: FAIL — body edit published $((edit_blobs - cold_blobs)) blobs, expected 1"
  exit 1
fi

# A signature change must reach the importers: the emitted C still
# matches plain, which it cannot if a stale interface was served.
cat > "$PROJ/a.kai" <<'EOF'
pub fn shout(s: String, n: Int) : Unit / Stdout = { print(s) print(int_to_string(n)) }
EOF
cat > "$PROJ/b.kai" <<'EOF'
import a
pub fn twice(s: String) : Unit / Stdout = { a.shout(s, 1) a.shout(s, 2) }
EOF
build "$PROJ/sig.c" --user-cache
build "$PROJ/sig_plain.c"
same "$PROJ/sig_plain.c" "$PROJ/sig.c" "signature-change --user-cache C differs from plain C"

echo "typedc_cut_active_byte_identical: OK — cut active, cold/warm/edit == plain, one blob per edited module"
exit 0
