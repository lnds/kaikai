#!/usr/bin/env bash
# Gate: every `layout:` line of stage2/runtime/record_layout.h lists the
# fields of its kaikai declaration in declaration order. The runtime reads
# those records by slot, so a reordered or renamed field must fail here.

set -uo pipefail
cd "$(dirname "$0")/.."

fail=0
count=0
while IFS= read -r line; do
  spec="${line#*layout: }"
  spec="${spec% \*/}"
  file="${spec%% *}"
  rest="${spec#* }"
  ty="${rest%%:*}"
  want="${rest#*: }"
  decl="$(grep -E "^(pub )?type $ty = \{" "$file" | head -1)"
  if [ -z "$decl" ]; then
    echo "record-layout: no 'type $ty = { ... }' in $file" >&2
    fail=1
    continue
  fi
  body="${decl#*\{}"
  body="${body%\}*}"
  got="$(printf '%s' "$body" | tr ',' '\n' | sed -E 's/^ *(priv +)?//; s/ *:.*$//' | tr '\n' ' ' | sed 's/ *$//')"
  count=$((count + 1))
  if [ "$got" != "$want" ]; then
    echo "record-layout: $file $ty declares '$got', record_layout.h says '$want'" >&2
    fail=1
  fi
done < <(grep -E '^/\* layout: ' stage2/runtime/record_layout.h)

[ "$count" -gt 0 ] || { echo "record-layout: no layout lines found" >&2; exit 1; }
[ "$fail" -eq 0 ] && echo "record-layout: $count layouts match"
exit "$fail"
