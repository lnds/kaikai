#!/bin/sh
# Generate bare git repos for examples/packages/ fixtures.
#
# Idempotent: re-running the script regenerates the bare repos
# from scratch, ensuring SHA stability across runs of the same
# script revision.
#
# Layout:
#   tests/fixtures/git-fixtures/
#     greet.bare/      contains pub fn greet (greet.kai), tag v0.1.0
#     util.bare/       depends on greet @ v0.1.0; tag v0.1.0
#
# Both bare repos sit under this directory so fixtures can
# reference them by absolute path. Path is computed from $0 so the
# script works from any cwd.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"

# Build a temporary checkout, commit, tag, then bare-clone it into
# `<name>.bare` next to this script. Cleans up the temp dir on
# success.
build_repo() {
  name="$1"
  bare="$DIR/$name.bare"
  src="$(mktemp -d)"
  rm -rf "$bare"
  cp -R "$DIR/$name/." "$src/"
  # Substitute placeholder paths so the transitive fixture's
  # nested manifest references the local greet.bare absolutely.
  if [ -f "$src/kai.toml" ]; then
    sed -i.bak "s#GREET_PATH_PLACEHOLDER#$DIR/greet.bare#g" "$src/kai.toml"
    rm -f "$src/kai.toml.bak"
  fi
  (
    cd "$src"
    git init -q
    git config user.email kaikai-fixtures@example.invalid
    git config user.name "kaikai fixtures"
    git add -A
    git commit -q -m "init $name"
    git tag v0.1.0
  )
  git clone -q --bare "$src" "$bare" >/dev/null
  rm -rf "$src"
}

build_repo greet
build_repo util

echo "tests/fixtures/git-fixtures/: regenerated greet.bare + util.bare"
