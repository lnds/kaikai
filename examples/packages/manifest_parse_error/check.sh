#!/bin/sh
# examples/packages/manifest_parse_error — assert every kai-pkg
# subcommand bails on a broken kai.toml (issue #420). When the
# manifest fails to parse, `show`, `install`, `add`, and `update`
# must each:
#   1. exit non-zero
#   2. print a one-line diagnostic mentioning kai.toml + parse error
#
# Without this the parse failure is silently masked and `kai install`
# pretends the manifest has no deps, exits 0, and downstream
# `kai run` / `kai build` fail with a less-obvious error elsewhere.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

# A clearly invalid kai.toml — unterminated section header and
# unterminated string. The decoder rejects both.
cat > "$TMP/kai.toml" <<'TOML'
this is not valid toml at all
[broken
name = "x
TOML

# Snapshot the manifest so we can prove no command silently
# rewrote it on a parse failure.
cp "$TMP/kai.toml" "$TMP/kai.toml.before"

run_one() {
  label="$1"; shift
  set +e
  ( cd "$TMP" && KAIKAI_CACHE_ROOT="$TMP/cache" \
      "$ROOT/bin/kai" "$@" >"$TMP/stdout" 2>"$TMP/stderr" )
  status=$?
  set -e

  if [ "$status" -eq 0 ]; then
    echo "manifest_parse_error: FAIL ($label) — expected non-zero exit, got 0" >&2
    cat "$TMP/stdout" >&2
    cat "$TMP/stderr" >&2
    exit 1
  fi

  # The diagnostic may go to stdout or stderr depending on the
  # subcommand; the contract is that SOMEWHERE the user sees a
  # clear "kai.toml" + "parse error" pairing.
  if ! grep -q 'kai.toml' "$TMP/stdout" "$TMP/stderr" \
       || ! grep -q 'parse error' "$TMP/stdout" "$TMP/stderr"; then
    echo "manifest_parse_error: FAIL ($label) — missing 'kai.toml: parse error' diagnostic" >&2
    echo "--- stdout ---" >&2; cat "$TMP/stdout" >&2
    echo "--- stderr ---" >&2; cat "$TMP/stderr" >&2
    exit 1
  fi

  if ! diff -q "$TMP/kai.toml" "$TMP/kai.toml.before" >/dev/null; then
    echo "manifest_parse_error: FAIL ($label) — kai.toml mutated despite parse failure" >&2
    diff "$TMP/kai.toml.before" "$TMP/kai.toml" >&2
    exit 1
  fi

  echo "  $label: OK (exit=$status)"
}

run_one "show"    show
run_one "install" install
run_one "update"  update
run_one "add"     add github.com/foo/bar@main

echo "manifest_parse_error: OK (show/install/update/add all bail on parse error)"
exit 0
