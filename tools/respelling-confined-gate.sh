#!/bin/sh
# The respelling stays where it is, and stops spreading.
#
# `hs_spell` rewrites a contested declaration's name to `Name__home` so
# two homonyms produce different strings. It is a stand-in for identity:
# a consumer that can ask which declaration a name means does not need
# the name to have been pre-disambiguated.
#
# Two facts decide what may change (both measured, both re-checkable):
#
#   Effects reach the runtime spelled. An effect label is dispatched by
#   its bare string, so `Log__loga` is baked into the binary and IS the
#   mechanism keeping two homonymous effects apart at run time. That
#   spelling is ABI and does not move.
#
#   Types do not. No `Cfg__ta` appears in an emitted binary; the type
#   respelling exists only to keep the typer from unifying two
#   homonyms, because `TyCon` carries `(Option[String], String, [Ty])`
#   and identity there is `module_slot_compat(am, bm) and an == bn` —
#   a string comparison whose module slot is `None` at every
#   construction site outside a test.
#
# So the type respelling retires when `TyCon` carries identity, not
# before. Until then this gate holds the surface: the files that may
# mint or read a home spelling are listed here by name, and a new one
# is a deliberate edit rather than a quiet spread.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/stage2/compiler"

# Files allowed to mint a home spelling.
#   effect_scope_table — es_spell, the ABI spelling for effects.
#   type_scope_table   — ts_spell, the compile-time spelling for types.
SPELL_ALLOW='effect_scope_table|type_scope_table'

# Files allowed to read one back (strip the suffix, or test for it).
#   decl_names        — registers a decl under the name the user wrote.
#   pub_access_table  — a private alias is reachable under both
#                       spellings; the display one carries the privacy
#                       verdict a bare cross-module reference needs.
#   name_report       — points a diagnostic at the decl behind a name.
#   infer             — renders a spelled head back as `mod.T`.
#   protos            — what a derived `show` prints.
DISPLAY_ALLOW='decl_names|pub_access_table|name_report|infer|protos'

scan() {
  pat="$1"
  for f in "$SRC"/*.kai; do
    b=$(basename "$f" .kai)
    [ "$b" = "home_spell" ] && continue
    n=$(grep -cE "(^|[^a-z_])$pat\(" "$f" || true)
    [ "$n" -gt 0 ] && printf '%s %s\n' "$b" "$n"
  done
  return 0
}

fail=0

spellers=$(scan hs_spell | grep -vE "^($SPELL_ALLOW) " || true)
if [ -n "$spellers" ]; then
  echo "respelling-confined FAIL — a new file mints a home spelling:" >&2
  echo "$spellers" | sed 's/^/  /' >&2
  echo "  Minting belongs behind es_spell / ts_spell, which decide when a" >&2
  echo "  name is contested. A caller that needs to know which declaration" >&2
  echo "  a name means should ask the symbol table for its SymId instead." >&2
  fail=1
fi

readers=$(scan hs_display | grep -vE "^($DISPLAY_ALLOW) " || true)
if [ -n "$readers" ]; then
  echo "respelling-confined FAIL — a new file reads a home spelling back:" >&2
  echo "$readers" | sed 's/^/  /' >&2
  echo "  Stripping a suffix to recover the user's name means the spelling" >&2
  echo "  reached somewhere it should not have. Carry the SymId instead, or" >&2
  echo "  add this file here with the reason written down." >&2
  fail=1
fi

[ "$fail" -eq 1 ] && exit 1

ns=$(scan hs_spell | awk '{s+=$2} END {print s+0}')
nd=$(scan hs_display | awk '{s+=$2} END {print s+0}')
echo "respelling-confined OK — $ns minting sites, $nd display sites, all declared"
