#!/usr/bin/env bash
# issue #120 — opt-in Perceus regions: P0 runtime arena gate harness.
#
# Compiles examples/perceus/region_arena.c against stage0/runtime.h and
# runs it, first with a plain build and then with AddressSanitizer +
# UndefinedBehaviorSanitizer. The fixture asserts the bump-arena
# invariants (see its header comment and docs/issue-120-regions-design.md).
# The ASAN run is the load-bearing half: it confirms the bulk free path
# touches no freed memory and has no use-after-free / overflow — the
# bug-class (#697/#703) that only ASAN catches.
#
# detect_leaks is intentionally NOT forced on: LeakSanitizer is
# unsupported on macOS/Darwin (the runner aborts with "detect_leaks is
# not supported on this platform"), and a chunk leak is already caught
# by the fixture's own arena-counter assertions, which are
# platform-independent. On Linux CI, LSan runs by default under
# -fsanitize=address and will flag a genuine chunk leak.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/examples/perceus/region_arena.c"
CC="${CC:-cc}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== arena-c-fixture: plain build =="
$CC -std=c11 -O2 -Wall -Wextra -Wno-unused-function -I"$ROOT" \
    "$SRC" -o "$TMP/region_arena"
"$TMP/region_arena"
echo "   plain ok"

echo "== arena-c-fixture: ASAN build (UAF / overflow detection) =="
$CC -std=c11 -fsanitize=address,undefined -g -O1 -Wall -Wextra \
    -Wno-unused-function -fno-omit-frame-pointer \
    -I"$ROOT" "$SRC" -o "$TMP/region_arena_asan"
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$TMP/region_arena_asan"
echo "   asan ok"

echo "arena-c-fixture passed"
