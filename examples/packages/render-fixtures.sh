#!/bin/sh
# Render kai.toml.template files into kai.toml with absolute paths
# substituted for GREET_BARE / UTIL_BARE placeholders.
#
# Run after tests/fixtures/git-fixtures/setup.sh.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
GIT_FIX="$ROOT/tests/fixtures/git-fixtures"

if [ ! -d "$GIT_FIX/greet.bare" ] || [ ! -d "$GIT_FIX/util.bare" ]; then
  echo "render-fixtures.sh: missing bare repos — run tests/fixtures/git-fixtures/setup.sh first" >&2
  exit 2
fi

render() {
  fixture="$1"
  template="$DIR/$fixture/kai.toml.template"
  out="$DIR/$fixture/kai.toml"
  [ -f "$template" ] || return 0
  sed -e "s#GREET_BARE#$GIT_FIX/greet.bare#g" \
      -e "s#UTIL_BARE#$GIT_FIX/util.bare#g" \
      "$template" > "$out"
  echo "  rendered $fixture/kai.toml"
}

render simple_dep
render transitive
render lockfile_reproducibility
render auto_install

echo "render-fixtures.sh: OK"
