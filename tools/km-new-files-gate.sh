#!/usr/bin/env bash
# The differential quality bar, executed: every NEW compiler/stdlib file
# on this branch scores at least B (the landing floor), with A− the
# stated target. Pre-existing files are debt being paid down elsewhere;
# this gate only judges what the branch itself authored, so "we're
# already at F" can never excuse a new monolith.
#
# `km` is a local tool (not on crates.io), so CI skips with a loud note
# and the gate binds wherever km exists — every dev and integrator
# machine, at tier0.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! command -v km >/dev/null 2>&1; then
  echo "km-new-files-gate SKIP — km not installed on this machine (binds locally only)"
  exit 0
fi

base=$(git merge-base HEAD origin/main 2>/dev/null || git rev-parse HEAD~1)
new_files=$(git diff --name-only --diff-filter=A "$base" HEAD -- 'stage2/compiler/*.kai' 'stdlib/*.kai' 'stdlib/**/*.kai' 2>/dev/null || true)

if [ -z "$new_files" ]; then
  echo "km-new-files-gate OK — no new compiler/stdlib files on this branch"
  exit 0
fi

fails=0
for f in $new_files; do
  [ -f "$f" ] || continue
  grade=$(km score "$f" 2>/dev/null | grep "Project Score:" | sed -E 's/.*Score: *([A-F+-]+).*/\1/')
  loc=$(km score "$f" 2>/dev/null | grep "Total LOC:" | sed 's/.*: *//')
  case "$grade" in
    A*|B)
      note=""
      [ "$grade" = "B" ] && note="  (floor; the stated bar is A−)"
      echo "  $grade  $f  ($loc LOC)$note"
      ;;
    *)
      echo "  $grade  $f  ($loc LOC)  << BELOW THE FLOOR — split or simplify before landing"
      fails=$((fails + 1))
      ;;
  esac
done

if [ "$fails" -gt 0 ]; then
  echo "km-new-files-gate FAIL — $fails new file(s) below B; the differential bar is not optional"
  exit 1
fi
echo "km-new-files-gate OK — every new file at or above the floor"
