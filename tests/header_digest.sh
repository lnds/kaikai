#!/bin/sh
# The C-modular cache keys on the runtime header through a digest kept
# across builds: a warm build reuses every object, and an edit that
# leaves the header's size alone still misses.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

lay="$work/root"
mkdir -p "$lay/stage2" "$lay/bin"
for e in "$ROOT"/*; do
  case "$(basename "$e")" in
    stage2|bin) ;;
    *) ln -s "$e" "$lay/$(basename "$e")" ;;
  esac
done
for e in "$ROOT"/stage2/*; do
  case "$(basename "$e")" in
    runtime.h|runtime) ;;
    *) ln -s "$e" "$lay/stage2/$(basename "$e")" ;;
  esac
done
cp "$ROOT/bin/kai" "$lay/bin/kai"
cp "$ROOT/stage2/runtime.h" "$lay/stage2/runtime.h"
cp -R "$ROOT/stage2/runtime" "$lay/stage2/runtime"
printf 'fn main() : Unit / Stdout = Stdout.print("hi")\n' > "$work/hello.kai"

build() {
  env KAI_BACKEND=c KAI_BIN_MEMO=0 KAI_MODULAR=1 KAI_MODULAR_STATS=1 KAI_THREADS=1 \
    KAI_MODULAR_CACHE_DIR="$work/cache" KAI_NATIVE_MODULAR_CACHE_DIR="$work/ncache" \
    "$lay/bin/kai" build "$work/hello.kai" -o "$work/hello" 2>&1 | sed -n 's/.*cache hits=[0-9]* compiled=\([0-9]*\).*/\1/p'
}

fail=0
expect() {
  if [ "$2" != "$3" ]; then
    echo "  FAIL $1: compiled '$2', expected '$3'"
    fail=$((fail + 1))
  fi
}

expect cold "$(build | sed 's/^[1-9][0-9]*$/some/')" some
sleep 3
expect warm "$(build)" 0

line="$(grep -n '[a-z]' "$lay/stage2/runtime.h" | sed -n '1p' | cut -d: -f1)"
sed -i.bak "${line}s/[a-z]/X/" "$lay/stage2/runtime.h"
rm -f "$lay/stage2/runtime.h.bak"
cmp -s "$ROOT/stage2/runtime.h" "$lay/stage2/runtime.h" && { echo "  FAIL edit did not change the header"; exit 1; }
[ "$(wc -c < "$ROOT/stage2/runtime.h")" -eq "$(wc -c < "$lay/stage2/runtime.h")" ] || { echo "  FAIL edit changed the size"; exit 1; }
sleep 3
edited="$(build)"
[ -n "$edited" ] && [ "$edited" != 0 ] || { echo "  FAIL same-size edit: nothing recompiled ('$edited')"; fail=$((fail + 1)); }

sleep 3
expect warm-edited "$(build)" 0

part="$lay/stage2/runtime/types.h"
line="$(grep -n '[a-z]' "$part" | sed -n '2p' | cut -d: -f1)"
sed -i.bak "${line}s/[a-z]/X/" "$part"
rm -f "$part.bak"
cmp -s "$ROOT/stage2/runtime/types.h" "$part" && { echo "  FAIL part edit did not change the header"; exit 1; }
sleep 3
edited="$(build)"
[ -n "$edited" ] && [ "$edited" != 0 ] || { echo "  FAIL same-size edit of a part: nothing recompiled ('$edited')"; fail=$((fail + 1)); }

[ "$fail" -eq 0 ] && echo "header digest: ok"
exit "$fail"
