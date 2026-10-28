#!/bin/sh
# tools/test-host-minimal.sh — the runtime on a host that provides no facility.
#
# stage2/runtime.h puts each facility a small host lacks behind a KAI_HOST_*
# macro. This builds the fixtures of examples/runtime/host_minimal_* on the
# host it runs on, with every macro at 0, and checks three things:
#
#   1. A program that needs no facility prints what its golden says, the same
#      as a default build: <name>.out.expected.
#   2. A program that reaches a facility stops with status 1 and the
#      diagnostic naming it, and nothing else on stderr: <name>.run.err.expected.
#   3. No binary of 1 or 2 imports a libc entry point of any facility, so
#      nothing is left for a host without them to stub or fail to link.
#
# Then it compiles the runtime with each facility switched off on its own, so
# the macros stay independent of one another.
#
# The C backend only. The native runtime is two halves: the owner object,
# compiled per build with $CFLAGS, and the hot bitcode, generated once when
# the compiler is built and merged into the emitted objects. The macros reach
# the first and not the second, and the two would then disagree on the layout
# of a fiber.
#
#   KAI=<wrapper>   default bin/kai
#   CC, CFLAGS      as the wrapper reads them; the macros are appended

set -eu
LC_ALL=C
export LC_ALL

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="${KAI:-$ROOT/bin/kai}"
CC="${CC:-cc}"
BASE_CFLAGS="${CFLAGS:--std=c99 -Wno-unused-function -Wno-unused-variable -O2}"
FIX="$ROOT/examples/runtime"

FACILITIES="FIBERS STACK_GUARD REACTOR SOCKETS PROCESS SIGNALS BACKTRACE UNWIND"
# A libc entry point of a facility, as `nm -u` prints an import of it.
IMPORTS='getcontext|makecontext|swapcontext|setcontext|mmap|mprotect|munmap|getrlimit|sigaltstack|sigaction|sigprocmask|pthread_sigmask|poll|pipe|socket|connect|bind|listen|accept|send|sendto|recv|recvfrom|getaddrinfo|freeaddrinfo|fork|execvp|waitpid|kill|backtrace|dladdr|popen|_Unwind_ForcedUnwind|_Unwind_GetCFA'

# off <facility>...: the flags that switch those facilities off.
off() {
  for f in "$@"; do printf ' -DKAI_HOST_%s=0' "$f"; done
}

work="$(mktemp -d "${TMPDIR:-/tmp}/kai-host-minimal.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

fail() {
  echo "host-minimal FAIL $*" >&2
  exit 1
}

# build <name> <binary> <cflags>
build() {
  CC="$CC" CFLAGS="$3" "$KAI" build --backend=c "$FIX/$1.kai" -o "$2" > "$work/build.log" 2>&1 \
    || { cat "$work/build.log" >&2; fail "$1: does not build"; }
}

# shellcheck disable=SC2086
MINIMAL="$BASE_CFLAGS -Werror=implicit-function-declaration$(off $FACILITIES)"

no_facility_import() {
  if nm -u "$2" | sed 's/^[[:space:]]*U*[[:space:]]*//' | grep -E "^_?($IMPORTS)(@|\\\$|\$)" > "$work/imports"; then
    cat "$work/imports" >&2
    fail "$1: the binary imports a facility's libc entry point"
  fi
}

for golden in "$FIX"/host_minimal_*.out.expected; do
  name="$(basename "$golden" .out.expected)"
  build "$name" "$work/$name.host" "$BASE_CFLAGS"
  build "$name" "$work/$name" "$MINIMAL"
  for bin in "$work/$name.host" "$work/$name"; do
    "$bin" > "$work/out" 2> "$work/err" || { cat "$work/err" >&2; fail "$name: $(basename "$bin") exits nonzero"; }
    diff "$golden" "$work/out" >&2 || fail "$name: $(basename "$bin") prints something else"
  done
  no_facility_import "$name" "$work/$name"
  echo "host-minimal OK $name (same output with no facility)"
done

for golden in "$FIX"/host_minimal_*.run.err.expected; do
  name="$(basename "$golden" .run.err.expected)"
  build "$name" "$work/$name" "$MINIMAL"
  rc=0
  "$work/$name" > "$work/out" 2> "$work/err" || rc=$?
  [ "$rc" -eq 1 ] || { cat "$work/err" >&2; fail "$name: exit $rc, expected 1"; }
  diff "$golden" "$work/err" >&2 || fail "$name: stderr is not the diagnostic"
  no_facility_import "$name" "$work/$name"
  echo "host-minimal OK $name ($(cat "$golden"))"
done

# Each facility off on its own, and the two groups that only go together.
OWNER="-DKAI_SEPARATE_COMPILATION=1 -DKAI_RUNTIME_OWNER=1"
alone() {
  # shellcheck disable=SC2086,SC2046
  "$CC" $BASE_CFLAGS -Werror=implicit-function-declaration -fsyntax-only \
      -I "$ROOT/stage2" -I "$ROOT/stage0" $OWNER $(off "$@") "$ROOT/stage0/runtime_llvm.c" \
      > "$work/alone.log" 2>&1 \
    || { cat "$work/alone.log" >&2; fail "the runtime does not compile without: $*"; }
}
for f in FIBERS SOCKETS PROCESS SIGNALS BACKTRACE UNWIND; do alone "$f"; done
alone FIBERS STACK_GUARD
alone SOCKETS PROCESS SIGNALS REACTOR
echo "host-minimal OK each facility switches off on its own"
