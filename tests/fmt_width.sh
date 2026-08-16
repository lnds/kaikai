#!/bin/sh
# tests/fmt_width.sh — the line-width gate for `kai fmt`.
#
# The golden suite pins the exact bytes of a few shapes and the
# property harness pins meaning. Neither can answer the question the
# width rewrite is judged on: does the writer produce lines a human
# wants to read? This harness answers it with two numbers over
# examples/fmt/width/, the corpus written for exactly this purpose:
#
#   soft      over-budget lines the writer COULD have broken (its
#             longest unbreakable atom still fits). The number a width
#             model drives to zero.
#   collapse  lines the writer removed relative to the hand-wrapped
#             source: sum of max(0, lines(input) - lines(fmt(input))).
#             This is the axis `soft` cannot see — a four-line pipe
#             chain flattened to 65 characters is inside any budget and
#             still destroys the style the documentation teaches.
#
# Both are RATCHETED, not asserted: they carry today's values in
# tools/fmt-width-baseline.txt. A run that measures worse fails as a
# regression; a run that measures better fails asking for the baseline
# to be updated, so the file records progress instead of rotting. That
# is the same discipline as tools/coverage-baseline.txt and the
# skip-lists in tests/fmt_selfhost.sh.
#
# Two fixtures are asserted absolutely rather than ratcheted, because
# their whole point is that no writer may move them:
#
#   already_narrow      fits with room to spare — soft and collapse
#                       must be 0, today and after any rewrite. The net
#                       against a writer that invents breaks.
#   unbreakable_atoms   every over-budget line is `hard` — soft must be
#                       0. The net against a metric that charges the
#                       writer for a long string literal.
#
# Env:
#   FMT_WIDTH=N          budget in columns (default: the baseline's)
#   FMT_WIDTH_STRICT=1   additionally require soft == 0 and collapse
#                        == 0 — the acceptance criterion of the width
#                        rewrite. Off by default; the lane that lands
#                        the document model turns it on for good.
#   VERBOSE=1            print the per-fixture table on success too.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
REPORT="$ROOT/tools/fmt-width-report.sh"
BASELINE="$ROOT/tools/fmt-width-baseline.txt"
CORPUS="$ROOT/examples/fmt/width"

if [ ! -x "$KAIC2" ]; then
  echo "fmt_width: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

baseline_of() {
  sed -n "s/^$1 *//p" "$BASELINE" | head -1
}

budget="${FMT_WIDTH:-$(baseline_of budget)}"
base_soft="$(baseline_of soft)"
base_soft_max="$(baseline_of soft_max)"
base_collapse="$(baseline_of collapse)"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

collapse=0
fail=0

# fmt every fixture into $tmp, keeping the fixture's own name so the
# report table reads as the corpus does.
for input in "$CORPUS"/*.input.kai; do
  name=$(basename "$input" .input.kai)
  if ! "$KAIC2" --path "$ROOT/stdlib" --fmt "$input" > "$tmp/$name.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — fmt refused a width fixture:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    rm -f "$tmp/$name.kai"
    continue
  fi
  in_lines=$(wc -l < "$input" | tr -d ' ')
  out_lines=$(wc -l < "$tmp/$name.kai" | tr -d ' ')
  lost=$((in_lines - out_lines))
  [ "$lost" -gt 0 ] || lost=0
  collapse=$((collapse + lost))
  echo "$name $lost" >> "$tmp/collapse.txt"
done

if [ "$fail" -gt 0 ]; then
  echo "fmt_width: $fail fixture(s) rejected by fmt" >&2
  exit 1
fi

totals=$(cd "$tmp" && sh "$REPORT" --width "$budget" --totals ./*.kai)
soft=$(echo "$totals" | sed -n 's/.*soft=\([0-9]*\).*/\1/p')
hard=$(echo "$totals" | sed -n 's/.*hard=\([0-9]*\).*/\1/p')
soft_max=$(echo "$totals" | sed -n 's/.*soft_max=\([0-9]*\).*/\1/p')

table=$(cd "$tmp" && sh "$REPORT" --width "$budget" ./*.kai)
[ -z "${VERBOSE:-}" ] || echo "$table"

# ---- absolute assertions: the two control fixtures --------------------
control_soft() {
  echo "$table" | awk -v f="./$1.kai" '$1 == f { print $3 }'
}
control_collapse() {
  awk -v f="$1" '$1 == f { print $2 }' "$tmp/collapse.txt"
}

for c in already_narrow unbreakable_atoms; do
  s=$(control_soft "$c")
  if [ "${s:-0}" -ne 0 ]; then
    echo "  FAIL control $c — $s soft over-budget line(s); this fixture must have none"
    fail=$((fail + 1))
  fi
done
n=$(control_collapse already_narrow)
if [ "${n:-0}" -ne 0 ]; then
  echo "  FAIL control already_narrow — fmt removed $n line(s) from a file that already fits"
  fail=$((fail + 1))
fi

# ---- ratchet ---------------------------------------------------------
ratchet() {
  measured="$1"; base="$2"; label="$3"
  if [ "$measured" -gt "$base" ]; then
    echo "  FAIL $label — $measured, worse than the baseline $base"
    return 1
  fi
  if [ "$measured" -lt "$base" ]; then
    echo "  FAIL $label — $measured, better than the baseline $base; record it in tools/fmt-width-baseline.txt"
    return 1
  fi
  return 0
}

ratchet "$soft" "$base_soft" "soft" || fail=$((fail + 1))
ratchet "$soft_max" "$base_soft_max" "soft_max" || fail=$((fail + 1))
ratchet "$collapse" "$base_collapse" "collapse" || fail=$((fail + 1))

if [ -n "${FMT_WIDTH_STRICT:-}" ]; then
  if [ "$soft" -ne 0 ] || [ "$collapse" -ne 0 ]; then
    echo "  FAIL strict — soft=$soft collapse=$collapse; the width rewrite requires both at 0"
    fail=$((fail + 1))
  fi
fi

echo "fmt_width: budget $budget — soft=$soft (max $soft_max), hard=$hard, collapse=$collapse"

if [ "$fail" -gt 0 ]; then
  [ -n "${VERBOSE:-}" ] || echo "$table"
  echo "fmt_width: $fail check(s) failed" >&2
  exit 1
fi
