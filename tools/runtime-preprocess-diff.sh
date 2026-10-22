#!/bin/sh
# tools/runtime-preprocess-diff.sh — does a change to stage2/runtime.h leave
# the runtime every existing platform compiles as it was?
#
#   runtime-preprocess-diff.sh [<base commit>]
#
# Preprocesses the runtime as it is at <base commit> (default: the merge base
# with origin/main) and as it is in the working tree, with `$CC -E -P`, in
# every shape a build instantiates it, and compares the two texts with blank
# lines removed. Equal text is equal input to the compiler proper, so the
# object code cannot differ: a change that passes needs no runtime or
# compile-cost measurement on the host it ran on. The text depends on the
# host's libc headers, so the claim holds per platform and the check runs on
# each.
#
# One line per shape: its name, the line count and the digest of the text.
# Exit 0 when every shape is identical, 1 on the first difference (with the
# diff), 2 on a usage or setup error.

set -eu
LC_ALL=C
export LC_ALL

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-cc}"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum; else shasum -a 256; fi
}

cd "$ROOT"
if [ $# -ge 1 ]; then
  base="$(git rev-parse --verify --quiet "$1^{commit}")" || {
    echo "runtime-preprocess-diff: $1 is not a commit in this clone" >&2
    exit 2
  }
else
  base="$(git merge-base origin/main HEAD 2>/dev/null)" || {
    echo "runtime-preprocess-diff: no merge base with origin/main" >&2
    exit 2
  }
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/kai-rt-pp.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM
mkdir -p "$work/base/stage0" "$work/base/stage2"
for f in stage2/runtime.h stage2/runtime_owner_c.c stage0/runtime_llvm.c $(git ls-tree -r --name-only "$base" stage2/runtime/); do
  mkdir -p "$work/base/$(dirname "$f")"
  git show "$base:$f" > "$work/base/$f"
done

# preprocess <tree> <tu> <flags...>: the tree's own stage2 leads the include
# path, as in every build.
preprocess() {
  tree="$1"; tu="$2"; shift 2
  "$CC" -std=c99 -E -P -I "$tree/stage2" -I "$tree/stage0" "$@" "$tree/$tu" | sed 's/^[[:space:]]*//' | grep -v '^$'
}

SEP="-DKAI_SEPARATE_COMPILATION=1"
OWNER="$SEP -DKAI_RUNTIME_OWNER=1"

status=0
# compare <name> <tu> <flags>
compare() {
  name="$1"; tu="$2"; flags="$3"
  # shellcheck disable=SC2086
  preprocess "$work/base" "$tu" $flags > "$work/before.i"
  # shellcheck disable=SC2086
  preprocess "$ROOT" "$tu" $flags > "$work/after.i"
  if cmp -s "$work/before.i" "$work/after.i"; then
    printf 'identical  %-16s %7d lines  %s\n' "$name" "$(wc -l < "$work/after.i")" \
      "$(sha256 < "$work/after.i" | cut -c1-16)"
  else
    printf 'DIFFERS    %s\n' "$name"
    diff "$work/before.i" "$work/after.i" | head -40
    status=1
  fi
}

echo "runtime-preprocess-diff: $(git rev-parse --short "$base") against the working tree, $CC on $(uname -sm)"
compare single-tu     stage2/runtime_owner_c.c ""
compare c-owner       stage2/runtime_owner_c.c "$OWNER -DKAI_PROGRAM_PROVIDES_MAIN=1"
compare c-module      stage2/runtime_owner_c.c "$SEP"
compare native-owner  stage0/runtime_llvm.c    "$OWNER"
compare native-hot    stage0/runtime_llvm.c    "$SEP -DKAI_HOT_ONLY=1"
compare rc-ledger     stage2/runtime_owner_c.c "-DKAI_TRACE_RC=1"
compare sized-main    stage2/runtime_owner_c.c "-DKAI_MAIN_STACK_SIZE=67108864"
exit "$status"
