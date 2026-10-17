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
#   - warm-cache (second run: hit, the core pruned to what the program
#                 reaches, every cached core TU spliced)
#
# The user program deliberately exercises generics, a protocol impl on a
# user type, a user effect with a handler, and units of measure — the
# surfaces whose lowering most plausibly could leak user state into a
# core TU. The cold and warm streams must be equal and agree
# with the oracle on every core TU and its slices, and its root and library TUs
# may only have lost what the pruning dropped (tools/cmodular-prune-diff.sh).
#
# Also gates the key discipline: a different toolchain id must MISS
# (never splice another compiler build's C), and a test-shaped build
# must not consult the cache at all.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the cache keys carry the
# edition, so this fixture must exercise the one the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
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
"$KAIC2" $EDITION_FLAG --emit=c-modular --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > "$PROJ/oracle.stream" 2>/dev/null

# Cold: empty cache -> emit-cache miss, entry written.
"$KAIC2" $EDITION_FLAG --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > "$PROJ/cold.stream" 2> "$PROJ/cold.err"
grep -q "core-emit-cache: miss" "$PROJ/cold.err" || {
  echo "emitc_cold_warm_identical FAIL — cold run did not report an emit-cache miss"
  cat "$PROJ/cold.err"; exit 1; }

# Warm: entry present -> hit, core TU bodies spliced.
"$KAIC2" $EDITION_FLAG --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > "$PROJ/warm.stream" 2> "$PROJ/warm.err"
grep -q "core-emit-cache: hit" "$PROJ/warm.err" || {
  echo "emitc_cold_warm_identical FAIL — warm run did not report an emit-cache hit"
  cat "$PROJ/warm.err"; exit 1; }

cmp -s "$PROJ/cold.stream" "$PROJ/warm.stream" || {
  echo "emitc_cold_warm_identical FAIL — cold-cache stream differs from the warm one"
  exit 1; }
"$ROOT/tools/cmodular-prune-diff.sh" "$KAIC2" "$STDLIB" "$PROJ/main.kai" "$PROJ/main.kai" \
  > "$PROJ/prune.out" 2>&1 || {
  echo "emitc_cold_warm_identical FAIL — warm-cache stream differs from no-cache oracle"
  cat "$PROJ/prune.out"; exit 1; }

# A different toolchain id must miss: never splice another build's C.
"$KAIC2" $EDITION_FLAG --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid2 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > /dev/null 2> "$PROJ/tid2.err"
grep -q "core-emit-cache: miss" "$PROJ/tid2.err" || {
  echo "emitc_cold_warm_identical FAIL — different toolchain id did not miss"
  cat "$PROJ/tid2.err"; exit 1; }

# A body-only edit hits: the hot rebuild loop.
sed 's/"light"/"lighter"/' "$PROJ/main.kai" > "$PROJ/edited.kai"
"$KAIC2" $EDITION_FLAG --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/edited.kai" \
  > /dev/null 2> "$PROJ/edited.err"
grep -q "core-emit-cache: hit" "$PROJ/edited.err" || {
  echo "emitc_cold_warm_identical FAIL — body-only edit did not hit"
  cat "$PROJ/edited.err"; exit 1; }

# Adding an impl changes the type-level surface, not the unwind variant:
# it hits, and the core TUs it is compiled against stay those of an
# unpruned compile.
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
"$KAIC2" $EDITION_FLAG --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/impl_added.kai" \
  > /dev/null 2> "$PROJ/impl.err"
grep -q "core-emit-cache: hit" "$PROJ/impl.err" || {
  echo "emitc_cold_warm_identical FAIL — added impl missed the core TU set"
  cat "$PROJ/impl.err"; exit 1; }
"$ROOT/tools/cmodular-prune-diff.sh" "$KAIC2" "$STDLIB" "$PROJ/main.kai" "$PROJ/impl_added.kai" \
  > "$PROJ/impl.out" 2>&1 || {
  echo "emitc_cold_warm_identical FAIL — added impl changed a core TU"
  cat "$PROJ/impl.out"; exit 1; }

# A test-shaped build must not consult the emit cache (bmode gating).
"$KAIC2" $EDITION_FLAG --emit=c-modular --test --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > /dev/null 2> "$PROJ/test.err" || true
grep -q "core-emit-cache: off" "$PROJ/test.err" || {
  echo "emitc_cold_warm_identical FAIL — test-shaped build consulted the emit cache"
  cat "$PROJ/test.err"; exit 1; }

echo "emitc_cold_warm_identical OK"
