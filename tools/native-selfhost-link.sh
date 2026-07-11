#!/bin/bash
# Link a native-compiled compiler object into an executable kaic2-native.
#
# The native backend compiles `stage2/main.kai` to a self-contained object
# (runtime bitcode linked in-process), but that object references the
# in-process libLLVM C-API prims as `kaix_core_llvm_*` /
# `kaix_core_native_ctx_*` / `kaix_core_native_di_*`. Those linkable
# forwarders live in `stage0/runtime_llvm.c` under `-DKAI_LLVM`, over the
# `static kai_<name>` bodies in `runtime.h`. So the link is:
#
#   object(main.o) + runtime_llvm.c[-DKAI_LLVM] + libLLVM -> kaic2-native
#
# Args: <main.o> <output-binary>. Env: LLVM_CONFIG (default llvm-config),
# CC (default cc). Exits non-zero on any step failure; prints the failing
# command's stderr. Native-only — never touches the C bootstrap.
set -eu

OBJ="${1:?usage: native-selfhost-link.sh <main.o> <out-binary>}"
OUT="${2:?usage: native-selfhost-link.sh <main.o> <out-binary>}"

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

LLVM_CONFIG="${LLVM_CONFIG:-llvm-config}"
CC="${CC:-cc}"
command -v "$LLVM_CONFIG" >/dev/null 2>&1 || {
  echo "native-selfhost-link: llvm-config not in PATH — cannot link the native compiler" >&2
  exit 1
}

SHIM_C="$ROOT/stage0/runtime_llvm_native_shim.c"
[ -f "$SHIM_C" ] || { echo "native-selfhost-link: missing $SHIM_C" >&2; exit 1; }
[ -f "$OBJ" ] || { echo "native-selfhost-link: missing object $OBJ" >&2; exit 1; }

# Compile the KAI_LLVM shim TU: it exports the kaix_core_llvm_* /
# native_ctx_* / native_di_* forwarders the object calls (over runtime.h's
# `static kai_*`). The bitcode-self-contained object already carries `main`
# + every other kaix_* (runtime_llvm.bc), so this TU adds ONLY the 88
# libLLVM prims — hence a dedicated file, not all of runtime_llvm.c.
CPP="$($LLVM_CONFIG --cflags)"
SHIM_O="$(dirname "$OUT")/native-selfhost-shim.o"
# shellcheck disable=SC2086
$CC -std=c99 -Wno-unused-function -O2 -DKAI_LLVM $CPP \
    -I "$ROOT/stage2" -I "$ROOT/stage0" -c "$SHIM_C" -o "$SHIM_O"

# Link the object + shim + libLLVM (dynamic dev; the same component set the
# stage2 Makefile links kaic2 against). `-lc++` on macOS, `-lstdc++` on Linux.
case "$(uname -s)" in Darwin) CXXLIB="-lc++" ;; *) CXXLIB="-lstdc++" ;; esac
LDF="$($LLVM_CONFIG --ldflags) \
     $($LLVM_CONFIG --libs passes core analysis target nativecodegen bitwriter) \
     $($LLVM_CONFIG --system-libs passes core analysis target nativecodegen bitwriter) \
     $CXXLIB"
# shellcheck disable=SC2086
$CC -O2 "$OBJ" "$SHIM_O" -o "$OUT" $LDF -lm -ldl
