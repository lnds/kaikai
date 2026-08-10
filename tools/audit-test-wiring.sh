#!/bin/sh
# tools/audit-test-wiring.sh — structural guard against imaginary
# coverage: a test target or fixture directory that exists but no tier
# ever runs. A gate that does not run is worse than none — it reads as
# coverage while the corpus underneath rots.
#
# Checks, all textual (no compiler needed, tier0-cheap):
#
#   A. Every `test-*` target defined in Makefile / stage2/Makefile is
#      referenced somewhere outside its own definition, `.PHONY`, and
#      comments — by an aggregate list, another recipe, a workflow, or
#      a tools/ script.
#   B. Every examples/ level-1 directory containing .kai fixtures is
#      mentioned by some harness source (Makefiles, tools/, workflows).
#   C. Every examples/packages/ fixture directory is named by some
#      harness (tools/test-packages.sh enumerates fixtures by hand).
#   D. Target allowlist entries must still be unwired — a wired one is
#      stale and fails, so the target list self-cleans. Checked with A.
#
# Known limit: a mention is necessary, not sufficient — a harness whose
# glob fails to descend into a subdirectory of a mentioned tree is not
# detected here. Closing that would couple this audit to each harness's
# enumeration semantics; the per-harness globs are the place to fix it.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ALLOWLIST="tools/test-wiring-allowlist.txt"
MAKEFILES="Makefile stage2/Makefile"
FAIL=0

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail()  { echo "audit-test-wiring FAIL: $1"; FAIL=1; }
allow() { grep -qE "^$1:$2:" "$ALLOWLIST"; }

# Two reference corpora. For targets: Makefile lines that are not a
# test-* definition, not .PHONY, comments stripped — so a target named
# only by .PHONY or a comment still counts as orphaned. For fixture
# dirs: the full harness sources, comments stripped. Workflows and
# tools/ scripts count for both (this audit and its allowlist excluded).
cat .github/workflows/*.yml > "$tmp/shared"
find tools -name '*.sh' ! -name 'audit-test-wiring.sh' -exec cat {} + >> "$tmp/shared"

grep -hE '\btest-[a-z0-9-]+' $MAKEFILES \
  | grep -vE '^test-[a-z0-9-]+:' \
  | grep -v '^\.PHONY' \
  | sed 's/#.*//' > "$tmp/refs" || true
cat "$tmp/shared" >> "$tmp/refs"

sed 's/#.*//' $MAKEFILES > "$tmp/refs_all"
cat "$tmp/shared" >> "$tmp/refs_all"

target_wired() { grep -qE "(^|[^a-z0-9-])$1(\$|[^a-z0-9-])" "$tmp/refs"; }
dir_wired() {
  grep -qF "$1" "$tmp/refs_all" \
    || grep -qE "(^|[^a-zA-Z0-9_])${1#examples/}([^a-zA-Z0-9_]|\$)" "$tmp/refs_all"
}

# A wired entry must be absent from the allowlist (stale check); an
# unwired one must be present (orphan check).
check_stale() {
  ! allow "$1" "$2" \
    || fail "stale allowlist entry '$2' (now wired — drop it from $ALLOWLIST)"
}

check_target() {
  if target_wired "$1"; then check_stale target "$1"; return; fi
  allow target "$1" \
    || fail "target '$1' is defined but no tier, workflow, or script runs it"
}

# Dir allowlist entries do not expire: short basenames (e.g. lsp) match
# incidental words, so textual staleness would misfire on them.
check_dir() {
  dir_wired "$1" || allow dir "$1" \
    || fail "$1 holds .kai fixtures but no harness mentions it"
}

# --- A + D: orphan targets / stale target allowlist ---
grep -hE '^test-[a-z0-9-]+:' $MAKEFILES | sed 's/:.*//' | sort -u > "$tmp/defined"
while read -r t; do
  check_target "$t"
done < "$tmp/defined"

# --- B: fixture dirs no harness mentions ---
for d in examples/*/; do
  d="${d%/}"
  find "$d" -name '*.kai' -print -quit | grep -q . || continue
  check_dir "$d"
done

# --- C: package fixtures no harness enumerates ---
for d in examples/packages/*/; do
  ls "$d"kai.toml "$d"kai.toml.template "$d"check.sh >/dev/null 2>&1 || continue
  grep -qF "$(basename "$d")" "$tmp/refs_all" \
    || fail "examples/packages/$(basename "$d") is enumerated by no harness (tools/test-packages.sh lists fixtures by hand)"
done

[ "$FAIL" -eq 0 ] || exit 1
echo "audit-test-wiring OK — every test-* target and fixture dir is reachable from a tier (allowlist: $(grep -cE '^(target|dir):' "$ALLOWLIST") entries)"
