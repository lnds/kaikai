#!/bin/sh
# examples/packages/fetch_failure — `kai fetch` with a dependency it
# cannot fetch exits non-zero and leaves kai.lock untouched; a full
# commit SHA as `ref` fetches that commit.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

mkdir -p "$TMP/up" "$TMP/proj"
( cd "$TMP/up" && git init -q \
  && printf 'pub fn hi() : Int = 7\n' > lib.kai \
  && printf '[package]\nname = "up"\nversion = "0.1.0"\n' > kai.toml \
  && git add . && git -c user.email=t@t -c user.name=t commit -qm one )
SHA="$(git -C "$TMP/up" rev-parse HEAD)"

manifest() {
  printf '[package]\nname = "p"\nversion = "0.1.0"\n\n[dependencies]\nup = { source = "file://%s/up", ref = "%s" }\n' \
    "$TMP" "$1" > "$TMP/proj/kai.toml"
}

fetch() {
  set +e
  ( cd "$TMP/proj" && XDG_CACHE_HOME="$TMP/xdg" "$ROOT/bin/kai" fetch \
      >"$TMP/stdout" 2>"$TMP/stderr" )
  status=$?
  set -e
}

fail() {
  echo "fetch_failure: FAIL — $1" >&2
  cat "$TMP/stdout" "$TMP/stderr" >&2
  exit 1
}

manifest "$SHA"
fetch
[ "$status" -eq 0 ] || fail "a full SHA ref did not fetch (exit $status)"
grep -q "$SHA" "$TMP/proj/kai.lock" || fail "kai.lock does not pin $SHA"
cp "$TMP/proj/kai.lock" "$TMP/lock.before"

for ref in 0000000000000000000000000000000000000000 no-such-branch; do
  manifest "$ref"
  fetch
  [ "$status" -ne 0 ] || fail "ref '$ref' could not be fetched, yet exit was 0"
  grep -q "could not be fetched" "$TMP/stderr" || fail "ref '$ref': no diagnostic naming the dependency"
  cmp -s "$TMP/proj/kai.lock" "$TMP/lock.before" || fail "ref '$ref': kai.lock was rewritten"
done

echo "fetch_failure: OK (SHA ref fetched; unfetchable refs exit non-zero, lock untouched)"
