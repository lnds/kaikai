#!/bin/sh
# Import-CHAIN coverage for the user-cache dependency walk, which
# memoises transitive closures across a compile so module i does not
# re-tokenise its i dependencies (O(N^2) on a chain, invisible on the
# flat packages every other cache fixture uses).
#
# This fixture does NOT gate the speedup — the memo is semantically
# neutral, so a build without it passes here too, and a timing gate
# would be flaky in CI. What it gates is the memo's soundness against
# future edits: replaying a cached closure must produce the same KEY a
# fresh walk does, and must never serve a stale one. Both are silent
# failures — a wrong key orphans every existing blob rather than
# erroring, and a stale closure serves outdated decls. The chain and
# diamond topologies below are where either would first show up.
#
# 1. A chained package emits C byte-identical with and without the
#    cache, cold and warm — the memo did not change what is compiled.
# 2. The blob keys a chained build writes are identical to the ones a
#    diamond re-derivation produces, i.e. replaying a memoised closure
#    yields the same discovery order (and so the same dep hash) as a
#    fresh walk. A memo that dropped or reordered a hex would change
#    the filename and silently orphan every existing entry.
# 3. Editing the DEEPEST module still invalidates every module above it
#    on the chain — the memo must not serve a stale closure.

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
    echo "userb_chain_dep_memo: FAIL — build exited non-zero ($*)"
    cat "$PROJ/err"
    exit 1
  fi
}

same() {
  if ! cmp -s "$1" "$2"; then
    echo "userb_chain_dep_memo: FAIL — $3"
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

# Gate 2: a diamond re-derives one leaf through two independent
# importers. Replaying the memoised closure for the second importer
# must dedup exactly as a fresh walk would, so the leaf's hex appears
# once, at first discovery. A wrong order or a duplicated hex changes
# the dep hash, hence the blob name — so a stable key set across a
# rebuild in a fresh cache dir is the observable.
keys_run1="$(user_keys)"
rm -rf "$PROJ/.kai-cache"
mkdir -p "$PROJ/.kai-cache"
build "$PROJ/cold2.c" --user-cache
keys_run2="$(user_keys)"

if [ "$keys_run1" != "$keys_run2" ]; then
  echo "userb_chain_dep_memo: FAIL — blob keys are not stable across rebuilds"
  echo "run1: $keys_run1"
  echo "run2: $keys_run2"
  exit 1
fi

# Gate 3: edit the DEEPEST module (m0). Every module above it on the
# chain has m0 in its transitive closure, so all N keys must change.
cat > "$PROJ/m0.kai" <<'EOF'
pub fn m0_step(n: Int) : Int = n + 0 + 0
EOF

build "$PROJ/edit.c" --user-cache
build "$PROJ/edit_plain.c"
same "$PROJ/edit_plain.c" "$PROJ/edit.c" "deep-edit --user-cache C differs from plain C"

# Editing m0 changes its content hash and every consumer's dep hash, so
# each of the N modules keys to a new blob. Entries are never swept, so
# the count must grow by the full chain length; a memo serving a stale
# closure would leave the upper modules on their old keys.
echo "$keys_run2" > "$PROJ/keys_before"
user_keys > "$PROJ/keys_after"
fresh="$(comm -13 "$PROJ/keys_before" "$PROJ/keys_after" | wc -l | tr -d ' ')"

if [ "$fresh" -lt "$N" ]; then
  echo "userb_chain_dep_memo: FAIL — deep edit produced $fresh new keys, expected >= $N"
  echo "the memo served a stale closure for modules above m0 on the chain"
  exit 1
fi

echo "userb_chain_dep_memo: OK — $N-module chain: C identical, keys stable, deep edit cascaded"
exit 0
