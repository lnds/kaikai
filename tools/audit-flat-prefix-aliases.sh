#!/usr/bin/env bash
# Issue #614 — institutional regression gate for the m14 follow-up
# qualified-call migration.
#
# For each of the 21 m14 follow-up modules, the canonical user-facing
# surface is the qualified form (`<module>.<op>`) and the legacy
# flat-prefix exports (`<module>_<op>` or the per-module legacy
# prefix like `q_*`, `stk_*`, `rx_*`, `dec_*`) MUST keep being
# defined as `pub fn` somewhere in the module file. A grep-based
# audit catches mass regressions (someone dropped half the aliases
# during a refactor) without invoking the compiler.
#
# Per-fn semantic coverage (the alias resolves AND returns the same
# value as the qualified form) lives in the regression fixtures
# under `examples/stdlib/qualified_migration/`. The fixtures are
# wired into the standard test-stdlib chain; this script is the
# cheap-and-fast institutional gate.
#
# Wired into `stage2/Makefile` as `test-flat-prefix-aliases`, part
# of the tier-1 chain.

set -euo pipefail

cd "$(dirname "$0")/.."

fail=0
pass=0

# Each entry is "<modfile>:<legacy-prefix>" — the audit requires
# at least one `pub fn <legacy-prefix>_*` declaration to exist in
# the module file. The check is grep-based, deliberately coarse:
# it catches "the file is empty" / "the legacy prefix vanished"
# regressions, not subtle per-fn omissions (those live in the
# regression fixtures).
modules=(
  "stdlib/array.kai:array"
  "stdlib/collections/set.kai:set"
  "stdlib/collections/queue.kai:q"
  "stdlib/collections/stack.kai:stk"
  "stdlib/path.kai:path"
  "stdlib/math/int.kai:int"
  "stdlib/math/real.kai:real"
  "stdlib/decimal.kai:dec"
  "stdlib/money.kai:money"
  "stdlib/fx.kai:fx"
  "stdlib/encoding/base64.kai:base64"
  "stdlib/encoding/hex.kai:hex"
  "stdlib/encoding/toml.kai:toml"
  "stdlib/uuid.kai:uuid"
  "stdlib/regexp.kai:rx"
  "stdlib/regexp.kai:regex"
  "stdlib/random_secure.kai:random_secure"
  "stdlib/fs/file.kai:file"
  "stdlib/spawn.kai:fiber"
  "stdlib/log.kai:log"
  "stdlib/net/http.kai:http"
  "stdlib/protocols.kai:bin"
)

for entry in "${modules[@]}"; do
  modfile="${entry%:*}"
  prefix="${entry##*:}"
  if [ ! -f "$modfile" ]; then
    fail=$((fail + 1))
    echo "audit-flat-prefix-aliases: FAIL $modfile missing" >&2
    continue
  fi
  count=$(grep -c "^pub fn ${prefix}_" "$modfile" || true)
  if [ "$count" -lt 1 ]; then
    fail=$((fail + 1))
    echo "audit-flat-prefix-aliases: FAIL $modfile dropped all '${prefix}_*' legacy exports" >&2
  else
    pass=$((pass + 1))
  fi
done

printf 'audit-flat-prefix-aliases: pass=%d fail=%d\n' "$pass" "$fail"
exit "$fail"
