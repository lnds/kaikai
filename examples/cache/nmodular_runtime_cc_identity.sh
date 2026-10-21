#!/bin/sh
# The cached runtime object is keyed by the C compiler's identity, not its name:
# two compilers answering to the same $CC but printing different --version must
# not share an object.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

native_capable=0
if [ -x "$KAIC2" ]; then
  printf 'fn main() : Unit = ()\n' > "$PROJ/probe.kai"
  "$KAIC2" $EDITION_FLAG --emit=native --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/probe.kai" \
    >/dev/null 2>"$PROJ/native-probe.err" || true
  grep -q "not built into this compiler" "$PROJ/native-probe.err" 2>/dev/null || native_capable=1
fi
if [ "$native_capable" != "1" ]; then
  echo "nmodular_runtime_cc_identity SKIP — kaic2 has no libLLVM native backend"
  exit 0
fi

cat > "$PROJ/dom.kai" <<'EOT'
pub fn two() : Int = 2
EOT
cat > "$PROJ/main.kai" <<'EOT'
import dom

fn main() = println("#{dom.two()}")
EOT

real_cc="$(command -v "${CC:-cc}")"
fake="$PROJ/fakecc"
nmroot="$PROJ/nmcache"

# The wrapper keeps its path (and so its name) across builds; only its identity changes.
install_cc() {
  cat > "$fake" <<EOT
#!/bin/sh
if [ "\$1" = "--version" ]; then echo "fakecc $1"; exit 0; fi
exec "$real_cc" "\$@"
EOT
  chmod +x "$fake"
}

build() {
  install_cc "$1"
  CC="$fake" KAI_NATIVE_MODULAR_CACHE_DIR="$nmroot" KAI_CORE_CACHE_DIR="$PROJ/core-cache" \
    KAI_BACKEND=native "$KAI" build "$PROJ/main.kai" -o "$PROJ/out" 2>"$PROJ/$1.err" 1>/dev/null \
    || { echo "nmodular_runtime_cc_identity FAIL ($1) — build failed"; cat "$PROJ/$1.err"; exit 1; }
  [ "$("$PROJ/out")" = "2" ] || { echo "nmodular_runtime_cc_identity FAIL ($1) — wrong output"; exit 1; }
}

runtime_objs() { find "$nmroot/runtime" -name '*.o' 2>/dev/null | wc -l | tr -d ' '; }

build one
n1=$(runtime_objs)
[ "$n1" -ge 1 ] || { echo "nmodular_runtime_cc_identity FAIL — no runtime object cached"; exit 1; }
build one
[ "$(runtime_objs)" = "$n1" ] || { echo "nmodular_runtime_cc_identity FAIL — same compiler missed the runtime cache"; exit 1; }
build two
[ "$(runtime_objs)" -gt "$n1" ] || { echo "nmodular_runtime_cc_identity FAIL — a different compiler reused the cached runtime object"; exit 1; }

echo "nmodular_runtime_cc_identity OK — same identity hits, different identity gets its own object"
exit 0
