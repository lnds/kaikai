#!/bin/bash
# Native self-host gate.
#
# The native-vs-C parity ratchet covers user-program fixtures, but nothing
# compiles the COMPILER ITSELF with the native backend (the compiler builds
# C-only). So a KIR construct the native backend cannot lower yet, but the
# compiler's own source uses, is never exercised — the subset gap is silent.
#
# This gate makes the gap visible and drives it to full SELF-COMPILE:
#   1. compiles `stage2/main.kai` with `--emit=native` and counts the
#      `unbound register` subset-gap aborts, ratcheting against
#      tools/native-selfhost-baseline.txt (FAIL on regression). The baseline
#      is 0 — the COMPILE half is cleared.
#   2. once the count is 0, LINKS the native object into an executable
#      kaic2-native (tools/native-selfhost-link.sh), RUNS it (`--version`),
#      then SELF-COMPILES: the native-built kaic2 emits C for a sample
#      program and that output must be BYTE-IDENTICAL to the C-direct oracle.
#      A native codegen bug that corrupts the emitted program shows up here.
#
# Milestone status (issue #1021): COMPILE + LINK + RUN + SELF-COMPILE are
# green — the circle closes.
#
# Native-only: does not touch the C bootstrap or the selfhost C-byte-id
# check. Exits 0 (SKIP) where libLLVM is absent.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

BASELINE_FILE="$ROOT/tools/native-selfhost-baseline.txt"

LLVM_CONFIG="${LLVM_CONFIG:-llvm-config}"
if ! command -v "$LLVM_CONFIG" >/dev/null 2>&1; then
  echo "native-selfhost-gate: SKIP (llvm-config not in PATH; native backend needs libLLVM)"
  exit 0
fi

CC="${CC:-cc}"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the repo's EDITION is what
# this gate must exercise.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
RUNTIME_LLVM_BC="$ROOT/stage0/runtime_llvm.bc"
[ -f "$RUNTIME_LLVM_BC" ] || RUNTIME_LLVM_BC=""

# Read the baseline: the single non-comment, non-blank integer line.
if [ ! -f "$BASELINE_FILE" ]; then
  echo "native-selfhost-gate FAIL — baseline file missing: $BASELINE_FILE"
  exit 1
fi
baseline="$(grep -vE '^[[:space:]]*(#|$)' "$BASELINE_FILE" | head -1 | tr -d '[:space:]')"
case "$baseline" in
  ''|*[!0-9]*)
    echo "native-selfhost-gate FAIL — baseline is not an integer: '$baseline'"
    exit 1 ;;
esac

# The native backend needs a kaic2 linked against libLLVM. The build job
# already produced exactly that binary and shipped it in the artifact, so
# rebuilding it here re-ran a `cc -O2` over the whole ~100 MB translation
# unit for a binary that was already correct. Assert the capability
# instead: a kaic2 without the native backend cannot emit an object, so a
# one-line probe distinguishes the two in well under a second.
[ -x "$KAIC2" ] || { echo "native-selfhost-gate FAIL — kaic2 not found at $KAIC2"; exit 1; }

echo "native-selfhost-gate: probing the native backend …"
PROBE_DIR="$(mktemp -d)"
printf 'fn main() : Unit / Console = println("probe")\n' > "$PROBE_DIR/probe.kai"
probe_rc=0
( cd "$PROBE_DIR" \
  && env KAI_NATIVE_RUNTIME_BC="$RUNTIME_LLVM_BC" \
       "$KAIC2" $EDITION_FLAG --emit=native --path "$ROOT/stdlib" probe.kai \
       >/dev/null 2>"$PROBE_DIR/probe.err" ) || probe_rc=$?
if [ "$probe_rc" -ne 0 ] || [ ! -f "$PROBE_DIR/probe.o" ]; then
  echo "::error::native-selfhost-gate FAIL — this kaic2 cannot emit native objects (exit $probe_rc)."
  echo "  The gate needs the KAI_LLVM=1 binary the build job publishes; this one was built C-only."
  head -10 "$PROBE_DIR/probe.err" 2>/dev/null | sed 's/^/    /'
  rm -rf "$PROBE_DIR"; exit 1
fi
rm -rf "$PROBE_DIR"
echo "native-selfhost-gate: native backend OK"

# Compile the compiler with itself, native backend. Run from stage2/ so
# `import compiler.driver` in main.kai resolves against compiler/*.kai
# (the modular path kaic2 takes — kaic1's concatenated bundle is the
# bootstrap path and is irrelevant here). The native spine derives the
# object path from the source (main.kai -> main.o) and ignores -o, so we
# pass no -o and clean main.o (+ any partial compiler/*.o) afterwards.
ERR="$(mktemp)"
trap 'rm -f "$ERR" "$ROOT/stage2/main.o" "$ROOT/stage2/kaic2-native" "$ROOT/stage2/native-selfhost-shim.o"; find "$ROOT/stage2/compiler" -name "*.o" -delete 2>/dev/null || true' EXIT

echo "native-selfhost-gate: compiling stage2/main.kai with --emit=native …"
rc=0
( cd "$ROOT/stage2" \
  && env KAI_NATIVE_RUNTIME_BC="$RUNTIME_LLVM_BC" \
       "$KAIC2" $EDITION_FLAG --emit=native --path "$ROOT/stdlib" main.kai >/dev/null 2>"$ERR" ) || rc=$?
obj_produced=0
[ -f "$ROOT/stage2/main.o" ] && obj_produced=1

count="$(grep -c 'unbound register' "$ERR" || true)"
count="${count:-0}"

# A zero count with a FAILED compile that produced no object is NOT the
# gap-closed milestone — it is an unrelated failure (typer/parser/link or
# a NEW subset-gap class that is not an `unbound register`). Surface it
# loudly instead of mis-reporting it as ACHIEVED or as a silent pass.
if [ "$count" -eq 0 ] && { [ "$rc" -ne 0 ] || [ "$obj_produced" -ne 1 ]; }; then
  echo "::error::native-selfhost-gate FAIL — the native self-compile failed (exit $rc, object produced=$obj_produced) with ZERO 'unbound register' aborts."
  echo "  This is NOT the known subset gap (which is all 'unbound register'). Likely an unrelated break or a new gap class. First 40 stderr lines:"
  head -40 "$ERR" | sed 's/^/    /'
  exit 1
fi

echo "native-selfhost-gate: unbound-register subset-gap count = $count (baseline $baseline)"
if [ "$count" -gt 0 ]; then
  echo "  breakdown by register name:"
  grep 'unbound register' "$ERR" \
    | sed -E 's/.*unbound register ([a-zA-Z0-9_]+).*/\1/' \
    | sort | uniq -c | sort -rn \
    | sed 's/^/    /'
fi

if [ "$count" -gt "$baseline" ]; then
  echo "::error::native-selfhost-gate FAIL — REGRESSION: $count subset-gap aborts > baseline $baseline."
  echo "  The native backend lost ground: it now rejects more of the compiler's KIR than before."
  echo "  Either fix the regression, or (if intentional and justified) raise the baseline in $BASELINE_FILE."
  exit 1
elif [ "$count" -eq 0 ]; then
  # COMPILE cleared: the native backend lowers the whole compiler with zero
  # subset-gap aborts and produced main.o. Now assert LINK + RUN — the
  # milestone this gate ratchets toward (issue #1021).
  echo "native-selfhost-gate: COMPILE OK — the compiler lowers under the native backend with zero subset-gap aborts."
  echo "native-selfhost-gate: linking the native compiler object …"
  BIN="$ROOT/stage2/kaic2-native"
  if ! LLVM_CONFIG="$LLVM_CONFIG" CC="$CC" "$ROOT/tools/native-selfhost-link.sh" "$ROOT/stage2/main.o" "$BIN" 2>&1; then
    echo "::error::native-selfhost-gate FAIL — the native compiler object did NOT link into an executable."
    exit 1
  fi
  [ -x "$BIN" ] || { echo "::error::native-selfhost-gate FAIL — linker produced no $BIN"; exit 1; }
  echo "native-selfhost-gate: LINK OK — produced $BIN"

  # RUN: the native-built compiler must START and execute real kaikai code.
  # `--version` runs `fn main`'s driver far enough to print via Stdout (the
  # inferred-row default-handler path #1021 fixed) and exit cleanly, WITHOUT
  # entering the front-end. It proves the binary loads, its runtime + effect
  # defaults are wired, and it runs — the LINK+RUN half of the milestone.
  echo "native-selfhost-gate: running the native compiler (--version) …"
  vout="$("$BIN" --version 2>&1)"; vrc=$?
  if [ "$vrc" -ne 0 ]; then
    echo "::error::native-selfhost-gate FAIL — the native-built compiler crashed on --version (exit $vrc)."
    echo "  It linked but does not run. Output:"; echo "$vout" | sed 's/^/    /'
    rm -f "$BIN"; exit 1
  fi
  echo "native-selfhost-gate: RUN OK — the native-built compiler starts and runs: $vout"

  # SELF-COMPILE (recursive self-host): the native-built kaic2 emits C for a
  # sample program, and that C must be BYTE-IDENTICAL to what the C-direct
  # oracle emits. This closes the circle — the native-built compiler produces
  # the same program the trusted bootstrap compiler does. Run from ROOT so a
  # transitive `import` of a `stdlib/core/*.kai` module resolves relative to
  # the CWD (the driver composes core paths from the CWD, not from --path;
  # the oracle does the same, so both must run in the same directory).
  SAMPLE="$(mktemp -d)/sc_probe.kai"
  printf 'fn main() : Unit / Console = println("kaikai self-host")\n' > "$SAMPLE"
  # `x="$(cmd)"; rc=$?` dies under `set -e` before the diagnostic below
  # can run: a crashing compiler takes the whole gate with it and the job
  # reports a bare exit code. Guard each capture so a non-zero status is
  # data, not a fatal error, and a signal death names its signal.
  nc="$("$BIN" --emit=c --path "$ROOT/stdlib" "$SAMPLE" 2>/dev/null)" || ncrc=$?
  ncrc="${ncrc:-0}"
  if [ "$ncrc" -gt 128 ]; then
    echo "::error::native-selfhost-gate FAIL — the native-built compiler died on signal $((ncrc - 128)) compiling the sample program."
    echo "  The binary links and runs \`--version\`, so this is the front-end,"
    echo "  not startup. Sample: $SAMPLE"
    rm -f "$BIN"; exit 1
  fi
  oc="$("$KAIC2" $EDITION_FLAG --emit=c --path "$ROOT/stdlib" "$SAMPLE" 2>/dev/null)" || ocrc=$?
  ocrc="${ocrc:-0}"
  # The trivial sample derives nothing, so the `#[derive(Layout)]` impl
  # builder never runs. Re-run the SAME native binary over a Layout-deriving
  # program so that codegen lowers under the native backend, and assert
  # byte-identical C vs the oracle — a native codegen bug in the derive
  # (crash or corruption) fails here. Reuses `$BIN`, so it costs a
  # `--emit=c` + diff, not a rebuild.
  LAYOUT="$ROOT/examples/sugars/kinds_layout_derive.kai"
  lnc=""; loc=""; lncrc=0; locrc=0
  if [ -f "$LAYOUT" ]; then
    lnc="$("$BIN" --emit=c --path "$ROOT/stdlib" "$LAYOUT" 2>/dev/null)" || lncrc=$?
    lncrc="${lncrc:-0}"
    if [ "$lncrc" -gt 128 ]; then
      echo "::error::native-selfhost-gate FAIL — the native-built compiler died on signal $((lncrc - 128)) compiling $LAYOUT."
      rm -f "$BIN"; exit 1
    fi
    loc="$("$KAIC2" $EDITION_FLAG --emit=c --path "$ROOT/stdlib" "$LAYOUT" 2>/dev/null)" || locrc=$?
    locrc="${locrc:-0}"
  fi
  rm -f "$BIN"
  if [ "$ncrc" -ne 0 ]; then
    echo "::error::native-selfhost-gate FAIL — the native-built compiler failed to compile the sample program (exit $ncrc)."
    exit 1
  fi
  if [ "$ocrc" -ne 0 ]; then
    echo "::error::native-selfhost-gate FAIL — the C-direct oracle failed to compile the sample program (exit $ocrc). Unexpected — the oracle is the trusted reference."
    exit 1
  fi
  if [ "$nc" != "$oc" ]; then
    echo "::error::native-selfhost-gate FAIL — the native-built compiler's emitted C DIVERGES from the oracle. A native codegen bug corrupts the emitted program."
    diff <(printf '%s' "$oc") <(printf '%s' "$nc") | head -20 | sed 's/^/    /'
    exit 1
  fi
  echo "native-selfhost-gate: SELF-COMPILE OK — the native-built compiler emits byte-identical C to the oracle."
  if [ -f "$LAYOUT" ]; then
    if [ "$lncrc" -ne 0 ]; then
      echo "::error::native-selfhost-gate FAIL — the native-built compiler failed on a Layout-bearing program (exit $lncrc). This is the #1201 native layout-rewrite crash class."
      exit 1
    fi
    if [ "$locrc" -ne 0 ]; then
      echo "::error::native-selfhost-gate FAIL — the C-direct oracle failed on the Layout program (exit $locrc). Unexpected — the oracle is the trusted reference."
      exit 1
    fi
    if [ "$lnc" != "$loc" ]; then
      echo "::error::native-selfhost-gate FAIL — the native-built compiler's emitted C DIVERGES from the oracle on a Layout-bearing input."
      diff <(printf '%s' "$loc") <(printf '%s' "$lnc") | head -20 | sed 's/^/    /'
      exit 1
    fi
    echo "native-selfhost-gate: LAYOUT SELF-COMPILE OK — byte-identical C for a Layout-deriving program (the native derive codegen lowers cleanly)."
  fi
  echo "native-selfhost-gate: COMPILE + LINK + RUN + SELF-COMPILE achieved — the circle closes (issue #1021)."
  exit 0
elif [ "$count" -lt "$baseline" ]; then
  echo "native-selfhost-gate: PASS — baseline improvable; part of the gap closed."
  echo "  Lower the baseline in $BASELINE_FILE from $baseline to $count to tighten the ratchet."
  exit 0
else
  echo "native-selfhost-gate: PASS — known gap held at baseline $baseline (documented expected-fail)."
  exit 0
fi
