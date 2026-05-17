#!/bin/bash
# Audit Option E canonical-name coverage of the m14-followup modules.
#
# For each module in scope, distinguish three classes of pub fns:
#   BOTH      — both `pub fn <prefix>_<base>` and `pub fn <base>` exist.
#               This is the Option E shape — alias + canonical.
#   LEGACY    — `pub fn <prefix>_<base>` exists, `pub fn <base>` does NOT.
#               The canonical is missing; callers only resolve via the
#               typer's qualified-call legacy fallback (a transition
#               mechanism we want to retire).
#   CANONICAL — `pub fn <base>` exists without a corresponding
#               `pub fn <prefix>_<base>` alias. Either the module
#               never had a legacy form (newer fns) or a previous
#               migration moved it.
#
# Output: a per-module report plus a totals line. Pure source
# inspection — does NOT depend on the typer's fallback behavior.

set -e

# Modules in scope:
#   <modfile>:<legacy-prefix>:<module-name>
# The module-name is the identifier callers write after `import`
# and use as `module.fn`. It may differ from the legacy prefix
# (e.g. queue.kai uses `q_` prefix but the import is `queue`).
modules=(
  "stdlib/array.kai:array:array"
  "stdlib/collections/set.kai:set:set"
  "stdlib/collections/queue.kai:q:queue"
  "stdlib/collections/stack.kai:stk:stack"
  "stdlib/path.kai:path:path"
  "stdlib/math/int.kai:int:int"
  "stdlib/math/real.kai:real:real"
  "stdlib/decimal.kai:dec:decimal"
  "stdlib/money.kai:money:money"
  "stdlib/fx.kai:fx:fx"
  "stdlib/encoding/base64.kai:base64:base64"
  "stdlib/encoding/hex.kai:hex:hex"
  "stdlib/encoding/toml.kai:toml:toml"
  "stdlib/uuid.kai:uuid:uuid"
  "stdlib/regexp.kai:rx:regexp"
  "stdlib/regexp.kai:regex:regexp"
  "stdlib/random_secure.kai:random_secure:random_secure"
  "stdlib/fs/file.kai:file:file"
  "stdlib/spawn.kai:fiber:spawn"
  "stdlib/log.kai:log:log"
  "stdlib/net/http.kai:http:http"
  "stdlib/bin.kai:bin:bin"
)

total_both=0
total_legacy=0
total_canonical=0

for entry in "${modules[@]}"; do
  modfile="${entry%%:*}"
  rest="${entry#*:}"
  prefix="${rest%%:*}"
  modname="${rest#*:}"
  if [ ! -f "$modfile" ]; then
    echo "MISSING $modfile"; continue
  fi

  # All pub fn names (just the identifier, no args)
  all_pub=$(grep -E "^pub fn [a-z]" "$modfile" \
    | awk '{print $3}' | sed 's/[(\[].*//' | sort -u)

  # Bases of legacy-prefixed exports
  legacy_bases=$(echo "$all_pub" \
    | grep -E "^${prefix}_" \
    | sed "s/^${prefix}_//" | sort -u)

  # Canonical-shape names (no prefix)
  canonical_names=$(echo "$all_pub" \
    | grep -vE "^${prefix}_" | sort -u)

  echo
  echo "═══ $modname  (file: $modfile, legacy prefix: $prefix) ═══"

  m_both=0
  m_legacy=0
  legacy_only=()

  for base in $legacy_bases; do
    if echo "$canonical_names" | grep -qx "$base"; then
      m_both=$((m_both + 1))
    else
      m_legacy=$((m_legacy + 1))
      legacy_only+=("$base")
    fi
  done

  m_canonical=0
  canonical_only=()
  for name in $canonical_names; do
    has_legacy_for=0
    for lb in $legacy_bases; do
      if [ "$lb" = "$name" ]; then has_legacy_for=1; break; fi
    done
    if [ "$has_legacy_for" = "0" ]; then
      m_canonical=$((m_canonical + 1))
      canonical_only+=("$name")
    fi
  done

  printf "  BOTH       (legacy + canonical): %2d\n" "$m_both"
  printf "  LEGACY     (canonical MISSING):  %2d" "$m_legacy"
  if [ "$m_legacy" -gt 0 ]; then
    printf "  →"
    for b in "${legacy_only[@]}"; do printf " %s.%s" "$modname" "$b"; done
  fi
  printf "\n"
  printf "  CANONICAL  (no legacy alias):    %2d" "$m_canonical"
  if [ "$m_canonical" -gt 0 ]; then
    printf "  →"
    for b in "${canonical_only[@]}"; do printf " %s.%s" "$modname" "$b"; done
  fi
  printf "\n"

  total_both=$((total_both + m_both))
  total_legacy=$((total_legacy + m_legacy))
  total_canonical=$((total_canonical + m_canonical))
done

echo
echo "═══ TOTAL ═══"
echo "  BOTH      (legacy + canonical): $total_both"
echo "  LEGACY    (canonical MISSING):  $total_legacy"
echo "  CANONICAL (no legacy alias):    $total_canonical"

# Exit non-zero when any legacy-only entries remain — the gate
# becomes green when every legacy form has a canonical sibling.
[ "$total_legacy" -eq 0 ]
