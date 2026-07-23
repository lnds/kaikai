#!/bin/sh
# Incremental-rebuild typed codec + interface-hash gates (issue #1428).
#
# The interface hash is what a per-module rebuild cut compares to decide
# whether a module's importers must be re-inferred. Two properties make
# that cut sound, and this fixture is the gate on both:
#
#   - DETERMINISM. Logically-equal interfaces must hash equal regardless
#     of inference order — table insertion order and HM's tyvar
#     numbering both vary run to run. A hash that moves on a
#     semantically-identical interface only costs cache hits, but a
#     canonicalization that over-collapses would let a CHANGED interface
#     keep its old hash: a stale-interface hit, the one unsound failure
#     the cut must never have.
#   - SENSITIVITY. A changed exported scheme must move the hash, or a
#     downstream module keeps type-checking against a signature that no
#     longer exists.
#
# Plus the `Ty`/`TyScheme` codec round-trip: a codec that drops or
# reorders a node restores a different typed artifact, which would break
# the byte-identical-C property the whole cache rests on.
#
# The checks run in-process (`kaic2 --cache-typed-selftest`) because
# they exercise compiler-internal types with no surface syntax.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

if [ ! -x "$KAIC2" ]; then
  echo "typedc_iface_hash_gates: FAIL (kaic2 not built at $KAIC2)"
  exit 1
fi

out="$("$KAIC2" --cache-typed-selftest 2>&1)" || {
  echo "typedc_iface_hash_gates: FAIL (selftest exited non-zero)"
  echo "$out"
  exit 1
}

case "$out" in
  OK*)
    echo "typedc_iface_hash_gates: OK"
    echo "  $out"
    ;;
  *)
    echo "typedc_iface_hash_gates: FAIL"
    echo "  $out"
    exit 1
    ;;
esac
