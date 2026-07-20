#!/bin/sh
# Core-cache POSITIVE fixture for the MONOLITHIC compile path via the
# shared `--core-cache-dir` (the dir bin/kai resolves), as opposed to
# the per-project `--user-cache` dir the sibling corec_* fixtures use.
#
# `run` loads the core through the same `load_core` call for every
# monolithic mode (C emit, --emit=kir, --emit=native), so proving the
# parse layer hits here proves it for the native front-end too. The
# `--core-cache-stats` parse-layer line is the observable: a cold run
# must report `miss`, a warm run must report `hit`, and the emitted
# KIR must be byte-identical cold vs warm vs cache-off.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
STDLIB="$ROOT/stdlib"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/main.kai" <<'EOF'
fn main() : Unit / Console {
  let xs = [1, 2, 3]
  print("sum=#{int_to_string(list.sum(xs))}")
}
EOF

CCDIR="$PROJ/core-cache"
mkdir -p "$CCDIR"

# Oracle: cache off — fresh core lex+parse.
"$KAIC2" --emit=kir --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > "$PROJ/oracle.kir" 2>/dev/null

# Cold: empty dir — every core module misses, blobs are written.
"$KAIC2" --emit=kir --path "$STDLIB" --path "$PROJ" \
  --core-cache-dir "$CCDIR" --toolchain-id fixture --core-cache-stats \
  "$PROJ/main.kai" > "$PROJ/cold.kir" 2> "$PROJ/cold.err"

# Warm: blobs present — every core module must hit (lex+parse skipped).
"$KAIC2" --emit=kir --path "$STDLIB" --path "$PROJ" \
  --core-cache-dir "$CCDIR" --toolchain-id fixture --core-cache-stats \
  "$PROJ/main.kai" > "$PROJ/warm.kir" 2> "$PROJ/warm.err"

if ! grep -q "core-parse-cache: miss" "$PROJ/cold.err"; then
  echo "corec_monolithic_hit_stats FAIL — cold run did not report a parse-layer miss"
  cat "$PROJ/cold.err"
  exit 1
fi
if ! grep -q "core-parse-cache: hit" "$PROJ/warm.err"; then
  echo "corec_monolithic_hit_stats FAIL — warm run did not report a parse-layer hit"
  cat "$PROJ/warm.err"
  exit 1
fi
if grep -q "core-parse-cache: miss" "$PROJ/warm.err"; then
  echo "corec_monolithic_hit_stats FAIL — warm run still misses (partial hit)"
  cat "$PROJ/warm.err"
  exit 1
fi

n="$(ls "$CCDIR"/core-*.kab 2>/dev/null | wc -l | tr -d ' ')"
if [ "$n" = "0" ]; then
  echo "corec_monolithic_hit_stats FAIL — no core blobs written by the cold run"
  exit 1
fi

if ! cmp -s "$PROJ/cold.kir" "$PROJ/oracle.kir"; then
  echo "corec_monolithic_hit_stats FAIL — cold KIR differs from cache-off oracle"
  diff "$PROJ/oracle.kir" "$PROJ/cold.kir" | head -20
  exit 1
fi
if ! cmp -s "$PROJ/warm.kir" "$PROJ/oracle.kir"; then
  echo "corec_monolithic_hit_stats FAIL — warm KIR differs from cache-off oracle"
  diff "$PROJ/oracle.kir" "$PROJ/warm.kir" | head -20
  exit 1
fi

echo "corec_monolithic_hit_stats OK — $n blob(s), cold miss -> warm hit, KIR identical"
exit 0
