#!/bin/sh
# A native link uses the cached core objects the build already holds: trimming
# every one of them between the emit and the link does not fail the build.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
"$ROOT/tools/kaic2-native-capable.sh" "$ROOT/stage2/kaic2" > /dev/null 2>&1 || {
  echo "native-cache-hold SKIP (kaic2 has no native backend)"
  exit 0
}
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM
XDG="$WORK/xdg"
mkdir -p "$WORK/p"
printf 'fn main() : Unit / Console = println("hold-ok")\n' > "$WORK/p/main.kai"
cd "$WORK/p"

XDG_CACHE_HOME="$XDG" KAI_BACKEND=native "$KAI" build main.kai -o "$WORK/warm" > "$WORK/warm.log" 2>&1 || {
  echo "native-cache-hold FAIL (warm build)"; cat "$WORK/warm.log"; exit 1
}
[ -n "$(find "$XDG/kai/core" -name 'ncore-*.o')" ] || { echo "native-cache-hold FAIL (no cached core object)"; exit 1; }

XDG_CACHE_HOME="$XDG" KAI_BACKEND=native \
  KAI_PRELINK_HOOK="find '$XDG/kai/core' -name 'ncore-*.o' -delete" \
  "$KAI" build main.kai -o "$WORK/out" > "$WORK/out.log" 2>&1 || {
  echo "native-cache-hold FAIL (link after trim)"; cat "$WORK/out.log"; exit 1
}
[ "$("$WORK/out")" = "hold-ok" ] || { echo "native-cache-hold FAIL (wrong output)"; exit 1; }
echo "native-cache-hold OK"
