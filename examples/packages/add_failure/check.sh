#!/bin/sh
# examples/packages/add_failure — assert `kai add` is atomic on
# git-clone failure (issue #418). A bad source must:
#   1. exit non-zero
#   2. leave kai.toml unchanged
#   3. leave kai.lock either absent or in a state consistent with
#      kai.toml (every dep in the manifest pinned in the lock).

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

# A pristine project with no deps yet.
( cd "$TMP" && "$ROOT/bin/kai" init add_failure_demo >/dev/null )
cp "$TMP/kai.toml" "$TMP/kai.toml.before"

# A clearly invalid source — file:/// with a path that does not
# exist makes git fail fast without network access.
BAD_SRC="file:///nonexistent/kaikai/add-failure-fixture"

set +e
( cd "$TMP" && KAIKAI_CACHE_ROOT="$TMP/cache" \
    "$ROOT/bin/kai" add "$BAD_SRC" >"$TMP/stdout" 2>"$TMP/stderr" )
status=$?
set -e

if [ "$status" -eq 0 ]; then
  echo "add_failure: FAIL — expected non-zero exit, got 0" >&2
  cat "$TMP/stdout" >&2
  cat "$TMP/stderr" >&2
  exit 1
fi

if ! diff -q "$TMP/kai.toml" "$TMP/kai.toml.before" >/dev/null; then
  echo "add_failure: FAIL — kai.toml mutated despite clone failure" >&2
  diff "$TMP/kai.toml.before" "$TMP/kai.toml" >&2
  exit 1
fi

# Lockfile invariant: every git dep declared in kai.toml has an
# entry in kai.lock. With zero deps, an absent or empty lock is
# fine; a non-empty lock would prove drift.
if [ -f "$TMP/kai.lock" ]; then
  if grep -q '^\[\[package\]\]' "$TMP/kai.lock"; then
    echo "add_failure: FAIL — kai.lock has [[package]] entries but kai.toml has none" >&2
    cat "$TMP/kai.lock" >&2
    exit 1
  fi
fi

echo "add_failure: OK (exit=$status, manifest unchanged, lock consistent)"
exit 0
