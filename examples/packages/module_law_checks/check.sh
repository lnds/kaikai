#!/bin/sh
# examples/packages/module_law_checks — the law checks synthesised for a
# lawful `Functorial` impl read names from the impl's own module, so a
# sibling module exporting its own `map` neither breaks them nor is
# called by them. Fixtures are generated in a tmpdir.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "module_law_checks: FAIL — $1" >&2; exit 1; }

mkdir -p "$TMP/p"
printf 'name = "p"\nedition = "hanga-roa"\n' > "$TMP/p/kai.toml"
cat > "$TMP/p/chain.kai" <<'KAI'
pub protocol Sequence[s: Shape] : Functorial {
  map(xs: s[Int], f: (Int) -> Int) : s[Int]
  foldl(xs: s[Int], init: Int, f: (Int, Int) -> Int) : Int
}

pub type Rope[a] = Knot | Strand(a, Rope[a])

impl Sequence for Rope {
  fn map(xs: Rope[Int], f: (Int) -> Int) : Rope[Int] = match xs {
    Knot         -> Knot
    Strand(h, t) -> Strand(f(h), map(t, f))
  }
  fn foldl(xs: Rope[Int], init: Int, f: (Int, Int) -> Int) : Int = match xs {
    Knot         -> init
    Strand(h, t) -> foldl(t, f(init, h), f)
  }
}
KAI
printf 'pub fn map(xs: [Int], f: (Int) -> Int) : [Int] = xs\n' > "$TMP/p/other.kai"
printf 'import chain\nimport other\n\nfn main() : Int = other.map([1], (x) => x).length()\n' > "$TMP/p/main.kai"

out="$(cd "$TMP/p" && "$KAI" check . 2>&1)" || fail "the law checks of a non-root impl failed: $out"
echo "$out" | grep -q "6/6 checks passed" || fail "the law checks of the module's impl did not all run and pass"

echo "module_law_checks: ok"
