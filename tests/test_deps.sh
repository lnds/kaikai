#!/bin/sh
# `kai test --json --deps` names the files each test file's build reads, and
# `--list` names the blocks a run would report, without building or running.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -R "$ROOT/tests/test_deps" "$WORK/pkg"
fail=0

kai() { (cd "$WORK/pkg" && XDG_CACHE_HOME="$WORK/cache" "$KAI" "$@"); }
check() {
  if [ "$2" != "$3" ]; then
    echo "test-deps FAIL ($1)"; echo "  want: $2"; echo "  got:  $3"; fail=1
  fi
}
sources() { sed -n "s|^{\"type\":\"deps\",\"file\":\"$2\",\"sources\":\(\[[^]]*\]\).*|\1|p" "$1"; }
external() { sed -n "s|^{\"type\":\"deps\",\"file\":\"$2\",.*\"external\":\(\[.*\]\)}\$|\1|p" "$1"; }
ids() { sed -n 's/^{"type":"test","id":"\(.*\)","file":.*/\1/p' | sort; }

for backend in --backend=c ""; do
  kai test $backend --json --deps . > "$WORK/run.out" 2> "$WORK/run.err"
  kai test $backend --json --list --deps . > "$WORK/list.out" 2> "$WORK/list.err"

  check "a_test sources $backend" '["depsfix/alpha.kai","depsfix/beta.kai","kai.toml","tests/a_test.kai"]' "$(sources "$WORK/list.out" tests/a_test.kai)"
  check "b_test sources $backend" '["depsfix/gamma.kai","kai.toml","tests/b_test.kai"]' "$(sources "$WORK/list.out" tests/b_test.kai)"
  check "c_test sources $backend" '["kai.toml","tests/c_test.kai"]' "$(sources "$WORK/list.out" tests/c_test.kai)"
  check "main sources $backend" '["kai.toml","main.kai"]' "$(sources "$WORK/list.out" main.kai)"
  check "a_test external $backend" '[]' "$(external "$WORK/list.out" tests/a_test.kai)"
  case "$(external "$WORK/list.out" tests/d_test.kai)" in
    *encoding/json.kai*) ;;
    *) echo "test-deps FAIL (d_test external names the stdlib module $backend)"; fail=1 ;;
  esac

  grep '"type":"deps"' "$WORK/run.out" > "$WORK/run.deps"
  grep '"type":"deps"' "$WORK/list.out" > "$WORK/list.deps"
  check "deps of a run equal deps of a list $backend" "$(cat "$WORK/list.deps")" "$(cat "$WORK/run.deps")"

  check "list ids equal run ids $backend" "$(ids < "$WORK/run.out")" "$(ids < "$WORK/list.out")"
  check "list names a quoted id $backend" 1 "$(grep -c 'tests/c_test.kai:bare \\"quoted\\"' "$WORK/list.out")"

  if grep -q 'SIDE EFFECT\|"type":"summary"\|"status"' "$WORK/list.out" "$WORK/list.err"; then
    echo "test-deps FAIL (--list ran a block $backend)"; fail=1
  fi
  only="$(kai test $backend --json --list --only 'tests/b_test.kai:gamma is three' . 2> /dev/null | ids)"
  check "list --only $backend" 'tests/b_test.kai:gamma is three' "$only"
  if kai test $backend --json --list --only 'tests/b_test.kai:nothing' . > /dev/null 2>&1; then
    echo "test-deps FAIL (--list --only matching nothing succeeded $backend)"; fail=1
  fi
done

# A change to one module reaches only the test files that read it.
printf 'import depsfix.alpha\n\npub fn three() : Int = alpha.one() + 2\n' > "$WORK/pkg/depsfix/gamma.kai"
kai test --backend=c --json --list --deps . > "$WORK/after.out" 2> /dev/null
check "b_test now reads alpha" '["depsfix/alpha.kai","depsfix/gamma.kai","kai.toml","tests/b_test.kai"]' "$(sources "$WORK/after.out" tests/b_test.kai)"
check "a_test unchanged" "$(sources "$WORK/list.out" tests/a_test.kai)" "$(sources "$WORK/after.out" tests/a_test.kai)"
check "c_test unchanged" "$(sources "$WORK/list.out" tests/c_test.kai)" "$(sources "$WORK/after.out" tests/c_test.kai)"

# The flags need --json; a plain run adds no deps record.
if kai test --list . > /dev/null 2>&1; then echo "test-deps FAIL (--list without --json)"; fail=1; fi
kai test --backend=c --json . 2> /dev/null > "$WORK/plain.out" || true
check "plain run has no deps record" 0 "$(grep -c '"type":"deps"' "$WORK/plain.out" || true)"

[ "$fail" = 0 ] && echo "test-deps OK"
exit "$fail"
