#!/bin/sh
# examples/packages/shadowed_core_names — a user file named like a core or
# stdlib module is the user's module: it builds and lints clean, and one
# that imports the module it shadows says so. Fixtures are generated in a
# tmpdir.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "shadowed_core_names: FAIL — $1" >&2; exit 1; }

# 1 — a root file named like a core module runs.
for m in tuple option result list string; do
  mkdir -p "$TMP/$m"
  printf 'fn main() : Unit / Console = print("%s ran")\n' "$m" > "$TMP/$m/$m.kai"
  out="$(cd "$TMP/$m" && "$KAI" run "$m.kai" 2>&1)" || fail "a root $m.kai did not run: $out"
  echo "$out" | grep -q "^$m ran$" || fail "a root $m.kai did not print its output"
  warns="$(cd "$TMP/$m" && "$KAI" lint "$m.kai" 2>&1 | grep -c "warning" || true)"
  [ "$warns" -eq 0 ] || fail "a root $m.kai drew $warns lint warnings"
done

# 2 — a file importing the module it shadows says so.
mkdir -p "$TMP/seq"
printf 'import seq\n\npub fn mine() : Int = 1\n' > "$TMP/seq/seq.kai"
printf 'import seq\n\nfn main() : Int = seq.mine() - 1\n' > "$TMP/seq/main.kai"
st=0
out="$(cd "$TMP/seq" && "$KAI" run main.kai 2>&1)" || st=$?
[ "$st" -ne 0 ] || fail "a file importing the module it shadows built"
echo "$out" | grep -q "shadows the module it imports" || fail "the shadowing import was not named"
echo "$out" | grep -q "import cycle" && fail "the shadowing import reported a cycle"

echo "shadowed_core_names: ok"
