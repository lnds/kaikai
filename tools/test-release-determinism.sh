#!/bin/sh
# tools/test-release-determinism.sh <bin/kai> — two cold `--release` builds of one program
# leave identical ThinLTO entries and cached objects.
set -eu

KAI="${1:?usage: test-release-determinism.sh <bin/kai>}"
work="$(mktemp -d "${TMPDIR:-/tmp}/release-determinism.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

fail() { echo "test-release-determinism: FAIL: $1" >&2; exit 1; }

printf 'fn main() : Unit / Stdout = Stdout.print("hi")\n' > "$work/main.kai"

backends="c"
if "$KAI" build --backend=native "$work/main.kai" -o "$work/probe" > /dev/null 2>&1; then
  backends="c native"
fi

for b in $backends; do
  for run in 1 2; do
    cache="$work/cache-$b-$run"
    mkdir -p "$cache"
    XDG_CACHE_HOME="$cache" KAI_LTO=1 "$KAI" build --backend="$b" --release "$work/main.kai" -o "$work/out-$b" \
      || fail "$b run $run: build failed"
    [ "$("$work/out-$b")" = "hi" ] || fail "$b run $run: binary does not run"
  done
  for part in lto cmodular nmodular; do
    [ -d "$work/cache-$b-1/kai/$part" ] || continue
    diff -r "$work/cache-$b-1/kai/$part" "$work/cache-$b-2/kai/$part" > "$work/diff" \
      || fail "$b: cold builds differ in $part: $(head -5 "$work/diff")"
  done
  if [ "$b" = c ]; then
    [ -n "$(ls "$work/cache-$b-1/kai/lto" 2> /dev/null)" ] || fail "c: no ThinLTO entries to compare"
  fi
done
echo "test-release-determinism: ok — $backends"
