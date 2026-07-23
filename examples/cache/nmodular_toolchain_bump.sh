#!/bin/sh
# Native-modular .o cache toolchain-identity gate. The partition key inside
# kaic2 hashes only the projected KIR, so a rebuilt compiler whose codegen
# changed while the KIR stayed identical would serve stale objects from the
# per-project cache dir. The wrapper closes that hole by folding the kaic2
# toolchain id (mtime+size, the core cache's build id) into the cache-dir
# key. This gate builds a two-module native project twice (cold populates,
# warm reuses without rewriting any .o), then perturbs the toolchain id
# (touch kaic2) and asserts the third build MISSES into a fresh cache dir
# instead of reusing the old objects.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
KAIC2="$ROOT/stage2/kaic2"
PROJ="$(mktemp -d)"
restore_mtime() { [ -f "$PROJ/kaic2.mtime.ref" ] && touch -r "$PROJ/kaic2.mtime.ref" "$KAIC2" 2>/dev/null; rm -rf "$PROJ"; }
trap 'restore_mtime' EXIT INT TERM

# Native-modular only engages on a native-capable kaic2.
native_capable=0
if [ -x "$KAIC2" ]; then
  printf 'fn main() : Unit = ()\n' > "$PROJ/probe.kai"
  "$KAIC2" --emit=native --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/probe.kai" \
    >/dev/null 2>"$PROJ/native-probe.err" || true
  grep -q "not built into this compiler" "$PROJ/native-probe.err" 2>/dev/null || native_capable=1
fi
if [ "$native_capable" != "1" ]; then
  echo "nmodular_toolchain_bump SKIP — kaic2 has no libLLVM native backend"
  exit 0
fi

# A local import is what routes bin/kai onto the native-modular path.
cat > "$PROJ/dom.kai" <<'EOF'
#[derive(Show)]
pub type Mov = { id: Int, glosa: String }
EOF
cat > "$PROJ/main.kai" <<'EOF'
import dom

fn main() {
  let ms = [dom.Mov { id: 1, glosa: "uno" }]
  println("#{ms}")
}
EOF

nmroot="$PROJ/nmcache"
out="$PROJ/out"

build() {
  KAI_NATIVE_MODULAR_CACHE_DIR="$nmroot" KAI_CORE_CACHE_DIR="$PROJ/core-cache" \
    KAI_BACKEND=native "$KAI" build "$PROJ/main.kai" -o "$out" 2>"$PROJ/$1.err" 1>/dev/null \
    || { echo "nmodular_toolchain_bump FAIL ($1) — build failed"; cat "$PROJ/$1.err"; exit 1; }
  got="$("$out")"
  [ "$got" = "[Mov { id: 1, glosa: uno }]" ] \
    || { echo "nmodular_toolchain_bump FAIL ($1) — wrong output: '$got'"; exit 1; }
}

build cold
dirs1=$(find "$nmroot" -mindepth 1 -maxdepth 1 -type d -not -name runtime | wc -l | tr -d ' ')
objs1=$(find "$nmroot" -name '*.o' -not -path '*/runtime/*' | wc -l | tr -d ' ')
[ "$dirs1" = "1" ] || { echo "nmodular_toolchain_bump FAIL — cold build made $dirs1 cache dirs (want 1)"; exit 1; }
[ "$objs1" -ge 2 ] || { echo "nmodular_toolchain_bump FAIL — cold build cached $objs1 objects (want >= 2)"; exit 1; }

touch "$PROJ/stamp"
build warm
rewritten=$(find "$nmroot" -name '*.o' -not -path '*/runtime/*' -newer "$PROJ/stamp" | wc -l | tr -d ' ')
dirs2=$(find "$nmroot" -mindepth 1 -maxdepth 1 -type d -not -name runtime | wc -l | tr -d ' ')
[ "$dirs2" = "1" ] || { echo "nmodular_toolchain_bump FAIL — warm build changed the cache-dir key ($dirs2 dirs)"; exit 1; }
[ "$rewritten" = "0" ] || { echo "nmodular_toolchain_bump FAIL — warm build rewrote $rewritten objects (want 0: all hits)"; exit 1; }

# Perturb the toolchain id: a new kaic2 mtime is a new build id. The ref
# file preserves the original mtime for restore on exit.
touch -r "$KAIC2" "$PROJ/kaic2.mtime.ref"
touch "$KAIC2"
touch "$PROJ/stamp2"
build bumped
touch -r "$PROJ/kaic2.mtime.ref" "$KAIC2"
dirs3=$(find "$nmroot" -mindepth 1 -maxdepth 1 -type d -not -name runtime | wc -l | tr -d ' ')
fresh=$(find "$nmroot" -name '*.o' -not -path '*/runtime/*' -newer "$PROJ/stamp2" | wc -l | tr -d ' ')
[ "$dirs3" = "2" ] || { echo "nmodular_toolchain_bump FAIL — toolchain bump did not rotate the cache dir ($dirs3 dirs, want 2: stale objects would be reused)"; exit 1; }
[ "$fresh" -ge 2 ] || { echo "nmodular_toolchain_bump FAIL — bumped build re-emitted only $fresh objects (want >= 2: a full MISS)"; exit 1; }

echo "nmodular_toolchain_bump OK — cold populate, warm all-hit, toolchain bump full-miss into a fresh dir"
exit 0
