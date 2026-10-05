#!/bin/sh
# Compiles and runs one generated program on both backends: it imports
# every stdlib module that lives in a subdirectory, and exercises each
# piece of sugar the compiler lowers to a call into a stdlib module
# (map, set and hashmap literals and indexing, minted big and decimal
# literals, interpolation, pipes). A module's home follows its import
# path, so a reference minted with a home spelled by hand compiles here
# into a symbol nothing defines.
#
# Usage: stdlib-module-smoke.sh [backend...]   (default: c native)
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
DIR="$(mktemp -d "${TMPDIR:-/tmp}/kai-stdlib-smoke.XXXXXX")"
trap 'rm -rf "$DIR"' EXIT

{
  (cd "$ROOT/stdlib" && find . -mindepth 2 -name '*.kai' | sort) | while read -r f; do
    p=$(printf '%s' "${f#./}" | sed 's/\.kai$//; s|/|.|g')
    printf 'import %s as smoke_%s\n' "$p" "$(printf '%s' "$p" | tr . _)"
  done
  cat <<'KAI'
import collections.map
import collections.set
import collections.hashmap
import math.bigint
import decimal as d
import decimal_big as db

fn show(o: Option[Int]) : String = match o {
  Some(v) -> int_to_string(v)
  None    -> "none"
}

fn main() : Unit / Stdout + Mutable {
  let m = %{ "a": 1, "b": 2 }
  Stdout.print("map index: #{show(m["a"])} #{show(m["z"])}")
  let s = %[1, 2, 2, 3]
  Stdout.print("set size: #{set.size(s)}")
  let h = hashmap.empty()
  hashmap.put(h, "k", 7)
  Stdout.print("hashmap index: #{show(h["k"])}")
  let x : bigint.BigInt = 340282366920938463463374607431768211456
  Stdout.print("bigint: #{bigint.to_string(x)}")
  let y : d.Decimal = 0.20
  Stdout.print("decimal: #{d.to_string(y)}")
  let z : db.DecimalBig = 3.141592653589793238462643383279
  Stdout.print("decimal_big: #{db.to_string(z)}")
  Stdout.print("pipe: #{[1, 2, 3] |> list.map((n) => n * 2) |> list.length}")
}
KAI
} > "$DIR/main.kai"

cat > "$DIR/want" <<'OUT'
map index: 1 none
set size: 3
hashmap index: 7
bigint: 340282366920938463463374607431768211456
decimal: 0.20
decimal_big: 3.141592653589793238462643383279
pipe: 3
OUT

fail=0
for backend in ${*:-c native}; do
  if KAI_BACKEND=$backend "$KAI" run "$DIR/main.kai" > "$DIR/got-$backend" 2> "$DIR/err-$backend" \
     && diff -u "$DIR/want" "$DIR/got-$backend" > "$DIR/diff-$backend"; then
    echo "stdlib-module-smoke OK ($backend)"
  else
    echo "stdlib-module-smoke FAIL ($backend)"
    cat "$DIR/diff-$backend" 2>/dev/null || true
    grep -E '^(error|kai: error)|undefined reference|^Undefined' "$DIR/err-$backend" | head -20
    fail=1
  fi
done
exit $fail
