#!/bin/sh
# Install clang, and libLLVM when called with `llvm`, on a CI runner.
# A runner image that already carries clang skips apt for the C-only case;
# every apt call is bounded and retried, so a wedged mirror fails this step
# by name in minutes instead of burning the job ceiling as `cancelled`.
set -eu

want_llvm=0
[ "${1:-}" = llvm ] && want_llvm=1

find_llvm_config() {
  command -v llvm-config 2>/dev/null || ls /usr/bin/llvm-config-* 2>/dev/null | sort -V | tail -1
}

# The native case always installs llvm-dev: the native gates depend on what
# that package provides, and the LLVM the runner image carries fails them.
have_toolchain() {
  [ "$want_llvm" = 0 ] && command -v clang >/dev/null 2>&1
}

apt_retry() {
  for attempt in 1 2 3; do
    if sudo timeout 180 apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 "$@"; then
      return 0
    fi
    echo "apt-get $1: attempt $attempt failed" >&2
    # A timeout can kill dpkg mid-install; the next attempt refuses to run
    # until the interrupted configuration is finished.
    sudo timeout 180 dpkg --configure -a || true
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
