#!/bin/bash
# Read-table audit for the ownership passes.
#
# stage2/compiler/perceus_reads.kai builds the one table of binder reads
# (`pcs_read_table`) and answers every question about it. Two ways a second
# derivation can creep back in, both checked here:
#
# - A perceus-family file other than perceus_reads.kai spelling a `U(...)`
#   read entry: building or scanning the table by hand instead of asking
#   perceus_reads.kai.
# - A new function calling `pcs_read_table`: every caller re-derives the
#   table for its own body. The callers are pinned in the baseline, one
#   `<file> <fn>` per line; a caller not listed fails, and a listed caller
#   that no longer calls fails too (shrink the baseline in that commit).
#
# `--self-test` injects each violation into a scratch copy and asserts the
# audit rejects it.
set -u
cd "$(dirname "$0")/.."

# audit <root>: the checks over the tree rooted there.
audit() {
  local ROOT="$1" status=0 observed hand
  local BASELINE="$ROOT/tools/perceus-read-baseline.txt"
  hand=$(grep -n '\bU(' "$ROOT"/stage2/compiler/perceus*.kai 2>/dev/null \
         | grep -v '/perceus_reads\.kai:' | grep -v ':[0-9]*:[[:space:]]*#')
  if [ -n "$hand" ]; then
    echo "perceus-read-audit FAIL — read entries handled outside perceus_reads.kai:"
    echo "$hand" | sed "s|$ROOT/||; s/^/  /"
    status=1
  fi
  observed=$(mktemp)
  for f in "$ROOT"/stage2/compiler/*.kai; do
    grep -n 'pcs_read_table(' "$f" | grep -v 'pub fn pcs_read_table' | cut -d: -f1 | while read -r ln; do
      awk -v t="$ln" 'NR<=t && /^(pub )?fn /{n=$2; sub(/\(.*/, "", n)} NR==t{print n; exit}' "$f"
    done | sort -u | sed "s|^|$(basename "$f") |"
  done | sort > "$observed"
  local new gone
  new=$(comm -13 <(grep -v '^#' "$BASELINE" | sort) "$observed")
  gone=$(comm -23 <(grep -v '^#' "$BASELINE" | sort) "$observed")
  if [ -n "$new" ]; then
    echo "perceus-read-audit FAIL — new read-table derivation site(s):"
    echo "$new" | sed 's/^/  /'
    echo "Read the table the pass already built instead of collecting another."
    status=1
  fi
  if [ -n "$gone" ]; then
    echo "perceus-read-audit FAIL — stale baseline entry(ies), shrink $(basename "$BASELINE"):"
    echo "$gone" | sed 's/^/  /'
    status=1
  fi
  rm -f "$observed"
  return $status
}

self_test() {
  local tmp rc
  tmp=$(mktemp -d)
  mkdir -p "$tmp/tools" "$tmp/stage2/compiler"
  cp tools/perceus-read-baseline.txt "$tmp/tools/"
  cp stage2/compiler/*.kai "$tmp/stage2/compiler/"
  printf '\nfn pra_probe(e: Expr) : [Use] / Console = pcs_read_table(e, [])\n' >> "$tmp/stage2/compiler/perceus_payer.kai"
  audit "$tmp" > /dev/null; rc=$?
  cp stage2/compiler/perceus_payer.kai "$tmp/stage2/compiler/"
  printf '\nfn pra_probe(u: Use) : String = match u { U(n, _, _, _) -> n }\n' >> "$tmp/stage2/compiler/perceus_tail_drop.kai"
  audit "$tmp" > /dev/null; local rc2=$?
  rm -rf "$tmp"
  if [ $rc -eq 0 ] || [ $rc2 -eq 0 ]; then echo "perceus-read-audit self-test FAIL — an injected derivation passed"; return 1; fi
  echo "perceus-read-audit self-test OK"
}

if [ "${1:-}" = --self-test ]; then self_test || exit 1; fi
audit "$(pwd)" || exit 1
echo "perceus-read-audit OK ($(grep -vc '^#' tools/perceus-read-baseline.txt) pinned read-table sites)"
