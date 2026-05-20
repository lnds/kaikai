#!/bin/sh
# examples/packages/auto_install — regression guard for #512 and
# the auto-install path. Assert that a clean repo (no kai.lock,
# empty cache) gets installed automatically on the first
# `kai build`.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

if [ ! -f "$DIR/kai.toml" ]; then
  echo "auto_install: missing kai.toml — run examples/packages/render-fixtures.sh" >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

# Clean state — no lock, isolated cache.
rm -f "$DIR/kai.lock"
KAIKAI_CACHE="$TMP/cache"
export KAIKAI_CACHE

# Build must succeed AND produce the lockfile from scratch.
out="$(cd "$DIR" && "$ROOT/bin/kai" run . 2>&1)"
exit_code=$?

if [ $exit_code -ne 0 ]; then
  echo "auto_install: FAIL — kai run exited $exit_code" >&2
  echo "$out" >&2
  exit 1
fi

if [ ! -f "$DIR/kai.lock" ]; then
  echo "auto_install: FAIL — kai.lock was not created" >&2
  exit 1
fi

expected="$(cat "$DIR/main.out.expected")"
actual="$(echo "$out" | tail -n 1)"
if [ "$actual" != "$expected" ]; then
  echo "auto_install: FAIL — output mismatch" >&2
  echo "expected: $expected" >&2
  echo "actual:   $actual" >&2
  exit 1
fi

echo "auto_install: OK (kai.lock created on first build; output matches)"
rm -f "$DIR/kai.lock"
exit 0
