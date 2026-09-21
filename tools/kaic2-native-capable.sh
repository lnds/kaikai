#!/usr/bin/env bash
# kaic2-native-capable.sh — exit 0 iff the given kaic2 has the in-process
# native backend compiled in (a KAI_LLVM=1 build).
#
# Usage: tools/kaic2-native-capable.sh [path/to/kaic2]   (default stage2/kaic2)
#
# A C-only kaic2 cannot emit an object, so emitting a one-line program with
# --emit=native tells the two apart in well under a second.
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
kaic2="${1:-$root/stage2/kaic2}"
case "$kaic2" in /*) ;; *) kaic2="$(pwd)/$kaic2" ;; esac
[ -x "$kaic2" ] || exit 1

bc="$root/stage0/runtime_llvm.bc"
[ -f "$bc" ] || bc=""

dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT
printf 'fn main() : Unit / Console = println("probe")\n' > "$dir/probe.kai"
( cd "$dir" \
  && env KAI_NATIVE_RUNTIME_BC="$bc" \
       "$kaic2" --edition "$(cat "$root/EDITION")" --emit=native \
       --path "$root/stdlib" probe.kai >/dev/null 2>&1 ) || exit 1
[ -f "$dir/probe.o" ]
