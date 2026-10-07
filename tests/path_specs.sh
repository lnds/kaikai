#!/bin/sh
# Every verb that takes a path decides file vs package by what the path is
# on disk, not by its spelling: `./one.kai` and `./sub/two.kai` are files
# like `one.kai` and `sub/two.kai`, and a directory is a package however it
# is spelled.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM
cd "$work"

src='fn main() : Unit / Stdout = println("hi")

test "holds" { assert 1 == 1 }
'
mkdir -p sub pk
printf '%s' "$src" > one.kai
"$KAI" fmt one.kai
cp one.kai sub/two.kai
cp one.kai pk/main.kai
printf 'name = "pk"\nversion = "0.1.0"\nedition = "%s"\n' "$(cat "$ROOT/EDITION")" > pk/kai.toml

fail=0
expect() {
  want="$1"; shift
  rc=0; "$KAI" "$@" > out 2> err || rc=$?
  if [ "$rc" -ne "$want" ]; then
    echo "  FAIL kai $* — rc=$rc (want $want)"
    sed 's/^/      /' err
    fail=$((fail + 1))
  fi
}

for p in one.kai ./one.kai sub/two.kai ./sub/two.kai; do
  for v in build check typecheck test lint; do
    expect 0 "$v" "$p"
  done
  expect 0 fmt --check "$p"
  expect 0 run "$p"
  if [ "$(cat out)" != hi ]; then
    echo "  FAIL kai run $p — stdout '$(cat out)' (want 'hi')"
    fail=$((fail + 1))
  fi
done

for p in pk ./pk ./pk/; do
  expect 0 build "$p"
  expect 0 typecheck "$p"
done

expect 2 build ./missing
grep -q "package directory (got: ./missing)" err \
  || { echo "  FAIL kai build ./missing — wrong diagnostic"; sed 's/^/      /' err; fail=$((fail + 1)); }

if [ "$fail" -ne 0 ]; then
  echo "path-specs FAIL ($fail)"
  exit 1
fi
echo "path-specs OK — ./file.kai and sub/file.kai are files and a directory is a package on every verb"
