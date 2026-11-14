#!/bin/sh
# A native build whose hold fails retries linking only objects it owns:
# trimming the whole cache before the hold, and again before the link, does
# not fail the build. Whole-program and per-module paths.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
"$ROOT/tools/kaic2-native-capable.sh" "$ROOT/stage2/kaic2" > /dev/null 2>&1 || {
  echo "native-cache-retry SKIP (kaic2 has no native backend)"
  exit 0
}
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

# check <name> <modular 0|1>
check() {
  name="$1"; modular="$2"
  xdg="$WORK/xdg-$name"
  mkdir -p "$WORK/$name"
  printf 'fn main() : Unit / Console = println("retry-ok")\n' > "$WORK/$name/main.kai"
  cd "$WORK/$name"
  trim="find '$xdg/kai' -name '*.o' -delete"
  XDG_CACHE_HOME="$xdg" KAI_BACKEND=native KAI_NATIVE_MODULAR="$modular" \
    "$KAI" build main.kai -o "$WORK/warm-$name" > "$WORK/warm-$name.log" 2>&1 || {
    echo "native-cache-retry FAIL ($name: warm build)"; cat "$WORK/warm-$name.log"; exit 1
  }
  XDG_CACHE_HOME="$xdg" KAI_BACKEND=native KAI_NATIVE_MODULAR="$modular" \
    KAI_PREHOLD_HOOK="$trim" KAI_PRELINK_HOOK="$trim" \
    "$KAI" build main.kai -o "$WORK/out-$name" > "$WORK/out-$name.log" 2>&1 || {
    echo "native-cache-retry FAIL ($name: link after retry)"; cat "$WORK/out-$name.log"; exit 1
  }
  [ "$("$WORK/out-$name")" = "retry-ok" ] || { echo "native-cache-retry FAIL ($name: wrong output)"; exit 1; }
}

check whole 0
check modular 1
echo "native-cache-retry OK"
