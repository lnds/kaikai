#!/bin/sh
# Modules of one package may import each other; a cycle anywhere else is an
# error. The positive packages run on the C backend and the default one; the
# negative ones must fail naming the rule they break; an edit to one member of
# a cycle is seen by its peers through the cache.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -R "$ROOT/tests/intra_pkg_cycles" "$WORK/pkgs"
export XDG_CACHE_HOME="$WORK/cache"
fail=0

for dir in "$WORK"/pkgs/pos/*; do
  name="$(basename "$dir")"
  for backend in --backend=c ""; do
    got="$(cd "$dir" && "$KAI" run $backend main.kai 2> "$WORK/err")" || {
      echo "intra-pkg-cycles FAIL ($name $backend): did not run"; cat "$WORK/err"; fail=1; continue; }
    if [ "$got" != "$(cat "$dir/expect.out")" ]; then
      echo "intra-pkg-cycles FAIL ($name $backend): want '$(cat "$dir/expect.out")', got '$got'"; fail=1
    fi
  done
done

for dir in "$WORK"/pkgs/neg/*; do
  name="$(basename "$dir")"
  entry="$dir"; [ -d "$dir/consumer" ] && entry="$dir/consumer"
  if (cd "$entry" && "$KAI" run --backend=c main.kai > /dev/null 2> "$WORK/err"); then
    echo "intra-pkg-cycles FAIL ($name): compiled"; fail=1; continue
  fi
  while IFS= read -r line; do
    grep -qF -- "$line" "$WORK/err" || { echo "intra-pkg-cycles FAIL ($name): no '$line' in:"; cat "$WORK/err"; fail=1; }
  done < "$dir/expect.err"
done

# A signature edit in one member reaches its peer's typed result.
pkg="$WORK/pkgs/pos/mutual_fns"
(cd "$pkg" && "$KAI" run --backend=c main.kai > /dev/null 2>&1)
printf 'import odd\n\npub fn is_even(n: Int, extra: Int) : Bool = if n == 0 { true } else { is_odd(n - 1) }\n' > "$pkg/even.kai"
if (cd "$pkg" && "$KAI" run --backend=c main.kai > /dev/null 2> "$WORK/err"); then
  echo "intra-pkg-cycles FAIL (cache): a stale peer compiled"; fail=1
elif ! grep -q "odd.kai" "$WORK/err"; then
  echo "intra-pkg-cycles FAIL (cache): the peer's error is missing"; cat "$WORK/err"; fail=1
fi

# The same cycle entered from another root gives the same answer.
pkg="$WORK/pkgs/pos/members_and_leaves"
printf 'import c\nimport b\n\nfn main() : Unit / Console {\n  print("#{fc(2)} #{fb(2)}")\n}\n' > "$pkg/other.kai"
want="$(cd "$pkg" && "$KAI" run --backend=c other.kai 2>&1)"
[ "$want" = "1002 1012" ] || { echo "intra-pkg-cycles FAIL (second root): got '$want'"; fail=1; }

# A spawn-crossing helper is held to its rule from either root of the cycle.
pkg="$WORK/pkgs/neg/spawn_cycle"
if (cd "$pkg" && "$KAI" run --backend=c other.kai > /dev/null 2> "$WORK/err"); then
  echo "intra-pkg-cycles FAIL (spawn_cycle, second root): compiled"; fail=1
elif ! grep -qF -- "$(cat "$pkg/expect.err")" "$WORK/err"; then
  echo "intra-pkg-cycles FAIL (spawn_cycle, second root): wrong error:"; cat "$WORK/err"; fail=1
fi

[ "$fail" = 0 ] && echo "intra-pkg-cycles OK"
exit "$fail"
