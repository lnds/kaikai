#!/bin/sh
# examples/packages/unknown_dep_form — an unrecognised dependency
# table must fail loudly, never lock zero entries and pretend success.
# The silent no-op was what turned a manifest typo into a confusing
# `cannot open module` failure two commands later.
#
# Asserts, for a dep with neither `path` nor `source`/`ref` (nor the
# `git`/`tag` aliases):
#   1. `install` exits non-zero
#   2. stderr names the offending dependency
#   3. no kai.lock is written (no false success)

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

cat > "$TMP/kai.toml" <<'TOML'
name = "unknown_dep_form"
version = "0.1.0"

[dependencies]
foo = { registry = "crates.io", version = "1.0" }
TOML

set +e
( cd "$TMP" && KAIKAI_CACHE_ROOT="$TMP/cache" \
    "$ROOT/bin/kai" install >"$TMP/stdout" 2>"$TMP/stderr" )
status=$?
set -e

if [ "$status" -eq 0 ]; then
  echo "unknown_dep_form: FAIL — expected non-zero exit, got 0" >&2
  cat "$TMP/stdout" "$TMP/stderr" >&2
  exit 1
fi

if ! grep -q "unrecognised" "$TMP/stdout" "$TMP/stderr"; then
  echo "unknown_dep_form: FAIL — missing 'unrecognised' diagnostic naming the dep" >&2
  echo "--- stdout ---" >&2; cat "$TMP/stdout" >&2
  echo "--- stderr ---" >&2; cat "$TMP/stderr" >&2
  exit 1
fi

if [ -f "$TMP/kai.lock" ]; then
  echo "unknown_dep_form: FAIL — kai.lock written despite unrecognised dep" >&2
  cat "$TMP/kai.lock" >&2
  exit 1
fi

echo "unknown_dep_form: OK (install bails loudly, no false lock)"
exit 0
