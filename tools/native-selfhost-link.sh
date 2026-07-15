#!/bin/bash
# Link a native-compiled compiler object into an executable kaic2-native.
#
# The native backend compiles `stage2/main.kai` to an object that merges the
# hot runtime bitcode in-process. Since the #1234 hot/owner split that bitcode
# is LEAF-ONLY (value/RC/arithmetic, no scheduler, no `main`, no runtime state),
# so the object references `main`, the singletons, the scheduler, and every
# non-leaf `kaix_*` as EXTERNAL — they come from the cc-compiled runtime OWNER
# object, exactly as `bin/kai`'s native link resolves them. On top of that the
# object also needs the in-process libLLVM C-API prims (`kaix_core_llvm_*` /
# `kaix_core_native_ctx_*` / `kaix_core_native_di_*`), which live ONLY under
# `-DKAI_LLVM` and so are carried by a dedicated shim TU. So the link is:
#
#   main.o + runtime_llvm.c[owner,-O0] + shim[-DKAI_LLVM] + libLLVM -> kaic2-native
#
# The owner is pinned to -O0 (issue #1234): clang -O1+ hoists the thread pointer
# across swapcontext in the scheduler, and CC here is clang.
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
# `static kai_*`). Compiled -DKAI_SEPARATE_COMPILATION so its view of the
# runtime state globals is `external` (owned by the runtime owner object
# below), matching the leaf bitcode merged into main.o — one shared copy, no
# duplicate/private singletons. This TU carries ONLY the 88 libLLVM prims.
CPP="$($LLVM_CONFIG --cflags)"
SHIM_O="$(dirname "$OUT")/native-selfhost-shim.o"
# shellcheck disable=SC2086
$CC -std=c99 -Wno-unused-function -O2 -DKAI_LLVM -DKAI_SEPARATE_COMPILATION=1 $CPP \
    -I "$ROOT/stage2" -I "$ROOT/stage0" -c "$SHIM_C" -o "$SHIM_O"

# Compile the runtime OWNER object: runtime_llvm.c with the state globals +
# main + scheduler DEFINED (KAI_RUNTIME_OWNER), the counterpart to the leaf
# bitcode merged into main.o. NO -DKAI_LLVM (the libLLVM prims are the shim's
# job), and -O0 so the scheduler's swapcontext-crossing code is not
# thread-pointer-hoisted under clang (issue #1234).
OWNER_O="$(dirname "$OUT")/native-selfhost-owner.o"
# shellcheck disable=SC2086
$CC -std=c99 -Wno-unused-function -O0 \
    -DKAI_SEPARATE_COMPILATION=1 -DKAI_RUNTIME_OWNER=1 \
    -I "$ROOT/stage2" -I "$ROOT/stage0" -c "$ROOT/stage0/runtime_llvm.c" -o "$OWNER_O"

# Link the object + shim + libLLVM (dynamic dev; the same component set the
# stage2 Makefile links kaic2 against). `-lc++` on macOS, `-lstdc++` on Linux.
case "$(uname -s)" in Darwin) CXXLIB="-lc++" ;; *) CXXLIB="-lstdc++" ;; esac
LDF="$($LLVM_CONFIG --ldflags) \
     $($LLVM_CONFIG --libs passes core analysis target nativecodegen bitwriter) \
     $($LLVM_CONFIG --system-libs passes core analysis target nativecodegen bitwriter) \
     $CXXLIB"
# shellcheck disable=SC2086
$CC -O2 "$OBJ" "$OWNER_O" "$SHIM_O" -o "$OUT" $LDF -lm -ldl
