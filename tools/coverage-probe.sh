#!/bin/bash
#
# Coverage probe — for each `## ` heading in the runtime / language
# design docs, look for at least one fixture under `examples/effects/`
# whose comment block names the heading. Headings without a fixture
# are reported as **coverage gaps**.
#
# This is preventive: when a doc grows a new section but no fixture
# follows, the probe catches it before someone hits the bug at
# runtime. A heading is allowed to lack a fixture by adding the
# literal marker `<!-- coverage: skip -->` on the line right after
# the heading (e.g. design discussion, references, motivation —
# things that are not testable features).
#
# Exit code:
#   0   no gaps (or all gaps are skip-marked)
#   1   one or more unmarked gaps
#
# Pinned in docs/testing-tiers.md §"Coverage probe".

set -euo pipefail
cd "$(dirname "$0")/.."

DOCS=(
  docs/effects.md
  docs/effects-impl.md
  docs/effects-stdlib.md
  docs/structured-concurrency.md
  docs/actors.md
  docs/fibers-impl.md
)

# Sections that document non-testable concerns (motivation, references,
# design rationale). Listed here as a fast filter so the probe doesn't
# alarm on every "## Motivation" / "## References" heading. To opt a
# section *out* of this filter (i.e. require a fixture), prefix the
# heading with a feature word.
TRIVIAL_PATTERNS=(
  '^Motivation$'
  '^References$'
  '^Context$'
  '^Open questions$'
  '^Next steps$'
  '^Out of scope for v1$'
  '^Out of scope$'
  '^Non-goals$'
  '^Glossary$'
  '^Notation$'
  '^Status .*$'
  '^Phasing$'
  '^Implementation notes$'
)

is_trivial() {
  local heading="$1"
  for pat in "${TRIVIAL_PATTERNS[@]}"; do
    if [[ "$heading" =~ $pat ]]; then
      return 0
    fi
  done
  return 1
}

# Slugify a heading into a fixture-prefix candidate. "## Mailbox policies"
# → "mailbox" / "Bounded(_, BlockSender)" → "bounded" / etc. We do a
# loose match — any fixture whose comment mentions any of the heading's
# significant words counts.
slugify() {
  echo "$1" | tr '[:upper:]' '[:lower:]' \
    | sed -E 's/[^a-z0-9]+/ /g' \
    | tr -s ' '
}

# Collect every heading with its source file.
headings_file="$(mktemp)"
trap 'rm -f "$headings_file"' EXIT

for doc in "${DOCS[@]}"; do
  if [ ! -f "$doc" ]; then continue; fi
  # Extract `## ` headings (depth 2 only — we don't probe sub-sections).
  awk -v doc="$doc" '
    /^## / {
      sub(/^## /, "", $0)
      print doc "\t" $0
      next
    }
  ' "$doc"
done > "$headings_file"

gap_count=0
total_count=0
skip_count=0

while IFS=$'\t' read -r doc heading; do
  total_count=$((total_count + 1))

  # Skip-marker check: does the line right after the heading contain
  # `<!-- coverage: skip -->`? grep with -A 1 then look for marker.
  if grep -A 1 "^## ${heading}$" "$doc" 2>/dev/null | grep -q '<!-- coverage: skip -->'; then
    skip_count=$((skip_count + 1))
    continue
  fi

  if is_trivial "$heading"; then
    skip_count=$((skip_count + 1))
    continue
  fi

  # Look for a fixture whose comment block mentions any significant
  # word from the heading. We match on the longest word in the
  # heading (≥ 4 chars) to keep noise low.
  slug=$(slugify "$heading")
  longest=$(echo "$slug" | tr ' ' '\n' | awk '{ if (length($0) >= 4) print }' | awk '{ print length($0) "\t" $0 }' | sort -nr | head -1 | cut -f2)

  if [ -z "$longest" ]; then
    skip_count=$((skip_count + 1))
    continue
  fi

  found=0
  for fx in examples/effects/*.kai examples/holes/*.kai examples/sugars/*.kai examples/stdlib/*.kai; do
    [ -f "$fx" ] || continue
    # Look in the comment block (first 30 lines) and in the filename.
    if head -30 "$fx" 2>/dev/null | grep -iq "$longest" \
       || basename "$fx" | grep -iq "$longest"; then
      found=1
      break
    fi
  done

  if [ "$found" -eq 0 ]; then
    echo "GAP  $doc  ## $heading  (no fixture matching '$longest')"
    gap_count=$((gap_count + 1))
  fi
done < "$headings_file"

echo
echo "coverage-probe: $total_count headings inspected, $skip_count skipped, $gap_count gaps"

# Baseline policy: gap count is checked against a baseline file. New
# gaps fail the probe; closing gaps (count below baseline) succeeds
# and suggests a baseline bump. The baseline starts at the current
# count and ratchets down as gaps are closed.
baseline_file="tools/coverage-baseline.txt"
baseline=0
if [ -f "$baseline_file" ]; then
  baseline=$(cat "$baseline_file")
fi

if [ "$gap_count" -gt "$baseline" ]; then
  echo "coverage-probe FAIL — gap count $gap_count > baseline $baseline"
  echo "fix path: add a fixture under examples/effects/ whose comment"
  echo "          mentions the heading's keyword, OR add the marker"
  echo "          '<!-- coverage: skip -->' on the line after the"
  echo "          heading if it documents a non-testable concern."
  exit 1
fi

if [ "$gap_count" -lt "$baseline" ]; then
  echo "coverage-probe OK (improved) — gaps $gap_count < baseline $baseline"
  echo "  suggest: echo $gap_count > $baseline_file"
else
  echo "coverage-probe OK — gaps at baseline $baseline"
fi
