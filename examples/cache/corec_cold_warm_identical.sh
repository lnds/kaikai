#!/bin/sh
# Issue #825 core-cache POSITIVE differential fixture — cold == warm == oracle.
#
# The load-bearing correctness gate for the core (auto-loaded stdlib)
# post-parse cache. selfhost byte-identity does NOT cover it (the
# compiler is built with the cache off, so nothing exercises a core
# cache hit during selfhost). This fixture is the behavioural check the
# core cache stands or falls on: a hit must reconstruct exactly the
# [Decl] a fresh parse of the core module would, so the emitted C is
# byte-identical whether the build was
#
#   - cold-cache  (cache enabled, first run, all misses -> 12 blobs written)
#   - warm-cache  (cache enabled, second run, all hits   -> 12 blobs read)
#   - no-cache    (cache disabled, the oracle)
#
# Any divergence means the codec dropped or reordered a node and the
# cache is serving subtly-wrong CORE decls — the one failure mode worse
# than having no cache at all (every program typechecks against wrong
# stdlib symbols).

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
STDLIB="$ROOT/stdlib"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

# A user file that pulls a spread of core modules through the cache:
# list (map/sum), string interpolation, Option, a match.
cat > "$PROJ/main.kai" <<'EOF'
fn classify(n: Int) : String = match n {
  0 -> "zero"
  _ -> "nonzero"
}
fn main() : Unit / Console {
  let xs = [1, 2, 3, 4]
  let ys = list.map(xs, (x) => x * x)
  print("sum=#{int_to_string(list.sum(ys))} #{classify(0)}")
}
EOF

mkdir -p "$PROJ/.kai-cache"

# Oracle: cache disabled — the core is lexed+parsed fresh.
"$KAIC2" --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" > "$PROJ/oracle.c" 2>/dev/null

# Cold: cache enabled, empty cache -> all core misses, 12 blobs written.
rm -f "$PROJ/.kai-cache/core-"*.kab 2>/dev/null || true
"$KAIC2" --user-cache --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" > "$PROJ/cold.c" 2>/dev/null

# Warm: cache enabled, blobs present -> all core hits, lex+parse skipped.
"$KAIC2" --user-cache --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" > "$PROJ/warm.c" 2>/dev/null

if ! cmp -s "$PROJ/cold.c" "$PROJ/oracle.c"; then
  echo "corec_cold_warm_identical FAIL — cold-cache C differs from no-cache oracle"
  diff "$PROJ/oracle.c" "$PROJ/cold.c" | head -20
  exit 1
fi
if ! cmp -s "$PROJ/warm.c" "$PROJ/oracle.c"; then
  echo "corec_cold_warm_identical FAIL — warm-cache C differs from no-cache oracle"
  diff "$PROJ/oracle.c" "$PROJ/warm.c" | head -20
  exit 1
fi

# Sanity: the cold run actually wrote core blobs (the 12-module set).
n="$(ls "$PROJ/.kai-cache/core-"*.kab 2>/dev/null | wc -l | tr -d ' ')"
if [ "$n" = "0" ]; then
  echo "corec_cold_warm_identical FAIL — no core blobs written, hit path never exercised"
  exit 1
fi

echo "corec_cold_warm_identical OK — cold == warm == oracle, $n core blob(s) cached"
exit 0
