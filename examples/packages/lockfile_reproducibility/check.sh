#!/bin/sh
# examples/packages/lockfile_reproducibility — assert two
# `kai install` runs from the same manifest produce byte-identical
# kai.lock files (same SHAs).
#
# Setup: tests/fixtures/git-fixtures/setup.sh + render-fixtures.sh.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

if [ ! -f "$DIR/kai.toml" ]; then
  echo "lockfile_reproducibility: missing kai.toml — run examples/packages/render-fixtures.sh" >&2
  exit 2
fi

# Run install twice in a clean cache; capture each lock.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

KAIKAI_CACHE="$TMP/cache-1"
export KAIKAI_CACHE
( cd "$DIR" && rm -f kai.lock && "$ROOT/bin/kai" install >/dev/null )
cp "$DIR/kai.lock" "$TMP/lock-1"

KAIKAI_CACHE="$TMP/cache-2"
export KAIKAI_CACHE
( cd "$DIR" && rm -f kai.lock && "$ROOT/bin/kai" install >/dev/null )
cp "$DIR/kai.lock" "$TMP/lock-2"

if diff -q "$TMP/lock-1" "$TMP/lock-2" >/dev/null; then
  echo "lockfile_reproducibility: OK (same SHA across two clean caches)"
  rm -f "$DIR/kai.lock"
  exit 0
else
  echo "lockfile_reproducibility: DIFF" >&2
  diff "$TMP/lock-1" "$TMP/lock-2" >&2
  exit 1
fi
