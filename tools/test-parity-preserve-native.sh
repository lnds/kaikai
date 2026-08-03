#!/usr/bin/env bash
# Contract gate for `tools/parity-preserve-native.sh`.
#
# The regression it locks: `tier1-backend-parity` used to declare `kaic2` a
# prerequisite, which relinks C-only whenever llvm-config is off PATH (a
# keg-only Homebrew LLVM, i.e. the mac default). The parity harness then
# probed native, found it gone, and exited 0 with SKIP — a green that
# verified nothing, over a capability the same make invocation had just
# destroyed. The C-only SKIP is correct policy; silently reaching it from a
# native tree is not.
#
# Hermetic: a sandbox root with a stub bin/kai whose native support is a
# flag file, and a stub `make` recording its argv. No real compiler build,
# no libLLVM — the three decisions are reachable on any host.

set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
check_mentions() {
  local label="$1" needle="$2" hay="$3"
  case "$hay" in
    *"$needle"*) echo "  ok   $label mentions '$needle'" ;;
    *) echo "  FAIL $label: '$needle' missing from: $hay"; fail=1 ;;
  esac
}
check_rc() {
  local label="$1" want="$2" got="$3"
  if [ "$got" = "$want" ]; then echo "  ok   $label -> exit $got"
  else echo "  FAIL $label: want exit $want, got $got"; fail=1; fi
}

# $1 = sandbox dir, $2 = 1 if the stub kai supports --backend=native.
# The stub kai consults $dir/native-ok at RUN time, so a stub `make` can
# flip it to model a rebuild that gains or loses the backend.
make_sandbox() {
  local dir="$1" native="$2"
  mkdir -p "$dir/tools" "$dir/bin" "$dir/stage2" "$dir/shim"
  cp "$ROOT/tools/parity-preserve-native.sh" "$dir/tools/"
  : > "$dir/stage2/kaic2"; chmod +x "$dir/stage2/kaic2"
  [ "$native" = 1 ] && : > "$dir/native-ok" || rm -f "$dir/native-ok"
  cat > "$dir/bin/kai" <<EOF
#!/bin/sh
case " \$* " in
  *" --backend=native "*) [ -f "$dir/native-ok" ] || exit 1 ;;
esac
exit 0
EOF
  chmod +x "$dir/bin/kai"
  # Stub make: records argv, and honours GAIN/LOSE to model what the
  # rebuild does to the tree's backend capability.
  cat > "$dir/shim/make" <<EOF
#!/bin/sh
echo "\$@" >> "$dir/make.argv"
case " \$* " in *" KAI_LLVM=1 "*) [ "\${SANDBOX_REBUILD:-keep}" = lose ] && rm -f "$dir/native-ok" ;; esac
case " \$* " in *" KAI_LLVM=1 "*) [ "\${SANDBOX_REBUILD:-keep}" = gain ] && : > "$dir/native-ok" ;; esac
exit 0
EOF
  chmod +x "$dir/shim/make"
}

# Run the script inside a sandbox. $2.. = env assignments.
run() {
  local dir="$1"; shift
  ( cd "$dir" && env PATH="$dir/shim:$PATH" "$@" bash tools/parity-preserve-native.sh 2>&1 )
}

echo "== test-parity-preserve-native =="

# 1. THE REGRESSION. Native tree, no llvm-config anywhere: the rebuild would
#    downgrade it, so the script must stop loud instead of letting the
#    harness reach its SKIP.
make_sandbox "$TMP/native-noconfig" 1
rc=0; out="$(run "$TMP/native-noconfig" PATH="$TMP/native-noconfig/shim:/usr/bin:/bin" LLVM_CONFIG=/nonexistent/llvm-config)" || rc=$?
check_rc "native tree, no llvm-config" 1 "$rc"
check_mentions "native/no-config stop" "NATIVE kaic2" "$out"
check_mentions "native/no-config stop" "LLVM_CONFIG=" "$out"
[ -f "$TMP/native-noconfig/make.argv" ] \
  && { echo "  FAIL native/no-config: rebuilt anyway (argv: $(cat "$TMP/native-noconfig/make.argv"))"; fail=1; } \
  || echo "  ok   native/no-config did not rebuild"

# 2. Native tree WITH an llvm-config: rebuild must forward KAI_LLVM=1, so
#    the capability survives.
make_sandbox "$TMP/native-config" 1
printf '#!/bin/sh\nexit 0\n' > "$TMP/native-config/shim/llvm-config"
chmod +x "$TMP/native-config/shim/llvm-config"
rc=0; out="$(run "$TMP/native-config")" || rc=$?
check_rc "native tree, llvm-config present" 0 "$rc"
check_mentions "native rebuild argv" "KAI_LLVM=1" "$(cat "$TMP/native-config/make.argv" 2>/dev/null)"

# 3. Native tree, llvm-config present, but the rebuild loses native anyway
#    (a broken link). Must fail rather than fall through to SKIP.
make_sandbox "$TMP/native-lost" 1
printf '#!/bin/sh\nexit 0\n' > "$TMP/native-lost/shim/llvm-config"
chmod +x "$TMP/native-lost/shim/llvm-config"
rc=0; out="$(run "$TMP/native-lost" SANDBOX_REBUILD=lose)" || rc=$?
check_rc "rebuild lost native" 1 "$rc"
check_mentions "rebuild-lost stop" "lost the native backend" "$out"

# 4. C-only tree, no request for native: the SKIP path is INTENTIONAL and
#    must stay reachable. Rebuild plainly, exit 0, no KAI_LLVM=1.
make_sandbox "$TMP/conly" 0
rc=0; out="$(run "$TMP/conly")" || rc=$?
check_rc "C-only tree" 0 "$rc"
case "$(cat "$TMP/conly/make.argv" 2>/dev/null)" in
  *KAI_LLVM=1*) echo "  FAIL C-only: forced KAI_LLVM=1 on a C-only tree"; fail=1 ;;
  *) echo "  ok   C-only rebuild stayed C-only (SKIP path intact)" ;;
esac

# 5. C-only tree but native asked for EXPLICITLY, and the rebuild does not
#    deliver it. An explicit request must never be answered with SKIP.
make_sandbox "$TMP/asked-unmet" 0
rc=0; out="$(run "$TMP/asked-unmet" KAI_LLVM=1)" || rc=$?
check_rc "KAI_LLVM=1 unmet" 1 "$rc"
check_mentions "unmet-request stop" "Not reporting SKIP" "$out"

# 6. C-only tree, native asked for explicitly, and the rebuild delivers.
make_sandbox "$TMP/asked-met" 0
rc=0; out="$(run "$TMP/asked-met" KAI_LLVM=1 SANDBOX_REBUILD=gain)" || rc=$?
check_rc "KAI_LLVM=1 met" 0 "$rc"

if [ "$fail" -eq 0 ]; then
  echo "test-parity-preserve-native OK — the parity gate cannot silently downgrade a native tree"
else
  echo "test-parity-preserve-native FAILED"
fi
exit "$fail"
