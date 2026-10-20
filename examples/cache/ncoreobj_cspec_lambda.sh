#!/bin/sh
# A closure specialisation of a core fn belongs to the program that asked for
# it, lambdas included: the cached native core object is shared by every
# program, so one built for a first program must still link a second one that
# specialises the same core fn with a different lambda.

set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

fail() { echo "ncoreobj_cspec_lambda: FAIL — $1"; exit 1; }

mkdir -p "$WORK/home" "$WORK/w1" "$WORK/w2"
cat > "$WORK/w1/one.kai" <<'KAI'
fn main() : Unit / Stdout = {
  let a = foldl([1, 2], Ok(0), (acc, x) => Ok(x))
  println("#{a}")
}
KAI
cat > "$WORK/w2/two.kai" <<'KAI'
fn main() : Unit / Stdout = {
  println("start")
  let b = foldl([1, 2, 3], Ok(10), (acc, y) => (acc |> and_then(n => Ok(n + y))))
  println("#{b}")
}
KAI

# A fresh per-user cache: the second program reuses what the first one left.
export HOME="$WORK/home"
export XDG_CACHE_HOME="$WORK/home/cache"

got="$(cd "$WORK/w1" && "$KAI" run --backend=native one.kai 2> "$WORK/err1")" || {
  grep -q -E "native backend (unavailable|is not built)" "$WORK/err1" && { echo "ncoreobj_cspec_lambda: SKIP — native backend unavailable"; exit 0; }
  cat "$WORK/err1"; fail "the first program does not run"; }
[ "$got" = "Ok(2)" ] || fail "the first program printed '$got'"

got="$(cd "$WORK/w2" && "$KAI" run --backend=native two.kai 2> "$WORK/err2")" || { cat "$WORK/err2"; fail "the second program does not link against the first one's cache"; }
want="$(printf 'start\nOk(16)')"
[ "$got" = "$want" ] || fail "the second program printed '$got'"

echo "ncoreobj_cspec_lambda: OK — a second program specialising a core fn differently links against the shared cache"
