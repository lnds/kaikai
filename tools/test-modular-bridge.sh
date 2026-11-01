#!/bin/sh
# The modular C build of a program that calls the libLLVM forwarders (the
# compiler's own shape) links against the stubs, with no libLLVM and no
# CFLAGS help: `kai build` adds the bridge define to the units that need it.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/main.kai" <<'EOF'
fn main() {
  print("tag=[#{llvm_backend_tag()}]")
}
EOF

if ! KAI_BACKEND=c KAI_MODULAR=1 KAI_CACHE=0 KAI_MODULAR_CACHE_DIR="$tmp/cache" \
     "$ROOT/bin/kai" build "$tmp/main.kai" -o "$tmp/prog" 2> "$tmp/build.err"; then
  echo "modular-bridge FAIL (build)"
  grep -E "error" "$tmp/build.err" | head -20
  exit 1
fi

got="$("$tmp/prog")"
if [ "$got" != "tag=[]" ]; then
  echo "modular-bridge FAIL (want tag=[], got $got)"
  exit 1
fi
echo "modular-bridge OK"
