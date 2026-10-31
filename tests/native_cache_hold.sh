#!/bin/sh
# A native link uses the cached objects the build already holds: trimming every
# one of them between the emit and the link does not fail the build, and the
# build leaves no hold dir behind. Whole-program and per-module paths.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
"$ROOT/tools/kaic2-native-capable.sh" "$ROOT/stage2/kaic2" > /dev/null 2>&1 || {
  echo "native-cache-hold SKIP (kaic2 has no native backend)"
  exit 0
}
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

# check <name> <modular 0|1> <store under kai/ whose objects the hook trims>
check() {
  name="$1"; modular="$2"; store="$3"
  xdg="$WORK/xdg-$name"
  mkdir -p "$WORK/$name"
  printf 'fn main() : Unit / Console = println("hold-ok")\n' > "$WORK/$name/main.kai"
  cd "$WORK/$name"
  XDG_CACHE_HOME="$xdg" KAI_BACKEND=native KAI_NATIVE_MODULAR="$modular" \
    "$KAI" build main.kai -o "$WORK/warm-$name" > "$WORK/warm-$name.log" 2>&1 || {
    echo "native-cache-hold FAIL ($name: warm build)"; cat "$WORK/warm-$name.log"; exit 1
  }
  [ -n "$(find "$xdg/kai/$store" -name '*.o')" ] || { echo "native-cache-hold FAIL ($name: no cached object)"; exit 1; }
  XDG_CACHE_HOME="$xdg" KAI_BACKEND=native KAI_NATIVE_MODULAR="$modular" \
    KAI_PRELINK_HOOK="find '$xdg/kai/$store' -name '*.o' -delete" \
    "$KAI" build main.kai -o "$WORK/out-$name" > "$WORK/out-$name.log" 2>&1 || {
    echo "native-cache-hold FAIL ($name: link after trim)"; cat "$WORK/out-$name.log"; exit 1
  }
  [ "$("$WORK/out-$name")" = "hold-ok" ] || { echo "native-cache-hold FAIL ($name: wrong output)"; exit 1; }
  [ -z "$(find "$xdg/kai/holds" -mindepth 1 2> /dev/null)" ] || { echo "native-cache-hold FAIL ($name: hold dir left behind)"; exit 1; }
}

check whole 0 core
check modular 1 nmodular
echo "native-cache-hold OK"
