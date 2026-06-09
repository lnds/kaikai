#!/bin/sh
# tests/fmt_selfhost.sh — `kai fmt` self-hosting ratchet (issue #786).
#
# `kai fmt` round-trips the full language surface and refuses nothing
# across stdlib/ + stage2/compiler/ (#784). This gate locks that
# coverage in so it cannot regress: for every source file in the
# corpus we require
#
#   1. fmt(file)        exits 0          — no refusal, AND
#   2. fmt(fmt(file)) == fmt(file)        — byte-identical idempotency.
#
# The first pass is the canonical form; the second pass must reproduce
# it byte-for-byte. A file that only stabilises after N>1 passes is a
# formatter bug, not a pass.
#
# `kai fmt` writes to stdout (it is NOT destructive in-place), so the
# real sources are never mutated — every pass redirects into a private
# scratch dir created with `mktemp -d`. Each file's scratch artefacts
# are keyed by a per-file counter, so a parallel `make -j` invocation
# of sibling targets cannot collide (each target gets its own mktemp).
#
# Skip-list: a space-separated list of repo-relative paths that are
# known non-idempotent and tracked by a GitHub issue. It MUST stay
# empty in steady state; any entry needs an inline `# refs #N` note.
# A skipped file is still required to PARSE (fmt must exit 0) — only
# the idempotency check is waived — so the skip cannot silently mask a
# total refusal.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# Known non-idempotent files, each tagged with its tracking issue.
# Empty today (#785's trailing-comment edge was fixed in #788).
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

# Corpus: every stdlib source (recursive) + the stage2 compiler tree.
# `find` keeps the list shell-independent — no glob-expansion surprises.
corpus="$(find "$ROOT/stdlib" -name '*.kai' -type f; find "$ROOT/stage2/compiler" -name '*.kai' -type f)"

for abs in $corpus; do
  rel="${abs#"$ROOT"/}"
  n=$((n + 1))
  p1="$tmp/$n.p1"
  p2="$tmp/$n.p2"
  err="$tmp/$n.err"

  if ! "$KAIC2" --fmt "$abs" > "$p1" 2> "$err"; then
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

  if ! "$KAIC2" --fmt "$p1" > "$p2" 2> "$err"; then
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
