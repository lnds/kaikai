#!/usr/bin/env bash
# Contract gate for `tools/gen-runtime-bc.sh --status`.
#
# The P2 opt-out is silent by design (stage 0 owes a zero-dependency
# fallback), so the status word is the ONLY thing the build banner, the
# `kai --version` field and the benchmark header have to go on. Collapsing
# "no clang 18 here" and "clang 18 is right there, the .bc was never built"
# into one word sends a dev after a toolchain install they already have —
# or, worse, leaves them measuring a ~2x slower native compiler and filing
# it as a codegen bug.
#
# Hermetic: a sandbox root with stub sources and a stub clang, so the three
# states are reachable on any host regardless of what it has installed.
# Seconds, no kaic2 dependency — tier 0.

set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
check() {
  local label="$1" want="$2" got="$3"
  if [ "$got" = "$want" ]; then
    echo "  ok   $label -> $got"
  else
    echo "  FAIL $label: want '$want', got '$got'"
    fail=1
  fi
}

check_mentions() {
  local label="$1" needle="$2" hay="$3"
  case "$hay" in
    *"$needle"*) echo "  ok   $label mentions '$needle'" ;;
    *) echo "  FAIL $label: '$needle' missing from: $hay"; fail=1 ;;
  esac
}

# A sandbox is a fake repo root carrying a copy of the generator whose
# absolute clang-18 candidates are neutralised, so resolution can only reach
# the stub we put on PATH. $1 = major version the stub reports.
make_sandbox() {
  local dir="$1" major="$2"
  mkdir -p "$dir/tools" "$dir/stage0" "$dir/stage2" "$dir/bin"
  echo 'int kaix_stub(void) { return 0; }' > "$dir/stage0/runtime_llvm.c"
  echo '/* stub */' > "$dir/stage2/runtime.h"
  sed -e 's#/opt/homebrew/opt/llvm@18/bin/clang#/nonexistent/a#' \
      -e 's#/usr/local/opt/llvm@18/bin/clang#/nonexistent/b#' \
      -e 's#/usr/lib/llvm-18/bin/clang#/nonexistent/c#' \
      "$ROOT/tools/gen-runtime-bc.sh" > "$dir/tools/gen-runtime-bc.sh"
  chmod +x "$dir/tools/gen-runtime-bc.sh"
  # `clang-18` and `cc` are the two PATH-resolved candidates left; both stub
  # to the same reported version so a host clang cannot leak into the test.
  for name in clang-18 cc; do
    cat > "$dir/bin/$name" <<EOF
#!/bin/sh
case " \$* " in *" --version "*) echo "clang version $major.1.0"; exit 0;; esac
prev=""; out=""
for a in "\$@"; do [ "\$prev" = "-o" ] && out="\$a"; prev="\$a"; done
[ -n "\$out" ] && : > "\$out"
exit 0
EOF
    chmod +x "$dir/bin/$name"
  done
}

status() { PATH="$1/bin:$PATH" "$1/tools/gen-runtime-bc.sh" "${2:-}"; }

echo "== test-p2-status =="

# 1. No clang 18 anywhere: not recoverable without an install, and the line
#    must say so rather than pointing at a rebuild that cannot help.
make_sandbox "$TMP/none" 99
check "no clang 18" "optout no-clang18" "$(status "$TMP/none" --status)"
check_mentions "no-clang18 line" "install" "$(status "$TMP/none" --status-line)"

# 2. clang 18 present, bitcode never generated. The state that bit #1339:
#    reported as a plain opt-out, actually one command from active.
make_sandbox "$TMP/regen" 18
check "clang 18, no .bc" "optout needs-regen" "$(status "$TMP/regen" --status)"
check_mentions "needs-regen line" "make KAI_LLVM=1 kaic2" "$(status "$TMP/regen" --status-line)"

# 3. After a generation run the same root reports active — the generator and
#    the status probe agree on what freshness means.
status "$TMP/regen" >/dev/null 2>&1
check "after generation" "active" "$(status "$TMP/regen" --status)"

# The first word stays the vocabulary callers already parse (tools/assert-runtime-bc.sh).
for s in "$TMP/none" "$TMP/regen"; do
  case "$(status "$s" --status)" in
    active|optout|optout\ *) ;;
    *) echo "  FAIL $s: status first word is neither active nor optout"; fail=1 ;;
  esac
done

if [ "$fail" -eq 0 ]; then
  echo "test-p2-status OK — the three P2 states are distinguishable"
else
  echo "test-p2-status FAILED"
fi
exit "$fail"
