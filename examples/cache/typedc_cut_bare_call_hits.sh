#!/bin/sh
# A module whose body resolves a bare cross-module name is restorable
# from its typed blob.
#
# A bare call survives resolve as `ESym`, carrying the id the symbol
# table minted; a qualified call never becomes one. When the expression
# codec can serialise a node it cannot read back, the blob is published
# and then fails to decode, so every module holding a bare call
# re-infers on every build while the qualified spelling of the same
# program hits. The cost lands on the form everyone writes and grows
# with the module count.
#
# The gate is republication: a build that decodes its blobs leaves them
# untouched, one that fails to decode re-infers and rewrites every one.
# Blob names are keys, so a miss is invisible in the file list — the
# mtimes are what separate a hit from a miss.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the cache keys carry the
# edition, so this fixture must exercise the one the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

N=8
cat > "$PROJ/m0.kai" <<'EOF'
pub fn step0(n: Int) : Int = n + 1
EOF
i=1
while [ "$i" -lt "$N" ]; do
  p=$((i - 1))
  cat > "$PROJ/m$i.kai" <<EOF
import m$p
pub fn step${i}(n: Int) : Int = step${p}(n) + $i
EOF
  i=$((i + 1))
done

last=$((N - 1))
cat > "$PROJ/main.kai" <<EOF
import m$last
fn main() : Unit / Console = println(int_to_string(step${last}(0)))
EOF

mkdir -p "$PROJ/.kai-cache"

build() {
  out="$1"; shift
  if ! "$KAIC2" $EDITION_FLAG "$@" --path "$PROJ" "$PROJ/main.kai" > "$out" 2>"$PROJ/err"; then
    echo "typedc_cut_bare_call_hits: FAIL — build exited non-zero ($*)"
    cat "$PROJ/err"
    exit 1
  fi
}

blob_count() { ls "$PROJ/.kai-cache"/tm-*.kab 2>/dev/null | wc -l | tr -d ' '; }
blob_stamp() { ls -lT "$PROJ/.kai-cache"/tm-*.kab 2>/dev/null | awk '{print $6, $7, $8, $9, $NF}' | sort; }

same() {
  if ! cmp -s "$1" "$2"; then
    echo "typedc_cut_bare_call_hits: FAIL — $3"
    diff "$1" "$2" | head -20
    exit 1
  fi
}

build "$PROJ/plain.c"
build "$PROJ/cold.c" --user-cache

blobs="$(blob_count)"
if [ "$blobs" -lt "$N" ]; then
  echo "typedc_cut_bare_call_hits: FAIL — cut published $blobs blobs for $N modules (cut inactive?)"
  exit 1
fi

blob_stamp > "$PROJ/stamp_cold"
# The filesystem timestamp is the evidence, so the second build must be
# distinguishable in time from the first.
sleep 2
build "$PROJ/warm.c" --user-cache
blob_stamp > "$PROJ/stamp_warm"

same "$PROJ/plain.c" "$PROJ/cold.c" "cold --user-cache C differs from plain C"
same "$PROJ/plain.c" "$PROJ/warm.c" "warm --user-cache C differs from plain C"

if ! cmp -s "$PROJ/stamp_cold" "$PROJ/stamp_warm"; then
  echo "typedc_cut_bare_call_hits: FAIL — warm rebuild republished blobs with no edit; a bare call's module did not decode"
  diff "$PROJ/stamp_cold" "$PROJ/stamp_warm" | head -10
  exit 1
fi

# The bare name still answers the scope, not a stale id: shadowing it at
# home over a warm cache must re-target the call.
cat > "$PROJ/m$last.kai" <<EOF
import m$((last - 1))
fn step$((last - 1))(n: Int) : Int = 100
pub fn step${last}(n: Int) : Int = step$((last - 1))(n) + $last
EOF

build "$PROJ/shadow.c" --user-cache
build "$PROJ/shadow_plain.c"
same "$PROJ/shadow_plain.c" "$PROJ/shadow.c" "shadowed bare call served a stale resolution from cache"

echo "typedc_cut_bare_call_hits: OK — $N modules, warm rebuild republished nothing, shadowing re-targets"
exit 0
