#!/bin/sh
# One function spells a contested declaration's emitted name.
#
# Two modules may each declare an effect or a type `Item`. Identity is
# the declaration's id, and no node carries a name. Below the emitters a
# name is a string again, and a contested one is told apart by its home:
# `hs_spell` mints `Item__da`.
#
# The pieces of a program meet on that string: an evidence struct and
# the perform that casts to it, a runtime label and the handler installed
# under it, a default node and the frame slot that addresses it, an impl
# symbol and the call that names it. They meet only when every emitter
# asks the same function, so `rt_name` is the one caller of `hs_spell`.
# A second file that mints a home spelling is a second rule, and two
# rules disagree on some program.
#
# Nothing reads a spelling back: a diagnostic prints the declared name,
# which the symbol table holds.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/stage2/compiler"

# The file allowed to mint a home spelling.
SPELL_ALLOW='rt_name'

# Comments and `#[doc]` bodies name the function when they explain the
# mechanism, so strip a line's comment before matching: prose about the
# spelling is not a use of it.
scan() {
  pat="$1"
  for f in "$SRC"/*.kai; do
    b=$(basename "$f" .kai)
    [ "$b" = "home_spell" ] && continue
    n=$(sed 's/#.*$//' "$f" | grep -cE "(^|[^a-z_])$pat\(" || true)
    [ "$n" -gt 0 ] && printf '%s %s\n' "$b" "$n"
  done
  return 0
}

spellers=$(scan hs_spell | grep -vE "^($SPELL_ALLOW) " || true)
if [ -n "$spellers" ]; then
  echo "respelling-confined FAIL — a second file mints a home spelling:" >&2
  echo "$spellers" | sed 's/^/  /' >&2
  echo "  An emitter that spells a declaration asks rt_name(tab, id), which" >&2
  echo "  decides when a name is contested. A pass that needs to know which" >&2
  echo "  declaration a name means holds its SymId instead." >&2
  exit 1
fi

ns=$(scan hs_spell | awk '{s+=$2} END {print s+0}')
echo "respelling-confined OK — $ns minting site(s), in rt_name"
