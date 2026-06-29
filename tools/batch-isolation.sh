#!/bin/bash
# Batch-isolation gate (#962 Lane 4) — proves the `--batch-list` driver
# keeps separate inputs isolated: compiling N files in one process must
# produce, for each file, a `.c` byte-identical to compiling it alone.
#
# The adversarial pair examples/oracle/lane4/{file_a,file_b}.kai both
# declare a root `fn tag` with INCOMPATIBLE signatures. If the batch
# leaked one file's decls into the other's typecheck, the shared name
# would retype and the `.c` would diverge. The gate is byte-identity:
# batch_a == alone_a AND batch_b == alone_b.
#
# This is the standing regression for cross-file contamination. It is
# trivially green while the batch only shares the immutable core
# `PreludesLoaded` (today); it becomes load-bearing the moment a future
# lane shares the core TYPECHECK across files (the #962 ~20x lever),
# where a mutable shared TyEnv could leak.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KAIC2="${KAIC2:-$ROOT/stage2/kaic2}"
STDLIB="${STDLIB:-$ROOT/stdlib}"
A="$ROOT/examples/oracle/lane4/file_a.kai"
B="$ROOT/examples/oracle/lane4/file_b.kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

if [ ! -x "$KAIC2" ]; then
  echo "batch-isolation: compiler not found at $KAIC2 (build it: make kaic2)" >&2
  exit 2
fi

# Standalone .c (the reference). kaic2 prints the .c to stdout.
"$KAIC2" --path "$STDLIB" "$A" > "$WORK/a_alone.c" 2>/dev/null
"$KAIC2" --path "$STDLIB" "$B" > "$WORK/b_alone.c" 2>/dev/null

# Batch .c (both in one process).
printf '%s\t%s\n%s\t%s\n' "$A" "$WORK/a_batch.c" "$B" "$WORK/b_batch.c" > "$WORK/iso.list"
"$KAIC2" --path "$STDLIB" --batch-list "$WORK/iso.list" >/dev/null 2>&1

fail=0
if cmp -s "$WORK/a_alone.c" "$WORK/a_batch.c"; then
  echo "batch-isolation OK file_a — batch .c byte-identical to standalone"
else
  echo "batch-isolation RED file_a — batch .c diverged from standalone (cross-file leak)"
  fail=1
fi
if cmp -s "$WORK/b_alone.c" "$WORK/b_batch.c"; then
  echo "batch-isolation OK file_b — batch .c byte-identical to standalone"
else
  echo "batch-isolation RED file_b — batch .c diverged from standalone (cross-file leak)"
  fail=1
fi

if [ "$fail" = "0" ]; then
  echo "batch-isolation OK — --batch-list keeps separate inputs isolated"
else
  echo "batch-isolation FAILED — the batch driver contaminated a compilation" >&2
fi
exit $fail
