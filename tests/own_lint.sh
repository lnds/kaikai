#!/bin/sh
# `kai lint` reports on the files of the package it lints and on no other:
# none of a stdlib module or a dependency they import. A module of the
# package is reported on once, whether the package has an entry or not.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -R "$ROOT/tests/own_lint" "$WORK/pkgs"
fail=0

# The run's findings as `<file> <rule>`, one per line, sorted: `want` is what the package owns.
expect() {
  label="$1"; want="$2"; got="$3"
  if [ "$got" != "$want" ]; then
    echo "own-lint FAIL ($label)"; echo "  want:"; echo "$want" | sed 's/^/    /'
    echo "  got:"; echo "$got" | sed 's/^/    /'; fail=1
  fi
}

run() { pkg="$1"; shift; (cd "$WORK/pkgs/$pkg" && XDG_CACHE_HOME="$WORK/cache" "$KAI" "$@" 2> "$WORK/err"); }

text() { run "$@" | sed -n 's/^\([^:]*\):.* \[\([a-z_]*\)\]$/\1 \2/p' | sed 's|^.*/pkgs/||' | sort; }

json() {
  run "$@" | tr '{' '\n' | sed -n 's/^"file": "\([^"]*\)".*"rule": "\([^"]*\)".*/\1 \2/p' | sed 's|^.*/pkgs/||' | sort
}

APP="app/helper.kai list_concat_literal_to_spread
app/main.kai redundant_if_bool"

expect "package" "$APP" "$(text app lint .)"
expect "entry file" "$APP" "$(text app lint main.kai)"
expect "package --json" "$APP" "$(json app lint --json .)"

# The dependency does trip the rules: its own lint reports them.
expect "dependency" "deplib/deplib.kai list_concat_literal_to_spread
deplib/deplib.kai match_option_to_combinator
deplib/deplib.kai redundant_if_bool" "$(text deplib lint .)"

expect "library" "lib/one.kai redundant_if_bool
lib/two.kai list_concat_literal_to_spread" "$(text lib lint .)"
[ "$fail" = 0 ] && echo "own-lint OK"
exit "$fail"
