#!/bin/sh
# A read-only cache whose entries are older than the refresh window still hits:
# the mtime refresh is a hint, so a failed refresh must not recompile the runtime.

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
  echo "nmodular_readonly_cache SKIP — kaic2 has no libLLVM native backend"
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
log="$PROJ/cc.log"
cat > "$fake" <<EOT
#!/bin/sh
echo "\$*" >> "$log"
exec "$real_cc" "\$@"
EOT
chmod +x "$fake"

build() {
  env CC="$fake" KAI_NATIVE_MODULAR_CACHE_DIR="$nmroot" KAI_CC_ID_CACHE_DIR="$PROJ/ccid" \
    KAI_CORE_CACHE_DIR="$PROJ/core-cache" KAI_BACKEND=native \
    "$KAI" build "$PROJ/main.kai" -o "$PROJ/out" 2>"$PROJ/build.err" 1>/dev/null \
    || { echo "nmodular_readonly_cache FAIL — build failed"; cat "$PROJ/build.err"; exit 1; }
  [ "$("$PROJ/out")" = "2" ] || { echo "nmodular_readonly_cache FAIL — wrong output"; exit 1; }
}

build
[ -d "$PROJ/ccid" ] || { echo "nmodular_readonly_cache FAIL — no cc-id entry"; exit 1; }
find "$nmroot" "$PROJ/ccid" "$PROJ/core-cache" -type f -exec touch -t 200001010000 {} +
# chmod does not stop an owner from refreshing an mtime; an immutable flag or a read-only mount does.
lock() { chflags -R uchg "$nmroot" "$PROJ/ccid" "$PROJ/core-cache"; }
unlock() { chflags -R nouchg "$PROJ" 2>/dev/null || true; }
if command -v chflags >/dev/null 2>&1; then
  lock
  trap 'unlock; rm -rf "$PROJ"' EXIT INT TERM
elif sudo -n true 2>/dev/null; then
  sudo chown -R root "$nmroot" "$PROJ/ccid" "$PROJ/core-cache"
  trap 'sudo rm -rf "$PROJ"' EXIT INT TERM
else
  if [ -n "${CI:-}" ]; then
    echo "nmodular_readonly_cache FAIL — CI has no way to make the cache read-only"; exit 1
  fi
  echo "nmodular_readonly_cache SKIP — no way to make an owned cache refuse mtime updates"
  exit 0
fi
: > "$log"
build
if grep -q -- ' -c ' "$log"; then
  echo "nmodular_readonly_cache FAIL — a read-only cache recompiled C"; cat "$log"; exit 1
fi

echo "nmodular_readonly_cache OK — aged entries in a read-only cache still hit"
exit 0
