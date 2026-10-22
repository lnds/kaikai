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
    if ! diff -rq "$work/cache-$b-1/kai/$part" "$work/cache-$b-2/kai/$part" > "$work/diff"; then
      f="$(sed -n 's|^Files \(.*\) and .* differ$|\1|p;s|^Binary files \(.*\) and .* differ$|\1|p' "$work/diff" | head -1)"
      if [ -n "$f" ]; then
        g="$work/cache-$b-2/${f#"$work/cache-$b-1/"}"
        echo "--- $f: $(cmp -l "$f" "$g" | wc -l) differing bytes" >&2
        strings -a "$f" > "$work/s1"; strings -a "$g" > "$work/s2"
        diff "$work/s1" "$work/s2" | head -10 >&2 || true
      fi
      fail "$b: cold builds differ in $part: $(head -5 "$work/diff")"
    fi
  done
done
echo "test-release-determinism: ok — $backends"
