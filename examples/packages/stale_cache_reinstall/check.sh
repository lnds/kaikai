#!/bin/sh
# examples/packages/stale_cache_reinstall — guard the wrapper's
# stale-dependency-cache check.
#
# The lock pins a git dep whose cache dir is then deleted. The
# wrapper must notice the dir is gone and re-install, rather than
# concluding the cache is fresh and letting the build fail
# downstream. Two assertions, because the check has failed in two
# distinct ways:
#
#   1. It must not emit awk diagnostics. The detector mirrors the
#      slug logic in an awk regex literal; an unescaped `/` inside
#      its bracket expression parses under gawk but not under BSD
#      awk (macOS), where awk then exits non-zero with no output.
#   2. It must actually re-populate the cache. A detector that
#      aborts silently reports "fresh" unconditionally, so the
#      build regresses without any visible error.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

if [ ! -f "$DIR/kai.toml" ]; then
  echo "stale_cache_reinstall: missing kai.toml — run examples/packages/render-fixtures.sh" >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "$DIR/kai.lock"' EXIT INT TERM

rm -f "$DIR/kai.lock"
KAIKAI_CACHE="$TMP/cache"
export KAIKAI_CACHE

# First build populates the cache and writes the lock.
if ! out="$(cd "$DIR" && "$ROOT/bin/kai" run . 2>&1)"; then
  echo "stale_cache_reinstall: FAIL — priming build failed" >&2
  echo "$out" >&2
  exit 1
fi

if [ ! -f "$DIR/kai.lock" ]; then
  echo "stale_cache_reinstall: FAIL — priming build wrote no kai.lock" >&2
  exit 1
fi

# Prune the cache while keeping the lock: exactly the state a user
# lands in after clearing ~/Library/Caches/kai (or ~/.cache/kai).
rm -rf "$KAIKAI_CACHE"

if ! out="$(cd "$DIR" && "$ROOT/bin/kai" run . 2>&1)"; then
  echo "stale_cache_reinstall: FAIL — rebuild with pruned cache failed" >&2
  echo "$out" >&2
  exit 1
fi

# Assertion 1: the detector ran cleanly.
if echo "$out" | grep -q '^awk:'; then
  echo "stale_cache_reinstall: FAIL — awk diagnostics leaked into build output" >&2
  echo "$out" >&2
  exit 1
fi

# Assertion 2: the detector fired and the dep was re-fetched.
if [ ! -d "$KAIKAI_CACHE" ]; then
  echo "stale_cache_reinstall: FAIL — cache not repopulated; stale check never fired" >&2
  exit 1
fi

expected="$(cat "$DIR/main.out.expected")"
actual="$(echo "$out" | tail -n 1)"
if [ "$actual" != "$expected" ]; then
  echo "stale_cache_reinstall: FAIL — output mismatch" >&2
  echo "expected: $expected" >&2
  echo "actual:   $actual" >&2
  exit 1
fi

echo "stale_cache_reinstall: OK (pruned cache detected and re-installed; no awk noise)"
exit 0
