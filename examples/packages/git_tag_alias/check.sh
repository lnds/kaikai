#!/bin/sh
# examples/packages/git_tag_alias — the documented `{ git = ...,
# tag = ... }` manifest form must resolve end-to-end. It once wrote
# an empty lock and left the build to fail with `cannot open module`
# two commands later; `git`/`tag` are now accepted aliases of
# `source`/`ref`.
#
# Asserts: install resolves 1 entry (not a silent 0-entry lock),
# and the program builds + runs against the git-sourced dep.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

if [ ! -f "$DIR/kai.toml" ]; then
  echo "git_tag_alias: missing kai.toml — run examples/packages/render-fixtures.sh" >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

# Clean state — no lock, isolated cache.
rm -f "$DIR/kai.lock"
KAIKAI_CACHE="$TMP/cache"
export KAIKAI_CACHE

out="$(cd "$DIR" && "$ROOT/bin/kai" run . 2>&1)"

if [ ! -f "$DIR/kai.lock" ]; then
  echo "git_tag_alias: FAIL — kai.lock was not created" >&2
  echo "$out" >&2
  exit 1
fi

# The lock must pin the greet dependency, proving the git/tag alias
# actually resolved instead of silently locking zero entries.
if ! grep -q 'name = "greet"' "$DIR/kai.lock"; then
  echo "git_tag_alias: FAIL — greet not in kai.lock (git/tag alias silently ignored?)" >&2
  cat "$DIR/kai.lock" >&2
  exit 1
fi

expected="$(cat "$DIR/main.out.expected")"
actual="$(echo "$out" | tail -n 1)"
if [ "$actual" != "$expected" ]; then
  echo "git_tag_alias: FAIL — output mismatch" >&2
  echo "expected: $expected" >&2
  echo "actual:   $actual" >&2
  exit 1
fi

echo "git_tag_alias: OK (git/tag alias resolved; build + run match)"
rm -f "$DIR/kai.lock"
exit 0
