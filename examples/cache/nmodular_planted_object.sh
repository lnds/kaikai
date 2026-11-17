#!/bin/sh
# A native object planted under a cached object's name is a miss. A cached
# object is trusted only with the checksum recorded when it was emitted, so an
# object another build wrote, or one whose record is gone or stale, is rebuilt.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

fail() { echo "nmodular_planted_object FAIL — $1"; exit 1; }

native_capable=0
if [ -x "$KAIC2" ]; then
  printf 'fn main() : Unit = ()\n' > "$PROJ/probe.kai"
  "$KAIC2" $EDITION_FLAG --emit=native --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/probe.kai" \
    >/dev/null 2>"$PROJ/native-probe.err" || true
  grep -q "not built into this compiler" "$PROJ/native-probe.err" 2>/dev/null || native_capable=1
fi
if [ "$native_capable" != "1" ]; then
  echo "nmodular_planted_object SKIP — kaic2 has no libLLVM native backend"
  exit 0
fi

mkproj() {
  mkdir -p "$PROJ/$1"
  cat > "$PROJ/$1/dom.kai" <<'EOF'
#[derive(Show)]
pub type Mov = { id: Int, glosa: String }
EOF
  cat > "$PROJ/$1/main.kai" <<EOF
import dom

fn main() {
  let ms = [dom.Mov { id: 1, glosa: "$2" }]
  println("#{ms}")
}
EOF
}
mkproj one uno
mkproj two dos

build() {
  KAI_NATIVE_MODULAR_CACHE_DIR="$PROJ/nm-$1" KAI_CORE_CACHE_DIR="$PROJ/core-$1" KAI_BIN_MEMO=0 \
    KAI_BACKEND=native "$KAI" build "$PROJ/$1/main.kai" -o "$PROJ/out-$1" 2>"$PROJ/$1.err" 1>/dev/null \
    || { cat "$PROJ/$1.err"; fail "$1: build failed"; }
  "$PROJ/out-$1"
}
objs() { find "$PROJ/nm-$1" -name '*.o' -not -path '*/runtime/*' | sort; }

[ "$(build one)" = "[Mov { id: 1, glosa: uno }]" ] || fail "one: cold build printed the wrong output"
[ "$(build two)" = "[Mov { id: 1, glosa: dos }]" ] || fail "two: cold build printed the wrong output"

mine="$(grep -l uno $(objs one) | head -1 || true)"
theirs="$(grep -l dos $(objs two) | head -1 || true)"
[ -n "$mine" ] && [ -n "$theirs" ] || fail "found no partition holding the program's own string"

plant() {
  cp "$theirs" "$mine"
  if [ "$1" = "drop" ]; then rm -f "$mine.sum"; fi
  got="$(build one)" || true
  [ "$got" = "[Mov { id: 1, glosa: uno }]" ] || fail "$1: a planted object was linked (printed '$got')"
}
plant drop
plant stale

echo "nmodular_planted_object OK — a planted object with no record and with a stale record were rebuilt"
exit 0
