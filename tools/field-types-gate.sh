#!/bin/sh
# A field read the typer leaves without a type is looked up by name at
# run time, where a value with no such field stops the program. The typer
# rejects every read it cannot type, so a program it accepts holds none:
# `--check-field-types` reports each one, over every fixture, demo and
# stdlib module that typechecks. A program the typer rejects reports
# nothing here; its diagnostics are another gate's.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION="$(cat "$ROOT/EDITION")"
fail() { echo "field-types-gate FAIL — $1"; exit 1; }
[ -x "$KAIC2" ] || fail "$KAIC2 is not built"

# An empty report must mean the check ran: it passes a program whose
# reads are all typed.
"$KAIC2" --edition "$EDITION" --check-field-types --path "$ROOT/stdlib"     "$ROOT/examples/refinements/refined_record_field.kai" > /dev/null 2>&1   || fail "--check-field-types no longer passes a program whose field reads are all typed"

files=$(find "$ROOT/examples" "$ROOT/demos" "$ROOT/stdlib" -name '*.kai' \
          -not -path '*/negative/*' -not -path '*/build/*' -not -path '*/.kai/*' | LC_ALL=C sort)

# FIELD_TYPES_SHARD=I/N keeps the files whose line number is congruent to I
# mod N: the N parts are disjoint and cover the corpus.
part=""
if [ -n "${FIELD_TYPES_SHARD:-}" ]; then
  printf '%s\n' "$FIELD_TYPES_SHARD" | grep -Eq '^[1-9][0-9]*/[1-9][0-9]*$' \
    || fail "FIELD_TYPES_SHARD='$FIELD_TYPES_SHARD' is not I/N"
  i="${FIELD_TYPES_SHARD%/*}"; n="${FIELD_TYPES_SHARD#*/}"
  [ "$i" -le "$n" ] || fail "FIELD_TYPES_SHARD='$FIELD_TYPES_SHARD' is not I/N with I <= N"
  total=$(printf '%s\n' "$files" | wc -l | tr -d ' ')
  files=$(printf '%s\n' "$files" | awk -v i="$i" -v n="$n" 'NR % n == i % n')
  kept=$(printf '%s\n' "$files" | grep -c .)
  want=0
  if [ "$total" -ge "$i" ]; then want=$(( (total - i) / n + 1 )); fi
  [ "$kept" -eq "$want" ] || fail "shard $FIELD_TYPES_SHARD kept $kept of $total files, expected $want"
  part=" (part $FIELD_TYPES_SHARD of $total)"
fi
out=$(printf '%s\n' "$files" | xargs -P 8 -n 1 sh -c \
  '"$0" --edition "$1" --check-field-types --path "$(dirname "$3")" --path "$2" "$3" 2>&1 >/dev/null | grep "^untyped field" | sed "s|^|$3: |"; exit 0' \
  "$KAIC2" "$EDITION" "$ROOT/stdlib")

if [ -n "$out" ]; then
  printf '%s\n' "$out"
  fail "a program that typechecks reads a field the typer left untyped"
fi
echo "field-types-gate OK — no untyped field read in $(printf '%s\n' "$files" | wc -l | tr -d ' ') programs$part"
