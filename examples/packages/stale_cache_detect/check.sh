#!/bin/sh
# examples/packages/stale_cache_detect — a lock that names a cache dir
# which is no longer there must trigger a re-resolve, silently and
# without awk diagnostics.
#
# The sibling auto_install fixture covers the no-lock arm. This one
# covers the arm the wrapper walks the lock for, which is where an awk
# regex that BSD awk rejects made the check fail open: awk exited
# non-zero with no output, the read loop saw no lines, and the wrapper
# concluded "cache is fresh" unconditionally. The build still succeeded,
# so asserting only on the exit status proves nothing — both assertions
# below are load-bearing.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

if [ ! -f "$DIR/kai.toml" ]; then
  echo "stale_cache_detect: missing kai.toml — run examples/packages/render-fixtures.sh" >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "$DIR/kai.lock"' EXIT INT TERM

KAIKAI_CACHE="$TMP/cache"
export KAIKAI_CACHE

# Round 1 — resolve from scratch so the lock names a real cache dir.
rm -f "$DIR/kai.lock"
if ! first="$(cd "$DIR" && "$ROOT/bin/kai" run . 2>&1)"; then
  echo "stale_cache_detect: FAIL — initial resolve failed" >&2
  echo "$first" >&2
  exit 1
fi
if [ ! -f "$DIR/kai.lock" ]; then
  echo "stale_cache_detect: FAIL — no kai.lock after the initial resolve" >&2
  exit 1
fi

# Prune the cache, keeping the lock: exactly the state a user reaches
# when the OS reclaims ~/Library/Caches, and the state the lock walk
# exists to notice.
rm -rf "$TMP/cache"

if ! out="$(cd "$DIR" && "$ROOT/bin/kai" run . 2>&1)"; then
  echo "stale_cache_detect: FAIL — build with a pruned cache failed" >&2
  echo "$out" >&2
  exit 1
fi

# (1) No awk diagnostic. A regex the host awk rejects lands here.
if echo "$out" | grep -qi "awk"; then
  echo "stale_cache_detect: FAIL — awk diagnostic in build output" >&2
  echo "$out" >&2
  exit 1
fi

# (2) The pruned dep was actually re-resolved. If the lock walk failed
# open, nothing re-fetched it and the cache stays empty.
if [ ! -d "$TMP/cache" ]; then
  echo "stale_cache_detect: FAIL — pruned cache not re-resolved (lock walk failed open)" >&2
  exit 1
fi

expected="$(cat "$DIR/main.out.expected")"
actual="$(echo "$out" | tail -n 1)"
if [ "$actual" != "$expected" ]; then
  echo "stale_cache_detect: FAIL — output mismatch" >&2
  echo "expected: $expected" >&2
  echo "actual:   $actual" >&2
  exit 1
fi

echo "stale_cache_detect: OK (pruned cache re-resolved; no awk diagnostic)"
exit 0
