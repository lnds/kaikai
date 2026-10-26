#!/bin/sh
# tests/fmt_selfhost.sh — `kai fmt` self-hosting ratchet.
#
# For every source file in the corpus: fmt(file) exits 0, and
# fmt(fmt(file)) == fmt(file) byte for byte. A file that only stabilises
# after more than one pass is a formatter bug.
#
# `kai fmt <file>` rewrites in place; this gate drives `kaic2 --fmt`,
# which prints to stdout, so the sources are never touched and every
# pass lands in a private `mktemp -d` scratch dir.
#
# SKIP lists repo-relative paths waived from the idempotency check only
# (each still has to format). It must stay empty in steady state.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; fmt reproduces the surface
# of the edition the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"

SKIP=""

if [ ! -x "$KAIC2" ]; then
  echo "fmt_selfhost: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

is_skipped() {
  for s in $SKIP; do
    [ "$1" = "$s" ] && return 0
  done
  return 1
}

fail=0
pass=0
skip=0
n=0

# Corpus: every stdlib source (recursive) + the stage2 compiler tree + the
# `kai` driver sources.
# `find` keeps the list shell-independent — no glob-expansion surprises.
corpus="$(find "$ROOT/stdlib" -name '*.kai' -type f; find "$ROOT/stage2/compiler" -name '*.kai' -type f; find "$ROOT/tools/kai" -name '*.kai' -type f)"

for abs in $corpus; do
  rel="${abs#"$ROOT"/}"
  n=$((n + 1))
  p1="$tmp/$n.p1"
  p2="$tmp/$n.p2"
  err="$tmp/$n.err"

  if ! "$KAIC2" $EDITION_FLAG --fmt "$abs" > "$p1" 2> "$err"; then
    echo "  FAIL $rel — fmt refused:"
    sed 's/^/      /' "$err"
    fail=$((fail + 1))
    continue
  fi

  if is_skipped "$rel"; then
    echo "  SKIP $rel — known non-idempotent (see skip-list)"
    skip=$((skip + 1))
    continue
  fi

  if ! "$KAIC2" $EDITION_FLAG --fmt "$p1" > "$p2" 2> "$err"; then
    echo "  FAIL $rel — fmt refused on second pass:"
    sed 's/^/      /' "$err"
    fail=$((fail + 1))
    continue
  fi

  if ! diff -u "$p1" "$p2" > "$tmp/$n.diff"; then
    echo "  FAIL $rel — fmt is not idempotent (fmt != fmt(fmt)):"
    sed 's/^/      /' "$tmp/$n.diff"
    fail=$((fail + 1))
    continue
  fi

  pass=$((pass + 1))
done

if [ "$fail" -gt 0 ]; then
  echo "fmt_selfhost: $pass passed, $skip skipped, $fail failed"
  exit 1
fi
echo "fmt_selfhost: $pass passed, $skip skipped, 0 failed"
