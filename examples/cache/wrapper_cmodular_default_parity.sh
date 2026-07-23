#!/bin/sh
# The C path resolves to separate compilation when the shared core cache
# is live and a hasher exists; KAI_MODULAR=0 forces the single TU and a
# dead cache falls back to it. This gate asserts the routing on all
# three states and that the two paths produce byte-identical program
# output — the property that makes the default flip safe. The stats
# line is the routing witness (KAI_MODULAR_STATS=1), with a negative
# case proving the witness does not fire vacuously.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/main.kai" <<'EOF'
fn scale(xs: [Int], k: Int) : [Int] = xs | (x => x * k)

fn main() : Unit / Console {
  let xs = scale([1, 2, 3, 4], 3)
  print("sum=#{int_to_string(list.sum(xs))}")
}
EOF

ccdir="$PROJ/core-cache"

# Warm the core cache so the auto route can engage.
KAI_CORE_CACHE_DIR="$ccdir" KAI_BACKEND=c "$KAI" build "$PROJ/main.kai" -o "$PROJ/warm" >/dev/null 2>&1

# 1. Auto with a live cache takes the modular path.
if ! KAI_CORE_CACHE_DIR="$ccdir" KAI_BACKEND=c KAI_MODULAR_STATS=1 \
     "$KAI" build "$PROJ/main.kai" -o "$PROJ/auto" 2>"$PROJ/auto.err"; then
  echo "FAIL: auto modular build errored"; cat "$PROJ/auto.err"; exit 1
fi
grep -q "kai: c-modular:" "$PROJ/auto.err" \
  || { echo "FAIL: live cache did not route to c-modular"; cat "$PROJ/auto.err"; exit 1; }

# 2. KAI_MODULAR=0 forces the single TU.
KAI_CORE_CACHE_DIR="$ccdir" KAI_BACKEND=c KAI_MODULAR=0 KAI_MODULAR_STATS=1 \
  "$KAI" build "$PROJ/main.kai" -o "$PROJ/single" 2>"$PROJ/single.err"
if grep -q "kai: c-modular:" "$PROJ/single.err"; then
  echo "FAIL: KAI_MODULAR=0 still routed to c-modular"; exit 1
fi

# 3. A dead cache falls back to the single TU.
KAI_CORE_CACHE=0 KAI_BACKEND=c KAI_MODULAR_STATS=1 \
  "$KAI" build "$PROJ/main.kai" -o "$PROJ/nocache" 2>"$PROJ/nocache.err"
if grep -q "kai: c-modular:" "$PROJ/nocache.err"; then
  echo "FAIL: dead cache still routed to c-modular"; exit 1
fi

# 4. Byte-identical program output across the three builds.
"$PROJ/auto"    > "$PROJ/out.auto"
"$PROJ/single"  > "$PROJ/out.single"
"$PROJ/nocache" > "$PROJ/out.nocache"
cmp -s "$PROJ/out.auto" "$PROJ/out.single" \
  || { echo "FAIL: modular output differs from single-TU output"; exit 1; }
cmp -s "$PROJ/out.single" "$PROJ/out.nocache" \
  || { echo "FAIL: cached single-TU output differs from uncached"; exit 1; }
grep -q "sum=30" "$PROJ/out.auto" \
  || { echo "FAIL: wrong program output"; cat "$PROJ/out.auto"; exit 1; }

echo "PASS: c-modular default routing + parity (auto=modular, forced/dead-cache=single-TU, outputs identical)"
