#!/bin/bash
# tier1-native (KIR Lane 1.2, docs/kir-design.md §"Lane 1.2").
#
# The in-process libLLVM native backend (`kaic2 --emit=native`,
# docs/kir-design.md §7.2) builds an LLVM module IN MEMORY via the C
# API, emits a native object — no `.ll` text, no `clang` subprocess —
# and the driver links that object against the effects runtime. This
# harness gates that path on every native fixture: for each
# examples/native/*.kai it
#   1. builds the object with the native backend (`kaic2 --emit=native`),
#      links it against stage0/runtime_llvm.c, runs it;
#   2. builds the same fixture with the C-direct backend (the oracle),
#      runs it;
#   3. diffs stdout + exit code. Any divergence is a native-backend bug.
#
# Why this exists: Lane 1 (PR #780) validated `main -> 42` BY HAND, so
# when the rebase left kaic1 emitting the native prims through the
# generic `kai_apply` closure path (a non-callable forwarder => runtime
# panic), CI was green and the break shipped. A backend that is opt-in
# is invisible to tier1 unless a path-gated workflow exercises it; this
# is that workflow's harness (mirrors tools/test-backend-parity.sh for
# the C<->LLVM axis).
#
# SCOPE: the native backend currently emits the `main -> 42` spine
# (the generic KIR walk is Parte B). Fixtures here must be programs the
# spine emits faithfully. As the walk grows, more fixtures land and the
# diff widens to real behaviour — the harness shape does not change.
#
# REQUIRES libLLVM: the native kaic2 links against libLLVM (discovered
# via llvm-config). With no llvm-config in PATH the harness SKIPs with
# success — the C/default path is unaffected and gated elsewhere.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

LLVM_CONFIG="${LLVM_CONFIG:-llvm-config}"
if ! command -v "$LLVM_CONFIG" >/dev/null 2>&1; then
  echo "test-native-parity: SKIP (llvm-config not in PATH; native backend needs libLLVM)"
  exit 0
fi

CC="${CC:-cc}"
FIXDIR="examples/native"
KAIC2="$ROOT/stage2/kaic2"
RUNTIME_LLVM_C="$ROOT/stage0/runtime_llvm.c"
# P2 (docs/native-codegen-perf-plan.md §P2): when the runtime bitcode is
# present (the stage2 KAI_LLVM build generated it via clang 18), exercise the
# P2 path — point kaic2 at the .bc so it links it before O2, and link the
# self-contained object without runtime_llvm.c. Absent → the legacy path
# (cc links runtime_llvm.c). Either way the oracle is C-direct, so this gate
# proves P2 keeps parity, not just that it runs.
RUNTIME_LLVM_BC="$ROOT/stage0/runtime_llvm.bc"
[ -f "$RUNTIME_LLVM_BC" ] || RUNTIME_LLVM_BC=""

# The native backend needs a kaic2 linked against libLLVM. The default
# `make kaic2` does NOT link it, so (re)build with KAI_LLVM=1 here. The
# bootstrap chain (kaic0/kaic1) is rebuilt by the stage2 Makefile as a
# prerequisite, so the native prims registered in stage1 reach kaic1.
#
# CRITICAL: `make` keys off prerequisite timestamps, NOT compiler flags.
# A kaic2 already built WITHOUT KAI_LLVM (e.g. by a prior `make selfhost`)
# is "up to date" w.r.t. build/stage2.c, so `make KAI_LLVM=1 kaic2` would
# NOT relink it against libLLVM — the binary would keep the `#else` stub
# that aborts `--emit=native` ("not built into this compiler"). Force the
# relink by removing the binary first. This only redoes the final link
# (seconds); the kaic0/kaic1 chain and build/stage2.c are untouched.
echo "test-native-parity: building kaic2 with KAI_LLVM=1 …"
rm -f "$KAIC2"
LLVM_CONFIG="$LLVM_CONFIG" make -C "$ROOT/stage2" KAI_LLVM=1 kaic2 >/dev/null 2>&1 \
  || { echo "test-native-parity FAIL — kaic2 (KAI_LLVM=1) build failed"; exit 1; }

if [ ! -x "$KAIC2" ]; then
  echo "test-native-parity FAIL — kaic2 not found at $KAIC2"
  exit 1
fi
if [ ! -f "$RUNTIME_LLVM_C" ]; then
  echo "test-native-parity FAIL — runtime_llvm.c not found at $RUNTIME_LLVM_C"
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

for fix in "$FIXDIR"/*.kai; do
  [ -e "$fix" ] || { echo "test-native-parity: no fixtures in $FIXDIR"; exit 1; }
  name="$(basename "$fix" .kai)"

  # --- native backend: emit object in-process, link, run ---
  obj="$WORK/$name.o"
  nbin="$WORK/$name-native"
  if ! env KAI_NATIVE_RUNTIME_BC="$RUNTIME_LLVM_BC" "$KAIC2" --emit=native -o "$obj" "$fix" >/dev/null 2>"$WORK/$name.nemit.err"; then
    # --emit=native prints the object path; -o is ignored by the
    # current spine, so fall back to the source-derived path.
    :
  fi
  # The spine derives the object path from the source: foo.kai -> foo.o.
  src_obj="${fix%.kai}.o"
  if [ -f "$src_obj" ]; then
    mv "$src_obj" "$obj"
  fi
  if [ ! -f "$obj" ]; then
    echo "FAIL $name — native backend produced no object"
    cat "$WORK/$name.nemit.err" 2>/dev/null || true
    fail=$((fail + 1))
    continue
  fi
  # With the bitcode linked in-process, the object is self-contained (it
  # defines main + every kaix_*), so re-adding runtime_llvm.c would be a
  # duplicate symbol. Without it, link runtime_llvm.c as before.
  if [ -n "$RUNTIME_LLVM_BC" ]; then
    nlink_cmd='"$CC" "$obj" -o "$nbin" -lm'
  else
    nlink_cmd='"$CC" "$obj" "$RUNTIME_LLVM_C" -I "$ROOT/stage2" -I "$ROOT/stage0" -o "$nbin" -lm'
  fi
  if ! eval "$nlink_cmd" >"$WORK/$name.nlink.err" 2>&1; then
    echo "FAIL $name — native object did not link"
    cat "$WORK/$name.nlink.err"
    fail=$((fail + 1))
    continue
  fi
  nout="$("$nbin" 2>/dev/null)" && nrc=0 || nrc=$?

  # --- C-direct backend (the oracle): emit C, compile, run ---
  cfile="$WORK/$name.c"
  cbin="$WORK/$name-c"
  if ! "$KAIC2" "$fix" >"$cfile" 2>"$WORK/$name.cemit.err"; then
    echo "FAIL $name — C-direct backend failed to emit"
    cat "$WORK/$name.cemit.err"
    fail=$((fail + 1))
    continue
  fi
  if ! "$CC" "$cfile" -I "$ROOT/stage2" -I "$ROOT/stage0" -o "$cbin" -lm \
       >"$WORK/$name.clink.err" 2>&1; then
    echo "FAIL $name — C-direct object did not link"
    cat "$WORK/$name.clink.err"
    fail=$((fail + 1))
    continue
  fi
  cout="$("$cbin" 2>/dev/null)" && crc=0 || crc=$?

  # --- diff exit code + stdout ---
  if [ "$nrc" != "$crc" ] || [ "$nout" != "$cout" ]; then
    echo "FAIL $name — native vs C-direct diverged"
    echo "  native: rc=$nrc stdout=[$nout]"
    echo "  C-dir : rc=$crc stdout=[$cout]"
    fail=$((fail + 1))
    continue
  fi
  echo "ok   $name (rc=$nrc)"
  pass=$((pass + 1))
done

echo "test-native-parity: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
