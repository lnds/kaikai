#!/usr/bin/env bash
# Phase-by-phase wall breakdown of `kai build empty.kai`. Cited in
# `docs/cache-design.md`. Re-run after any caching lane closes to
# verify the breakdown still holds.
#
# Usage: tools/bench-phases.sh
# Reqs:  stage2/kaic2 built (run `make -C stage2 kaic2` first).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
STDLIB_ROOT="$ROOT/stdlib"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

SRC="$TMP/empty.kai"
printf 'fn main() : Unit { () }\n' > "$SRC"

[ -x "$KAIC2" ] || { echo "kaic2 not built; run: make -C stage2 kaic2" >&2; exit 1; }

# Build the same prelude_flags shape that bin/kai uses.
STDLIB_CORE_DIR="$STDLIB_ROOT/core"
prelude_flags=""
for core_file in "$STDLIB_CORE_DIR"/*.kai; do
  [ -f "$core_file" ] && prelude_flags="$prelude_flags --prelude $core_file"
done
for f in protocols.kai effects.kai array.kai random.kai random_secure.kai \
         encoding/base64.kai encoding/hex.kai \
         collections/map.kai collections/set.kai collections/queue.kai collections/stack.kai \
         math/numeric.kai math/int.kai math/real.kai math/complex.kai \
         encoding/json.kai encoding/toml.kai \
         money/decimal.kai money/money.kai money/fx.kai \
         uuid.kai regexp.kai path.kai \
         crypto/hash.kai crypto/mac.kai; do
  full="$STDLIB_ROOT/$f"
  [ -f "$full" ] && prelude_flags="$prelude_flags --prelude $full"
done

median_real() {
  for _ in 1 2 3 4 5; do
    /usr/bin/time -p sh -c "$1" 2>&1 | awk '/^real/{print $2}'
  done | sort -n | sed -n '3p'
}

echo "stage                  median(s)"
echo "---------------------- --------"
printf "%-22s %s\n" "no-stdlib"             "$(median_real "$KAIC2 --path $STDLIB_ROOT $SRC > $TMP/out.c 2>/dev/null")"
printf "%-22s %s\n" "tokens (lex)"          "$(median_real "$KAIC2 --tokens --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out 2>/dev/null")"
printf "%-22s %s\n" "ast (parse)"           "$(median_real "$KAIC2 --ast --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out 2>/dev/null")"
printf "%-22s %s\n" "check (typecheck)"     "$(median_real "$KAIC2 --check --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out 2>/dev/null")"
printf "%-22s %s\n" "infer (typecheck)"     "$(median_real "$KAIC2 --infer --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out 2>/dev/null")"
printf "%-22s %s\n" "default (emit C)"      "$(median_real "$KAIC2 --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out.c 2>/dev/null")"
