#!/bin/sh
# Install clang, and libLLVM when called with `llvm`, on a CI runner.
# A runner image that already carries them skips apt entirely; otherwise
# every apt call is bounded and retried, so a wedged mirror fails this step
# by name in minutes instead of burning the job ceiling as `cancelled`.
set -eu

want_llvm=0
[ "${1:-}" = llvm ] && want_llvm=1

find_llvm_config() {
  command -v llvm-config 2>/dev/null || ls /usr/bin/llvm-config-* 2>/dev/null | sort -V | tail -1
}

major() { sed -n 's/[^0-9]*\([0-9][0-9]*\)\..*/\1/p' | head -1; }

# libLLVM must not be older than clang: it reads the bitcode clang emits.
llvm_usable() {
  lc=$(find_llvm_config) || return 1
  [ -n "$lc" ] || return 1
  [ -f "$("$lc" --includedir)/llvm-c/Core.h" ] || return 1
  ls "$("$lc" --libdir)"/libLLVM*.so >/dev/null 2>&1 || return 1
  [ "$("$lc" --version | major)" = "$(clang --version | major)" ]
}

have_toolchain() {
  command -v clang >/dev/null 2>&1 || return 1
  [ "$want_llvm" = 0 ] || llvm_usable
}

apt_retry() {
  for attempt in 1 2 3; do
    if sudo timeout 180 apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 "$@"; then
      return 0
    fi
    echo "apt-get $1: attempt $attempt failed" >&2
    sleep 10
  done
  return 1
}

if have_toolchain; then
  echo "toolchain already on the runner; skipping apt"
else
  pkgs=clang
  [ "$want_llvm" = 0 ] || pkgs="clang llvm-dev"
  apt_retry update
  # shellcheck disable=SC2086
  apt_retry install -y --no-install-recommends $pkgs
fi

clang --version | head -1
if [ "$want_llvm" = 1 ]; then
  lc=$(find_llvm_config)
  if [ -z "$lc" ]; then
    echo "::error::no llvm-config found"
    exit 1
  fi
  echo "LLVM_CONFIG=$lc" >> "$GITHUB_ENV"
  echo "using $lc ($("$lc" --version))"
fi
