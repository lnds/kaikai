#!/bin/sh
# Keeps the program generator (tools/progen) in step with the language:
# the differential run over its programs is nightly, so without this a
# surface change would break the generator with nobody watching.
#
# The generator must build; the first seed must regenerate the pinned
# program byte for byte (a seed means one program on every machine);
# and each seed's program must build on the C backend, exit 0, and end
# in its checksum line. The seeds are ones no entry of
# tools/progen-diff-skips.txt applies to.
#
# After a deliberate change to the generator, refresh the pin with
#   tools/progen-smoke.sh --update
#
# Usage: tools/progen-smoke.sh [--update]

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
PIN="$ROOT/tools/progen-seed.expected"
OUT="$ROOT/stage2/build/progen-smoke"
SEEDS="1 4 5 6"

fail() {
  echo "progen-smoke FAIL — $1"
  [ -z "${2:-}" ] || tail -20 "$2"
  exit 1
}

rm -rf "$OUT"
mkdir -p "$OUT"
"$KAI" build --backend=c "$ROOT/tools/progen/main.kai" -o "$OUT/progen" > "$OUT/progen.log" 2>&1 \
  || fail "the generator does not build" "$OUT/progen.log"

for seed in $SEEDS; do
  "$OUT/progen" "$seed" > "$OUT/$seed.kai" || fail "the generator failed on seed $seed"
done

if [ "${1:-}" = "--update" ]; then
  cp "$OUT/${SEEDS%% *}.kai" "$PIN"
  echo "progen-smoke: pinned seed ${SEEDS%% *} in tools/progen-seed.expected"
fi
cmp -s "$OUT/${SEEDS%% *}.kai" "$PIN" \
  || fail "seed ${SEEDS%% *} no longer generates tools/progen-seed.expected (rerun with --update if the generator changed on purpose)"

for seed in $SEEDS; do
  "$KAI" build --backend=c "$OUT/$seed.kai" -o "$OUT/$seed.bin" > "$OUT/$seed.log" 2>&1 \
    || fail "seed $seed does not build on the C backend" "$OUT/$seed.log"
  "$OUT/$seed.bin" > "$OUT/$seed.out" 2>&1 || fail "seed $seed exits non-zero" "$OUT/$seed.out"
  tail -1 "$OUT/$seed.out" | grep -q '^checksum=' || fail "seed $seed prints no checksum" "$OUT/$seed.out"
done

echo "progen-smoke OK — seeds $SEEDS regenerate, build on C, and print their checksum"
