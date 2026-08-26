#!/bin/sh
# Symbol-table gates.
#
# The table assigns one opaque id per declaration and resolves it to
# {class, name, home, scope} in O(1). Nothing consumes it yet, so these
# in-process properties are the whole safety net:
#
#   - IDENTITY. Two declarations of one bare name in different homes must
#     be different ids reporting their own homes. String comparison
#     cannot hold this, which is the reason the table exists.
#   - NAMESPACES. A `type Log` and an `effect Log` are two habitants; a
#     class-filtered lookup must return one, never both.
#   - INDEX FIDELITY. The bucket lookup must agree with a linear scan for
#     every name in the table. A hash that dropped an entry would resolve
#     a declared name to `None` — a miss that looks like an undeclared
#     name to any future consumer.
#   - DENSITY. Ids must be dense and in stream order, so the table is a
#     function of the decl list alone.
#
# The checks run in-process (`kaic2 --symtab-selftest`) because they
# exercise compiler-internal types with no surface syntax.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; run the one the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"

if [ ! -x "$KAIC2" ]; then
  echo "typedc_symtab_gates: FAIL (kaic2 not built at $KAIC2)"
  exit 1
fi

out="$("$KAIC2" $EDITION_FLAG --symtab-selftest 2>&1)" || {
  echo "typedc_symtab_gates: FAIL (selftest exited non-zero)"
  echo "$out"
  exit 1
}

case "$out" in
  OK*)
    echo "typedc_symtab_gates: OK"
    echo "  $out"
    ;;
  *)
    echo "typedc_symtab_gates: FAIL"
    echo "  $out"
    exit 1
    ;;
esac
