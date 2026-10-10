#!/bin/sh
# Install the C toolchain on a CI runner.
#
#   (no argument)  clang, for the C backend
#   llvm           clang plus the libLLVM major mk/llvm.mk pins, with its
#                  clang and llvm-* tools; exports LLVM_CONFIG
#   bitcode        only that major's clang and llvm-* tools, for a build
#                  that brings its own libLLVM
#
# A runner image that already carries clang skips apt for the C-only case;
# every apt call is bounded and retried, so a wedged mirror fails this step
# by name in minutes instead of burning the job ceiling as `cancelled`.
set -eu

mode="${1:-}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
major="$(sed -n 's/^LLVM_VERSION ?= *\([0-9]*\)\..*/\1/p' "$ROOT/mk/llvm.mk")"

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

# The distribution's own archive rarely carries the pinned major.
add_llvm_apt_source() {
  apt-cache show "clang-$major" >/dev/null 2>&1 && return 0
  codename="$(. /etc/os-release && echo "$VERSION_CODENAME")"
  sudo install -d -m 0755 /etc/apt/keyrings
  curl -fsSL --retry 3 --max-time 60 https://apt.llvm.org/llvm-snapshot.gpg.key \
    | sudo tee /etc/apt/keyrings/apt.llvm.org.asc >/dev/null
  echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] https://apt.llvm.org/$codename/ llvm-toolchain-$codename-$major main" \
    | sudo tee "/etc/apt/sources.list.d/llvm-$major.list" >/dev/null
}

case "$mode" in
  "")
    if command -v clang >/dev/null 2>&1; then
      echo "toolchain already on the runner; skipping apt"
    else
      apt_retry update
      apt_retry install -y --no-install-recommends clang
    fi
    clang --version | head -1
    ;;
  llvm|bitcode)
    [ -n "$major" ] || { echo "::error::no LLVM_VERSION in mk/llvm.mk"; exit 1; }
    pkgs="clang-$major llvm-$major"
    [ "$mode" = bitcode ] || pkgs="clang clang-$major llvm-$major-dev"
    add_llvm_apt_source
    apt_retry update
    # shellcheck disable=SC2086
    apt_retry install -y --no-install-recommends $pkgs
    "clang-$major" --version | head -1
    if [ "$mode" = llvm ]; then
      clang --version | head -1
      lc="/usr/bin/llvm-config-$major"
      have="$("$lc" --version 2>/dev/null || true)"
      case "$have" in
        "$major".*) ;;
        *) echo "::error::$lc answers '$have', expected LLVM $major"; exit 1 ;;
      esac
      echo "LLVM_CONFIG=$lc" >> "$GITHUB_ENV"
      echo "using $lc ($have)"
    fi
    ;;
  *)
    echo "usage: ci-install-toolchain.sh [llvm|bitcode]" >&2
    exit 2
    ;;
esac
