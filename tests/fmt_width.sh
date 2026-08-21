#!/bin/sh
# tests/fmt_width.sh — the line-width gate for `kai fmt`.
#
# The golden suite pins the exact bytes of a few shapes and the
# property harness pins meaning. Neither can answer the question the
# width model is judged on: does the writer produce lines a human
# wants to read? This harness answers it with two numbers over
# examples/fmt/width/, the corpus written for exactly this purpose:
#
#   soft      over-budget lines the writer COULD have broken (its
#             longest unbreakable atom still fits).
#   collapse  lines the writer removed relative to the hand-wrapped
#             source: sum of max(0, lines(input) - lines(fmt(input))).
#             This is the axis `soft` cannot see — a four-line pipe
#             chain flattened to 65 characters is inside any budget and
#             still destroys the style the documentation teaches.
#
# Both must be ZERO: the writer breaks every line it can bring inside
# the budget, and it never joins a break the author wrote at one of its
# own break points. `soft_max` rides along and is zero with `soft`. The
# three values are also pinned in tools/fmt-width-baseline.txt, so a run
# that measures anything else fails and names the number that moved.
#
# Two fixtures are asserted on their own, because their whole point is
# that no writer may move them:
#
#   already_narrow      fits with room to spare — soft and collapse
#                       must be 0. The net against a writer that
#                       invents breaks.
#   unbreakable_atoms   every over-budget line is `hard` — soft must be
#                       0. The net against a metric that charges the
#                       writer for a long string literal.
#
# The width is a setting, so the corpus is also formatted at a second,
# narrower width (`--fmt-width 60`) with the same two requirements, and
# the `kai fmt --width N` / kai.toml `[fmt] width` plumbing is checked
# against the compiler flag it must reduce to.
#
# Env:
#   FMT_WIDTH=N          budget in columns (default: the baseline's)
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

# ---- pinned values ---------------------------------------------------
ratchet() {
  measured="$1"; base="$2"; label="$3"
  if [ "$measured" -ne "$base" ]; then
    echo "  FAIL $label — $measured, but tools/fmt-width-baseline.txt pins $base"
    return 1
  fi
  return 0
}

ratchet "$soft" "$base_soft" "soft" || fail=$((fail + 1))
ratchet "$soft_max" "$base_soft_max" "soft_max" || fail=$((fail + 1))
ratchet "$collapse" "$base_collapse" "collapse" || fail=$((fail + 1))

if [ "$soft" -ne 0 ] || [ "$collapse" -ne 0 ]; then
  echo "  FAIL width — soft=$soft collapse=$collapse; the writer must bring both to 0"
  fail=$((fail + 1))
fi

echo "fmt_width: budget $budget — soft=$soft (max $soft_max), hard=$hard, collapse=$collapse"

# ---- a second width: the setting has to hold, not just the default ---
narrow=60
mkdir -p "$tmp/narrow"
ncollapse=0
for input in "$CORPUS"/*.input.kai; do
  name=$(basename "$input" .input.kai)
  "$KAIC2" --path "$ROOT/stdlib" --fmt-width "$narrow" --fmt "$input" > "$tmp/narrow/$name.kai" 2>/dev/null || true
  in_lines=$(wc -l < "$input" | tr -d ' ')
  out_lines=$(wc -l < "$tmp/narrow/$name.kai" | tr -d ' ')
  lost=$((in_lines - out_lines))
  [ "$lost" -gt 0 ] || lost=0
  ncollapse=$((ncollapse + lost))
done
ntotals=$(cd "$tmp/narrow" && sh "$REPORT" --width "$narrow" --totals ./*.kai)
nsoft=$(echo "$ntotals" | sed -n 's/.*soft=\([0-9]*\).*/\1/p')
if [ "$nsoft" -ne 0 ] || [ "$ncollapse" -ne 0 ]; then
  echo "  FAIL width $narrow — soft=$nsoft collapse=$ncollapse; the writer must bring both to 0 at any width"
  fail=$((fail + 1))
fi
echo "fmt_width: budget $narrow — soft=$nsoft, collapse=$ncollapse"

# ---- plumbing: kai fmt --width and kai.toml [fmt] width ---------------
probe="$CORPUS/call_args.input.kai"
mkdir -p "$tmp/pkg/src"
printf 'name = "w"\nversion = "0.1.0"\n\n[fmt]\nwidth = 60\n' > "$tmp/pkg/kai.toml"
cp "$probe" "$tmp/pkg/src/a.kai"
"$KAIC2" --fmt-width 60 --fmt "$probe" > "$tmp/want60.kai"
"$KAIC2" --fmt-width 120 --fmt "$probe" > "$tmp/want120.kai"
# --check on a canonical file prints nothing (check-clean), so the
# plumbing probes drive the in-place rewrite path instead: write-back
# only fires when the width actually reached kaic2.
cp "$probe" "$tmp/pkg/src/a.kai"
"$ROOT/bin/kai" fmt "$tmp/pkg/src/a.kai" 2>/dev/null
if ! cmp -s "$tmp/pkg/src/a.kai" "$tmp/want60.kai"; then
  echo "  FAIL plumbing — kai fmt did not take width = 60 from the package's kai.toml [fmt] table"
  fail=$((fail + 1))
fi
cp "$probe" "$tmp/pkg/src/a.kai"
"$ROOT/bin/kai" fmt --width 120 "$tmp/pkg/src/a.kai" 2>/dev/null
if ! cmp -s "$tmp/pkg/src/a.kai" "$tmp/want120.kai"; then
  echo "  FAIL plumbing — kai fmt --width 120 did not override the manifest"
  fail=$((fail + 1))
fi
if ! "$ROOT/bin/kai" fmt --width 60 --stdin < "$probe" > "$tmp/got.kai" 2>/dev/null; then :; fi
if ! cmp -s "$tmp/got.kai" "$tmp/want60.kai"; then
  echo "  FAIL plumbing — kai fmt --stdin --width 60 differs from kaic2 --fmt-width 60"
  fail=$((fail + 1))
fi

if [ "$fail" -gt 0 ]; then
  [ -n "${VERBOSE:-}" ] || echo "$table"
  echo "fmt_width: $fail check(s) failed" >&2
  exit 1
fi
