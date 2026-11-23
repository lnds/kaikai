#!/usr/bin/env bash
# asan-switch-gate.sh — every swapcontext/setcontext in the runtime is bracketed
# for AddressSanitizer.
#
# A switch off a stack that ASan is not told about leaves that stack's shadow
# in place, and the next frames to land on the memory trip a false
# stack-buffer-overflow. Each switch call must be preceded, within a few
# lines, by the annotation that names the stack it leaves for
# (KAI_ASAN_OUT / __sanitizer_start_switch_fiber).
#
# `--self-test` runs the checker on hand-written input and needs no tree.

set -euo pipefail
cd "$(dirname "$0")/.."

WINDOW=8

check() {
  awk -v window="$WINDOW" '
    function code(line) { sub(/\/\*.*\*\//, "", line); sub(/\/\/.*/, "", line); return line }
    FNR == 1 { delete hist }
    {
      line = code($0)
      if (line ~ /(^|[^A-Za-z_])(swapcontext|setcontext)\(/ && line !~ /^[ \t]*(static|#|\*|\/\*)/) {
        ok = 0
        for (i = FNR - window; i < FNR; i++)
          if (i > 0 && hist[i] ~ /KAI_ASAN_OUT|__sanitizer_start_switch_fiber/) ok = 1
        if (!ok) { printf "%s:%d: unannotated stack switch: %s\n", FILENAME, FNR, $0; bad = 1 }
      }
      hist[FNR] = $0
    }
    END { exit bad }
  ' "$@"
}

if [ "${1:-}" = "--self-test" ]; then
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  printf 'KAI_ASAN_OUT(w, a, b, NULL);\nsetcontext(&b->ctx);\n' > "$tmp/ok.h"
  printf 'x = 1;\nswapcontext(&a, &b);\n' > "$tmp/bad.h"
  check "$tmp/ok.h" || { echo "asan-switch-gate self-test: rejected an annotated switch" >&2; exit 1; }
  if check "$tmp/bad.h" > /dev/null; then
    echo "asan-switch-gate self-test: accepted an unannotated switch" >&2
    exit 1
  fi
  echo "asan-switch-gate self-test OK"
  exit 0
fi

files=$(ls stage2/runtime/*.h | grep -v '/wasm_ctx\.h$')
# shellcheck disable=SC2086
check $files
echo "asan-switch-gate OK"
