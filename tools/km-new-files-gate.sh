#!/usr/bin/env bash
# The differential quality bar, executed: every NEW compiler/stdlib file
# on this branch scores at least B (the landing floor), with A- the
# stated target. Pre-existing files are debt being paid down elsewhere;
# this gate only judges what the branch itself authored, so "we're
# already at F" can never excuse a new monolith.
#
# The floor is the whole B band: B-, B and B+ all land, as does every
# A grade (A++ through A-). Anything below B- does not. Modifiers are
# part of the grade, so the accepted set is spelled out rather than
# matched by prefix - an earlier `A*|B)` accepted a bare B while
# rejecting the better B+. An empty grade means km failed, which is a
# failure to judge, not a pass.
#
# `km` ships in the `kimun` crate on crates.io, pinned to one version by
# the tier0 workflow so a kimun release cannot redden a PR that touched
# no code. A developer without it gets a skip; CI does not, because a
# gate that can quietly skip is not a gate.

set -eu

# Keep in step with the pin in .github/workflows/tier1.yml.
KIMUN_VERSION=0.24.0

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! command -v km >/dev/null 2>&1; then
  if [ -n "${CI:-}${GITHUB_ACTIONS:-}" ]; then
    echo "km-new-files-gate FAIL - km is missing in CI, so the differential bar went unjudged"
    echo "  install it with: cargo install kimun --version $KIMUN_VERSION --locked"
    exit 1
  fi
  echo "km-new-files-gate SKIP - km not installed on this machine (install: cargo install kimun)"
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
    A++|A+|A|A-)
      echo "  $grade  $f  ($loc LOC)"
      ;;
    B+|B|B-)
      echo "  $grade  $f  ($loc LOC)  (floor; the stated bar is A-)"
      ;;
    "")
      echo "  ??  $f  ($loc LOC)  << km score produced no grade"
      fails=$((fails + 1))
      ;;
    *)
      echo "  $grade  $f  ($loc LOC)  << BELOW THE FLOOR - split or simplify before landing"
      fails=$((fails + 1))
      ;;
  esac
done

if [ "$fails" -gt 0 ]; then
  echo "km-new-files-gate FAIL - $fails new file(s) below the B floor; the differential bar is not optional"
  exit 1
fi
echo "km-new-files-gate OK — every new file at or above the floor"
