#!/bin/sh
# The parse cache keys a module by its own text, so what else a parse reads
# must be in that key or be shown not to matter:
#
# 1. The HOME (module path) is part of the key. Two modules with byte-identical
#    text under different homes must each come back with their own home, cold
#    and warm: the KIR names the functions of each after its own module.
# 2. The EDITION is not an input of the parser, but a change of edition in a
#    project that already holds blobs must still build exactly as a plain
#    build does, so this flips it over a warm cache and compares.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

mkdir -p "$PROJ/a" "$PROJ/b" "$PROJ/.kai-cache"
for m in a b; do
  cat > "$PROJ/$m/util.kai" <<'EOF'
pub fn twice(n: Int) : Int = n + n
EOF
done
cat > "$PROJ/main.kai" <<'EOF'
import a.util as au
import b.util as bu

fn main() : Unit / Console {
  println(int_to_string(au.twice(2) + bu.twice(3)))
}
EOF

EDITION="$(cat "$ROOT/EDITION")"

build() {
  out="$1"; ed="$2"; shift 2
  if ! "$KAIC2" --edition "$ed" "$@" --path "$PROJ" "$PROJ/main.kai" > "$out" 2>"$PROJ/err"; then
    echo "userb_home_and_edition: FAIL — build exited non-zero (--edition $ed $*)"
    cat "$PROJ/err"
    exit 1
  fi
}

same() {
  cmp -s "$1" "$2" || { echo "userb_home_and_edition: FAIL — $3"; exit 1; }
}

build "$PROJ/plain.c" "$EDITION"
build "$PROJ/cold.c" "$EDITION" --user-cache --user-cache-dir "$PROJ/.kai-cache"
build "$PROJ/warm.c" "$EDITION" --user-cache --user-cache-dir "$PROJ/.kai-cache"
same "$PROJ/plain.c" "$PROJ/cold.c" "cold C differs from plain C"
same "$PROJ/plain.c" "$PROJ/warm.c" "warm C differs from plain C"

"$KAIC2" --edition "$EDITION" --emit=kir-noinline --user-cache --user-cache-dir "$PROJ/.kai-cache" --path "$PROJ" "$PROJ/main.kai" > "$PROJ/warm.kir" 2>"$PROJ/err" || {
  echo "userb_home_and_edition: FAIL — the KIR dump over the warm cache exited non-zero"
  cat "$PROJ/err"
  exit 1
}
grep -q '^fn kaiu_a__util__twice' "$PROJ/warm.kir" && grep -q '^fn kaiu_b__util__twice' "$PROJ/warm.kir" || {
  echo "userb_home_and_edition: FAIL — a byte-identical module lost its own home (expected a__util__twice and b__util__twice)"
  exit 1
}

# The two util.kai files have the same text: one blob per home, not one shared
# (the root file is not cached).
blobs="$(find "$PROJ/.kai-cache" -name '*.kab' ! -name 'core-*' ! -name 'tm-*' | wc -l | tr -d ' ')"
[ "$blobs" = "2" ] || {
  echo "userb_home_and_edition: FAIL — expected one blob per imported module (a.util, b.util), found $blobs"
  exit 1
}

# An edition flip over the warm cache must build as a plain build of that edition.
for ed in tongariki hanga-roa orongo; do
  if "$KAIC2" --edition "$ed" --path "$PROJ" "$PROJ/main.kai" > "$PROJ/plain_$ed.c" 2>"$PROJ/plain_$ed.err"; then
    build "$PROJ/cached_$ed.c" "$ed" --user-cache --user-cache-dir "$PROJ/.kai-cache"
    same "$PROJ/plain_$ed.c" "$PROJ/cached_$ed.c" "--edition $ed over a warm cache differs from a plain build"
  else
    if "$KAIC2" --edition "$ed" --user-cache --user-cache-dir "$PROJ/.kai-cache" --path "$PROJ" "$PROJ/main.kai" > /dev/null 2>"$PROJ/cached_$ed.err"; then
      echo "userb_home_and_edition: FAIL — --edition $ed fails plain but builds over the cache"
      exit 1
    fi
    cmp -s "$PROJ/plain_$ed.err" "$PROJ/cached_$ed.err" || {
      echo "userb_home_and_edition: FAIL — --edition $ed reports differently over the cache"
      exit 1
    }
  fi
done

echo "userb_home_and_edition: OK — identical text under two homes keeps each home, and an edition flip over a warm cache builds as plain"
exit 0
