#!/bin/sh
# The optimisation level of the C compile follows KAI_C_OPT when CFLAGS is
# unset: -O2 by default, the value when valid, -O2 again for a bad one, and
# -O0 under `kai test`. An explicit CFLAGS wins over all of them. The fake
# compiler logs every line and forwards to the real `cc`.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

real_cc="$(command -v cc)"
cat > "$work/fakecc" <<EOF
#!/bin/sh
echo "\$*" >> "$work/cc.log"
exec "$real_cc" "\$@"
EOF
chmod +x "$work/fakecc"
printf 'fn main() {\n  println("opt")\n}\n' > "$work/hello.kai"
printf 'test "opt" {\n  assert(1 + 1 == 2)\n}\n' > "$work/hello_test.kai"

# The runtime owner object compiles at -O2 whatever the level.
levels() {
  grep -e ' -c ' "$work/cc.log" | grep -v -e RUNTIME_OWNER | grep -o -e ' -O[0-3s]' | sort -u | tr -d ' \n'
}

fail=0
# $1 label, $2 expected level, $3.. the environment of the build.
case_for() {
  label="$1"; want="$2"; shift 2
  : > "$work/cc.log"
  (cd "$work" && env "$@" CC="$work/fakecc" KAI_MODULAR=1 KAI_BIN_MEMO=0 XDG_CACHE_HOME="$work/cache-$label" \
    "$KAI" build --backend=c hello.kai -o "$work/out-$label" >/dev/null) \
    || { echo "c-opt FAIL ($label): build failed"; fail=1; return; }
  got="$(levels)"
  [ "$got" = "$want" ] || { echo "c-opt FAIL ($label): compiled at '$got', wanted '$want'"; fail=1; }
}

case_for default -O2 KAI_C_OPT=
case_for zero -O0 KAI_C_OPT=0
case_for bad -O2 KAI_C_OPT=fast
case_for cflags -O1 KAI_C_OPT=0 "CFLAGS=-std=c99 -O1"

# `kai test` defaults the level to -O0 for the program it builds.
: > "$work/cc.log"
(cd "$work" && env CC="$work/fakecc" KAI_BIN_MEMO=0 KAI_BACKEND=c XDG_CACHE_HOME="$work/cache-test" \
  "$KAI" test hello_test.kai >/dev/null) || { echo "c-opt FAIL (test): kai test failed"; fail=1; }
[ "$(levels)" = -O0 ] || { echo "c-opt FAIL (test): kai test compiled at '$(levels)'"; fail=1; }

[ "$fail" = 0 ] && echo "c-opt OK"
exit "$fail"
