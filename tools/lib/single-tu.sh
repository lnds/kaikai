#!/usr/bin/env bash
# Single-TU C build for the M:N gates. Source it for `kai_build_single_tu`.
#
#   kai_build_single_tu <src.kai> <out-bin>
#
# Compiles kaic2-emitted C as ONE translation unit at -O2, with no separate
# scheduler owner object: the scheduler inlines into the program's own frames.
# The `stage2` Makefile recipes compile the emitted C exactly the way this
# does, and work-stealing races whose only symptom is a rotated fiber identity
# have surfaced on this shape when `bin/kai`'s split builds did not show them.
#
# `-O2` is load-bearing, not a default: at `-O0` the compiler does not keep a
# thread-local address across `swapcontext` and the window closes.
#
# CC picks the compiler; KAI_SINGLE_TU_CFLAGS overrides the flags (keep -O2).

KAI_SINGLE_TU_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
KAI_SINGLE_TU_CFLAGS="${KAI_SINGLE_TU_CFLAGS:--std=c99 -w -g -O2}"

kai_build_single_tu() {
  local src="$1" out="$2"
  local root="$KAI_SINGLE_TU_ROOT"
  local kaic2="$root/stage2/kaic2"
  local edition_flag="--edition $(cat "$root/EDITION")"
  local stdlib="$root/stdlib"

  [ -x "$kaic2" ] || { echo "kai_build_single_tu: $kaic2 missing — run 'make kaic2'" >&2; return 1; }
  "$kaic2" $edition_flag --path "$stdlib" "$src" > "$out.c" || return 1
  # shellcheck disable=SC2086
  ${CC:-cc} $KAI_SINGLE_TU_CFLAGS \
    -I "$root/stage2" -DKAI_STDLIB_PATH="\"$stdlib\"" -I "$root/stage0" \
    "$out.c" -o "$out" -lm
}
