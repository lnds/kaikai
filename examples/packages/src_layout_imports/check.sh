#!/bin/sh
# examples/packages/src_layout_imports — `kai test` finds the modules the
# entry imports under a `src/` layout, and an impl alone is not a test block.
# Fixtures are generated in a tmpdir.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "src_layout_imports: FAIL — $1" >&2; exit 1; }

IMPL='pub type Spell = Arm | Compare\nimpl Show for Spell { fn show(s: Spell) : String = match s { Arm -> "arm"  Compare -> "compare" } }\n'
MAIN='import p.e\n\nfn main() : Unit / Stdout = println(show(Arm))\n'

# 1 — src layout, imported impl-only module: no warning.
mkdir -p "$TMP/src/src/p"
printf 'name = "p"\nversion = "0.1.0"\nentry = "src/main.kai"\n' > "$TMP/src/kai.toml"
printf "$MAIN" > "$TMP/src/src/main.kai"
printf "$IMPL" > "$TMP/src/src/p/e.kai"
out="$(cd "$TMP/src" && "$KAI" test 2>&1)" || fail "'kai test' failed on the src-layout package"
echo "$out" | grep -q "not reachable" && fail "an imported module under src/ drew the unreachable warning"

# 2 — src layout, unimported module with a real test block: warning stays.
printf 'pub fn stray() : Int = 1\n\ntest "stray" {\n  assert stray() == 1\n}\n' > "$TMP/src/src/p/stray.kai"
out="$(cd "$TMP/src" && "$KAI" test 2>&1)" || fail "'kai test' failed with a stray module"
echo "$out" | grep -q "src/p/stray.kai declares test blocks but is not reachable" || fail "an unimported module lost its warning"
echo "$out" | grep -q "e.kai.*not reachable" && fail "the imported module was reported unreachable beside the stray one"

# 3 — unimported impl-only module: the warning names impls, not test blocks.
rm "$TMP/src/src/p/stray.kai"
printf 'pub type Other = One | Two\nimpl Show for Other { fn show(s: Other) : String = match s { One -> "one"  Two -> "two" } }\n' > "$TMP/src/src/p/o.kai"
out="$(cd "$TMP/src" && "$KAI" test 2>&1)" || fail "'kai test' failed with an unimported impl module"
echo "$out" | grep -q "o.kai declares impls whose law checks run under test" || fail "an unimported impl module lost its warning"
echo "$out" | grep -q "o.kai declares test blocks" && fail "an impl was reported as a test block"

# 4 — root layout: unchanged.
mkdir -p "$TMP/root/p"
printf 'name = "q"\nversion = "0.1.0"\n' > "$TMP/root/kai.toml"
printf "$MAIN" > "$TMP/root/main.kai"
printf "$IMPL" > "$TMP/root/p/e.kai"
out="$(cd "$TMP/root" && "$KAI" test 2>&1)" || fail "'kai test' failed on the root-layout package"
echo "$out" | grep -q "not reachable" && fail "an imported module in a root layout drew the unreachable warning"

echo "src_layout_imports: ok"
