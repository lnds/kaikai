#!/usr/bin/env bash
# The native self-host gate must consume the kaic2 the build job published,
# not rebuild it. The rebuild was a second `cc -O2` over the whole ~100 MB
# translation unit for a binary that was already correct, and it ran inside
# the one job that then holds a multi-GB self-compile alive for ~20 minutes
# on a 16 GB runner — the two peaks together are what pushed the job over
# the memory ceiling.
#
# What replaces it is a capability probe, so a C-only kaic2 still fails fast
# and says so instead of dying twenty minutes later inside the self-compile.
# This guard pins both halves. Hermetic, no kaic2 dependency, milliseconds.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GATE="$ROOT/tools/test-native-selfhost-gate.sh"

[ -f "$GATE" ] || { echo "selfhost-gate-no-rebuild FAIL — missing $GATE"; exit 1; }

fail=0

# The gate must not rebuild the compiler it was handed.
if grep -qE '^[[:space:]]*rm -f "\$KAIC2"' "$GATE"; then
  echo "selfhost-gate-no-rebuild FAIL — the gate deletes \$KAIC2 to force a rebuild."
  fail=1
fi
if grep -qE 'make .*KAI_LLVM=1 kaic2' "$GATE"; then
  echo "selfhost-gate-no-rebuild FAIL — the gate rebuilds kaic2; build-native already published it."
  fail=1
fi

# …and it must still prove the binary can emit native objects before it
# spends twenty minutes assuming so.
if ! grep -q 'probe.kai' "$GATE"; then
  echo "selfhost-gate-no-rebuild FAIL — the gate no longer probes the native backend."
  fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "selfhost-gate-no-rebuild OK — the gate consumes the published kaic2 and probes it"
