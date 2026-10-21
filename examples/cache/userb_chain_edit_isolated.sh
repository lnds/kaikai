#!/bin/sh
# Import-CHAIN coverage for the user-file parse cache. A parse blob is a
# function of one file's own text, so an edit must re-parse that file and
# nothing else, however deep it sits in an import chain, while the build still
# comes out exactly as an uncached one (resolution, derive expansion and typing
# run after the lookup, over the cached decls).
#
# 1. A chained package emits C byte-identical with and without the cache, cold
#    and warm.
# 2. The blob keys of a rebuild in a fresh cache dir are the ones of the first
#    build.
# 3. A body edit of the DEEPEST module publishes exactly one new blob: its
#    importers keep theirs, and the C is still identical to a plain build.
# 4. An interface edit of the deepest module (a changed signature its importer
#    calls with the old arity) is reported by the cached build exactly as the
#    plain one reports it: the importers' cached parse is not a stale
#    interface.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the cache keys carry the
# edition, so this fixture must exercise the one the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

N=8

# m0 <- m1 <- ... <- m7, then main imports every module.
i=0
while [ "$i" -lt "$N" ]; do
  {
    if [ "$i" -gt 0 ]; then echo "import m$((i - 1))"; fi
    echo "pub fn m${i}_step(n: Int) : Int = n + $i"
  } > "$PROJ/m$i.kai"
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
    echo "userb_chain_edit_isolated: FAIL — build exited non-zero ($*)"
    cat "$PROJ/err"
    exit 1
  fi
}

same() {
  if ! cmp -s "$1" "$2"; then
    echo "userb_chain_edit_isolated: FAIL — $3"
    exit 1
  fi
}

# User blobs only: the core and typed-module caches key differently.
user_keys() {
  find "$PROJ/.kai-cache" -name '*.kab' -exec basename {} \; \
    | grep -v '^core-' | grep -v '^tm-' | sort
}

build "$PROJ/plain.c"
build "$PROJ/cold.c" --user-cache
build "$PROJ/warm.c" --user-cache

same "$PROJ/plain.c" "$PROJ/cold.c" "cold --user-cache C differs from plain C"
same "$PROJ/plain.c" "$PROJ/warm.c" "warm --user-cache C differs from plain C"

# Gate 2: a rebuild in a fresh cache dir writes the same blob names.
keys_run1="$(user_keys)"
rm -rf "$PROJ/.kai-cache"
mkdir -p "$PROJ/.kai-cache"
build "$PROJ/cold2.c" --user-cache
keys_run2="$(user_keys)"

if [ "$keys_run1" != "$keys_run2" ]; then
  echo "userb_chain_edit_isolated: FAIL — blob keys are not stable across rebuilds"
  echo "run1: $keys_run1"
  echo "run2: $keys_run2"
  exit 1
fi

# Gate 3: a body edit of the DEEPEST module (m0) writes one blob.
cat > "$PROJ/m0.kai" <<'EOF2'
pub fn m0_step(n: Int) : Int = n + 0 + 0
EOF2

echo "$keys_run2" > "$PROJ/keys_before"
build "$PROJ/edit.c" --user-cache
build "$PROJ/edit_plain.c"
same "$PROJ/edit_plain.c" "$PROJ/edit.c" "deep-edit --user-cache C differs from plain C"
user_keys > "$PROJ/keys_after"
fresh="$(comm -13 "$PROJ/keys_before" "$PROJ/keys_after" | wc -l | tr -d ' ')"
if [ "$fresh" != "1" ]; then
  echo "userb_chain_edit_isolated: FAIL — a body edit of m0 wrote $fresh blobs, expected 1"
  exit 1
fi

# Gate 4: an interface edit of m0. main still calls m0_step(1), so both
# builds must fail, and with the same report.
cat > "$PROJ/m0.kai" <<'EOF2'
pub fn m0_step(n: Int, k: Int) : Int = n + k
EOF2

if "$KAIC2" $EDITION_FLAG --user-cache --path "$PROJ" "$PROJ/main.kai" > /dev/null 2> "$PROJ/iface_cached.err"; then
  echo "userb_chain_edit_isolated: FAIL — the cached build accepted a call with the old arity"
  exit 1
fi
if "$KAIC2" $EDITION_FLAG --path "$PROJ" "$PROJ/main.kai" > /dev/null 2> "$PROJ/iface_plain.err"; then
  echo "userb_chain_edit_isolated: FAIL — the plain build accepted a call with the old arity"
  exit 1
fi
same "$PROJ/iface_plain.err" "$PROJ/iface_cached.err" "the cached build reports the interface edit differently from the plain one"

echo "userb_chain_edit_isolated: OK — $N-module chain: C identical, keys stable, a body edit wrote one blob, an interface edit reported as plain"
exit 0
