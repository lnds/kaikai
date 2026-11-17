#!/bin/sh
# `--user-cache` without `--user-cache-dir` is a usage error: the compiler
# never derives a cache directory from where the sources sit.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

printf 'fn main() : Unit = ()\n' > "$PROJ/main.kai"
rc=0
"$KAIC2" --edition "$(cat "$ROOT/EDITION")" --user-cache --path "$ROOT/stdlib" "$PROJ/main.kai" \
  > /dev/null 2> "$PROJ/err" || rc=$?
[ "$rc" = "2" ] || { echo "userb_user_cache_needs_dir FAIL — exit $rc, expected 2"; exit 1; }
grep -q "requires --user-cache-dir" "$PROJ/err" || { echo "userb_user_cache_needs_dir FAIL — no usage message"; exit 1; }
[ ! -e "$PROJ/.kai-cache" ] || { echo "userb_user_cache_needs_dir FAIL — a cache directory appeared beside the source"; exit 1; }

echo "userb_user_cache_needs_dir OK — --user-cache alone is rejected, nothing written beside the source"
exit 0
