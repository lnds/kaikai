#!/bin/sh
# Core emit-cache POSITIVE differential fixture — cold == warm == oracle.
#
# The load-bearing correctness gate for the emitted-C core TU cache on
# the c-modular backend. A warm entry splices the cached core TU bodies
# into the marker stream instead of re-emitting them, so the stream must
# be byte-identical whether the build was
#
#   - no-cache   (cache flags absent, the oracle)
#   - cold-cache (dir + toolchain id set, first run: miss, entry written)
#   - warm-cache (second run: hit, 13 core TU bodies spliced)
#
# The user program deliberately exercises generics, a protocol impl on a
# user type, a user effect with a handler, and units of measure — the
# surfaces whose lowering most plausibly could leak user state into a
# core TU. Any stream divergence means the splice is not sound.
#
# Also gates the key discipline: a different toolchain id must MISS
# (never splice another compiler build's C), and a test-shaped build
# must not consult the cache at all.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
STDLIB="$ROOT/stdlib"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/main.kai" <<'EOF'
unit m
unit s

type Shade = Light | Dark

impl Show for Shade {
  fn show(x: Shade) : String = match x {
    Light -> "light"
    Dark  -> "dark"
  }
}

effect Tick {
  tick() : Int
}

fn speeds[u: Measure](xs: [Real<u>], t: Real<s>) : [Real<u/s>] =
  xs | (x) => x / t

fn main() : Unit / Stdout = {
  let xs = [1, 2, 3, 4]
  let ys = xs | (x) => x * x
  let total = foldr(ys, 0, (a, b) => a + b)
  let vs = speeds([10.0<m>, 20.0<m>], 2.0<s>)
  let n = handle {
    Tick.tick() + Tick.tick()
  } with Tick {
    tick(resume) -> resume(7)
  }
  Stdout.print("#{total} #{show(Dark)} #{list_length(vs)} #{n}")
}
EOF

CACHE="$PROJ/core-cache"
mkdir -p "$CACHE"

# Oracle: no cache flags — the core is emitted fresh.
"$KAIC2" --emit=c-modular --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > "$PROJ/oracle.stream" 2>/dev/null

# Cold: empty cache -> emit-cache miss, entry written.
"$KAIC2" --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > "$PROJ/cold.stream" 2> "$PROJ/cold.err"
grep -q "core-emit-cache: miss" "$PROJ/cold.err" || {
  echo "emitc_cold_warm_identical FAIL — cold run did not report an emit-cache miss"
  cat "$PROJ/cold.err"; exit 1; }

# Warm: entry present -> hit, core TU bodies spliced.
"$KAIC2" --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > "$PROJ/warm.stream" 2> "$PROJ/warm.err"
grep -q "core-emit-cache: hit" "$PROJ/warm.err" || {
  echo "emitc_cold_warm_identical FAIL — warm run did not report an emit-cache hit"
  cat "$PROJ/warm.err"; exit 1; }

cmp -s "$PROJ/cold.stream" "$PROJ/oracle.stream" || {
  echo "emitc_cold_warm_identical FAIL — cold-cache stream differs from no-cache oracle"
  exit 1; }
cmp -s "$PROJ/warm.stream" "$PROJ/oracle.stream" || {
  echo "emitc_cold_warm_identical FAIL — warm-cache stream differs from no-cache oracle"
  exit 1; }

# A different toolchain id must miss: never splice another build's C.
"$KAIC2" --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid2 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > /dev/null 2> "$PROJ/tid2.err"
grep -q "core-emit-cache: miss" "$PROJ/tid2.err" || {
  echo "emitc_cold_warm_identical FAIL — different toolchain id did not miss"
  cat "$PROJ/tid2.err"; exit 1; }

# A body-only edit keeps the surface key: the hot rebuild loop hits.
sed 's/"light"/"lighter"/' "$PROJ/main.kai" > "$PROJ/edited.kai"
"$KAIC2" --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/edited.kai" \
  > /dev/null 2> "$PROJ/edited.err"
grep -q "core-emit-cache: hit" "$PROJ/edited.err" || {
  echo "emitc_cold_warm_identical FAIL — body-only edit did not hit"
  cat "$PROJ/edited.err"; exit 1; }

# Adding an impl changes the type-level surface: must MISS (a user
# impl can perturb synthetic decls attributed to a core TU).
cat "$PROJ/main.kai" > "$PROJ/impl_added.kai"
cat >> "$PROJ/impl_added.kai" <<'EOF'

impl Eq for Shade {
  fn eq(a: Shade, b: Shade) : Bool = match (a, b) {
    (Light, Light) -> true
    (Dark, Dark)   -> true
    _              -> false
  }
}
EOF
"$KAIC2" --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/impl_added.kai" \
  > /dev/null 2> "$PROJ/impl.err"
grep -q "core-emit-cache: miss" "$PROJ/impl.err" || {
  echo "emitc_cold_warm_identical FAIL — added impl did not change the surface key"
  cat "$PROJ/impl.err"; exit 1; }

# A test-shaped build must not consult the emit cache (bmode gating).
"$KAIC2" --emit=c-modular --test --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > /dev/null 2> "$PROJ/test.err" || true
grep -q "core-emit-cache: off" "$PROJ/test.err" || {
  echo "emitc_cold_warm_identical FAIL — test-shaped build consulted the emit cache"
  cat "$PROJ/test.err"; exit 1; }

echo "emitc_cold_warm_identical OK"
