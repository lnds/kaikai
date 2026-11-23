#!/bin/sh
# Permission-denied fs errors on both backends; root bypasses file modes, so it skips.

set -eu

if [ "$(id -u)" = 0 ]; then
  echo "fs_err_denied SKIP (running as root: chmod 000 does not deny access)"
  exit 0
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
SRC="$ROOT/tests/fs_err_denied/main.kai"
GOLD="$ROOT/tests/fs_err_denied/main.out.expected"
OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT INT TERM

for backend in c native; do
  KAI_BACKEND="$backend" "$KAI" run "$SRC" > "$OUT" 2>/dev/null || { echo "fs_err_denied FAIL ($backend run)"; cat "$OUT"; exit 1; }
  diff "$OUT" "$GOLD" > /dev/null || { echo "fs_err_denied FAIL ($backend output)"; diff "$OUT" "$GOLD"; exit 1; }
  echo "fs_err_denied OK ($backend)"
done
