#!/bin/sh
# Phase B user-cache POSITIVE differential fixture — cold == warm == oracle.
#
# The load-bearing correctness gate. selfhost byte-identity does NOT
# cover the user cache (the compiler is built with the cache off, so
# nothing exercises a cache hit during selfhost). This fixture is the
# behavioural check the cache stands or falls on: a cache hit must
# reconstruct exactly the [Decl] a fresh parse would, so the emitted C
# is byte-identical whether the build was
#
#   - cold-cache  (cache enabled, first run, all misses -> blobs written)
#   - warm-cache  (cache enabled, second run, all hits   -> blobs read)
#   - no-cache    (cache disabled, the oracle)
#
# Any divergence means the codec dropped or reordered a node and the
# cache is serving subtly-wrong decls — the one failure mode worse than
# having no cache at all.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

# A multi-module project that exercises a spread of decl/expr shapes
# through the cache: a sum type, a record, a generic fn, a match, a
# pipe, string interpolation.
cat > "$PROJ/shapes.kai" <<'EOF'
pub type Shape = Circle(Int) | Square(Int)

pub fn area(s: Shape) : Int = match s {
  Circle(r) -> r * r * 3
  Square(w) -> w * w
}

pub fn label(s: Shape) : String = match s {
  Circle(_) -> "circle"
  Square(_) -> "square"
}
EOF
cat > "$PROJ/main.kai" <<'EOF'
import shapes
fn describe(s: shapes.Shape) : String =
  "#{shapes.label(s)}=#{int_to_string(shapes.area(s))}"
fn main() : Unit / Console {
  print(describe(shapes.Circle(2)))
  print(describe(shapes.Square(3)))
}
EOF

mkdir -p "$PROJ/.kai-cache"

# Oracle: cache disabled.
"$KAIC2" --path "$PROJ" "$PROJ/main.kai" > "$PROJ/oracle.c" 2>/dev/null

# Cold: cache enabled, empty cache -> all misses, blobs written.
rm -f "$PROJ/.kai-cache/"*.kab 2>/dev/null || true
"$KAIC2" --user-cache --path "$PROJ" "$PROJ/main.kai" > "$PROJ/cold.c" 2>/dev/null

# Warm: cache enabled, blobs present -> all hits, parse skipped.
"$KAIC2" --user-cache --path "$PROJ" "$PROJ/main.kai" > "$PROJ/warm.c" 2>/dev/null

if ! cmp -s "$PROJ/cold.c" "$PROJ/oracle.c"; then
  echo "userb_cold_warm_identical FAIL — cold-cache C differs from no-cache oracle"
  diff "$PROJ/oracle.c" "$PROJ/cold.c" | head -20
  exit 1
fi
if ! cmp -s "$PROJ/warm.c" "$PROJ/oracle.c"; then
  echo "userb_cold_warm_identical FAIL — warm-cache C differs from no-cache oracle"
  diff "$PROJ/oracle.c" "$PROJ/warm.c" | head -20
  exit 1
fi

# Sanity: the warm run actually read a blob (the cache was populated).
n="$(ls "$PROJ/.kai-cache/"*.kab 2>/dev/null | wc -l | tr -d ' ')"
if [ "$n" = "0" ]; then
  echo "userb_cold_warm_identical FAIL — no blobs written, hit path never exercised"
  exit 1
fi

echo "userb_cold_warm_identical OK — cold == warm == oracle, $n blob(s) cached"
exit 0
