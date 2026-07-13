#!/usr/bin/env bash
# M:N determinism gate (no TSAN, fast — rides tier1). A deterministic
# concurrent program must print the SAME output at KAI_THREADS=1 and
# KAI_THREADS=4: a divergence is either a real missing serialization point
# or a latent scheduling-order dependence, and this gate surfaces it. Also
# asserts the cross-thread run terminates (no shutdown hang) and is stable
# across repeats.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

FIXTURES=(
  "demos/parallel_actors/main.kai"
  "examples/effects/mn_cross_thread_copy_stress.kai"
)

fail=0
for src in "${FIXTURES[@]}"; do
  name="$(basename "$(dirname "$src")")/$(basename "$src")"
  bin="$TMP/$(echo "$name" | tr '/' '_')"
  "$KAI" build "$src" -o "$bin" >/dev/null 2>"$TMP/b.log" \
    || { echo "BUILD FAILED $name"; cat "$TMP/b.log"; exit 1; }

  ref="$(KAI_THREADS=1 "$bin")"
  ok=1
  for n in 4 8; do
    for r in 1 2 3; do
      got="$(timeout 60 env KAI_THREADS=$n "$bin")" || { echo "HANG/FAIL $name N=$n"; ok=0; break; }
      [ "$got" = "$ref" ] || { echo "DIVERGE $name N=$n: '$got' != N=1 '$ref'"; ok=0; break; }
    done
    [ "$ok" = "1" ] || break
  done
  [ "$ok" = "1" ] && echo "OK $name (N=1==N=4==N=8: $ref)" || fail=1
done

[ "$fail" = "0" ] && echo "run-mn-determinism: OK" || { echo "run-mn-determinism: FAIL"; exit 1; }
