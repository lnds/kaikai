#!/bin/sh
# ThinLTO on the modular C link engages only on clang with a linker that
# keeps an LTO cache, and it must not change what a program prints or what a
# warm rebuild produces:
#
#   - the same program built with KAI_LTO=0 and KAI_LTO=1 prints the same
#   - when the probe settles on a linker cache, the cached objects are
#     bitcode and a warm rebuild relinks to the same bytes as the cold one
#   - when it settles on none (gcc, a linker without a cache), the objects
#     are native and the build is the one KAI_LTO=0 makes
#
# Runs once with the default `cc` and once with `clang` when that is another compiler.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/main.kai" <<'EOF'
import math.real

unit m

fn loop(i: Int, n: Int, acc: Real<m>) : Real<m> =
  if i >= n { acc } else { loop(i + 1, n, acc + real.round(1.4<m>)) }

fn main() : Unit / Stdout = Stdout.print("#{loop(0, 1000, 0.0<m>)}")
EOF

build() {
  tag="$1"; lto="$2"; cc="$3"
  KAI_BIN_MEMO=0 KAI_MODULAR=1 KAI_LTO="$lto" CC="$cc" \
    KAI_MODULAR_CACHE_DIR="$PROJ/$tag.objs" KAI_LTO_CACHE_DIR="$PROJ/$tag.lto" \
    "$KAI" build --backend=c "$PROJ/main.kai" -o "$PROJ/$tag.bin" 2>"$PROJ/$tag.err" \
    || { echo "wrapper_modular_lto FAIL ($tag build)"; cat "$PROJ/$tag.err"; exit 1; }
}

# The linked code only: the file's bytes carry a per-link UUID and debug-map paths.
code_of() {
  if command -v otool >/dev/null 2>&1; then
    otool -tv "$1" | tail -n +2 | cksum
  else
    objdump -d "$1" | tail -n +3 | cksum
  fi
}

bitcode_objects() {
  n=0
  for o in "$PROJ/$1.objs"/*.o; do
    case "$(od -An -tx1 -N4 "$o" | tr -d ' ')" in
      4243c0de | dec0170b) n=$((n + 1)) ;;
    esac
  done
  echo "$n"
}

check() {
  label="$1"; cc="$2"
  build "$label-off" 0 "$cc"
  build "$label-cold" 1 "$cc"
  KAI_BIN_MEMO=0 KAI_MODULAR=1 KAI_MODULAR_STATS=1 KAI_LTO=1 CC="$cc" \
    KAI_MODULAR_CACHE_DIR="$PROJ/$label-cold.objs" KAI_LTO_CACHE_DIR="$PROJ/$label-cold.lto" \
    "$KAI" build --backend=c "$PROJ/main.kai" -o "$PROJ/$label-warm.bin" 2>"$PROJ/$label-warm.err"
  grep -q 'compiled=0' "$PROJ/$label-warm.err" \
    || { echo "wrapper_modular_lto FAIL ($label: warm rebuild recompiled)"; cat "$PROJ/$label-warm.err"; exit 1; }
  [ "$(code_of "$PROJ/$label-cold.bin")" = "$(code_of "$PROJ/$label-warm.bin")" ] \
    || { echo "wrapper_modular_lto FAIL ($label: warm rebuild differs from cold)"; exit 1; }
  want="$("$PROJ/$label-off.bin")"
  [ "$("$PROJ/$label-warm.bin")" = "$want" ] \
    || { echo "wrapper_modular_lto FAIL ($label: output differs from KAI_LTO=0)"; exit 1; }
  kind="$(cat "$PROJ/$label-cold.lto"/probe-* 2>/dev/null || echo none)"
  bc="$(bitcode_objects "$label-cold")"
  off_bc="$(bitcode_objects "$label-off")"
  [ "$off_bc" = 0 ] || { echo "wrapper_modular_lto FAIL ($label: KAI_LTO=0 left bitcode objects)"; exit 1; }
  if [ -n "${KAI_EXPECT_LTO:-}" ] && [ "$label" = clang ] && [ "$kind" = none ]; then
    echo "wrapper_modular_lto FAIL (clang found no linker LTO cache; install lld)"; exit 1
  fi
  if [ "$kind" = none ]; then
    [ "$bc" = 0 ] || { echo "wrapper_modular_lto FAIL ($label: no LTO cache but $bc bitcode objects)"; exit 1; }
  else
    [ "$bc" -gt 0 ] || { echo "wrapper_modular_lto FAIL ($label: LTO via $kind produced no bitcode objects)"; exit 1; }
  fi
  echo "wrapper_modular_lto OK ($label: lto=$kind, $bc bitcode objects, output equal)"
}

check default cc
if command -v clang >/dev/null 2>&1 && [ "$(command -v clang)" != "$(command -v cc)" ]; then
  check clang clang
fi
