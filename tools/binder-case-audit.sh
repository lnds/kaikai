#!/bin/sh
# Every parser function that mints a binder must consult the naming table
# (tok_bad / name_bad of parse_naming.kai) in the same function, so a new
# binder site cannot skip the case check. A function whose check lives
# elsewhere is pinned in tools/baselines/binder-case/ as
# `<file>__<fn>` holding the reason.
#
# Usage: tools/binder-case-audit.sh

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PINS="$ROOT/tools/baselines/binder-case"
fail=0

for f in "$ROOT"/stage2/compiler/parse_*.kai; do
  base=$(basename "$f")
  [ "$base" = "parse_naming.kai" ] && continue
  awk -v file="$base" '
    function flush() {
      if (name != "" && mints && !checks) print file "__" name
    }
    /^(pub(\(pkg\))? )?fn / {
      flush()
      line = $0
      sub(/^(pub(\(pkg\))? )?fn /, "", line)
      sub(/[\[(].*/, "", line)
      name = line; mints = 0; checks = 0
    }
    /binder_written\(|tbinder_written\(|PendName \{/ { mints = 1 }
    /tok_bad\(|name_bad\(/ { checks = 1 }
    END { flush() }
  ' "$f"
done | sort > "${TMPDIR:-/tmp}/binder-case.$$"

while read -r key; do
  if [ ! -f "$PINS/$key" ]; then
    echo "binder-case-audit FAIL — $key mints a binder without consulting the naming table; call tok_bad/name_bad in it, or pin it in tools/baselines/binder-case/ with the reason"
    fail=1
  fi
done < "${TMPDIR:-/tmp}/binder-case.$$"

for pin in "$PINS"/*; do
  [ -f "$pin" ] || continue
  key=$(basename "$pin")
  if ! grep -qx "$key" "${TMPDIR:-/tmp}/binder-case.$$"; then
    echo "binder-case-audit FAIL — stale pin $key (the function now checks, or is gone)"
    fail=1
  fi
done

rm -f "${TMPDIR:-/tmp}/binder-case.$$"
[ "$fail" = 0 ] && echo "binder-case-audit OK"
exit "$fail"
