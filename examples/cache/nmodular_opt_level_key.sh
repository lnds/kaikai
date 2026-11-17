#!/bin/sh
# The optimisation level is part of a native partition's key: the same
# program built at two levels leaves two sets of objects in one cache, and
# a build at one level never links an object emitted at the other.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

fail() { echo "nmodular_opt_level_key FAIL — $1"; exit 1; }

native_capable=0
if [ -x "$KAIC2" ]; then
  printf 'fn main() : Unit = ()\n' > "$PROJ/probe.kai"
  "$KAIC2" $EDITION_FLAG --emit=native --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/probe.kai" \
    >/dev/null 2>"$PROJ/native-probe.err" || true
  grep -q "not built into this compiler" "$PROJ/native-probe.err" 2>/dev/null || native_capable=1
fi
if [ "$native_capable" != "1" ]; then
  echo "nmodular_opt_level_key SKIP — kaic2 has no libLLVM native backend"
  exit 0
fi

mkdir -p "$PROJ/p"
printf 'pub fn value(n: Int) : Int = n * 3 + 1\n' > "$PROJ/p/dep.kai"
cat > "$PROJ/p/main.kai" <<'EOF'
import dep
fn main() : Unit / Console { print(int_to_string(dep.value(14))) }
EOF

nm="$PROJ/nm"
build() {
  KAI_NATIVE_MODULAR_CACHE_DIR="$nm" KAI_CORE_CACHE_DIR="$PROJ/core" KAI_BIN_MEMO=0 \
    KAI_BACKEND=native KAI_NATIVE_OPT="$1" "$KAI" build "$PROJ/p/main.kai" -o "$PROJ/out$1" \
    2>"$PROJ/err$1" 1>/dev/null || { cat "$PROJ/err$1"; fail "O$1 build failed"; }
  [ "$("$PROJ/out$1")" = "43" ] || fail "O$1 build printed the wrong output"
}
count() { find "$nm" -name '*.o' -not -path '*/runtime/*' | wc -l | tr -d ' '; }

build 0
at0="$(count)"
[ "$at0" -ge 1 ] || fail "the -O0 build cached no objects"
build 2
at2="$(count)"
[ "$at2" -ge $((at0 * 2)) ] || fail "the -O2 build reused -O0 objects ($at0 objects after -O0, $at2 after -O2)"

echo "nmodular_opt_level_key OK — $at0 objects at -O0, $at2 after -O2: no object is shared across levels"
exit 0
