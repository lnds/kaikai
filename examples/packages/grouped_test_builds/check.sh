#!/bin/sh
# examples/packages/grouped_test_builds — the test files of one directory
# build as one binary that runs once per file. Each file still checks alone
# first, so one that does not build on its own fails as it does alone; a
# file's run is the blocks its own build would run, in that build's order; a
# failure that only the shared build hits fails the run before any test runs.
# Also: a block's unused binding is reported in the file that declares it,
# a module that does not parse fails the build even when nothing uses it,
# a block reaches its own file's functions whatever else the build holds,
# a type named like a core effect is one type across the build, and a test
# file's `main` is typed as that file's entry point and called as a function.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "grouped_test_builds: FAIL — $1" >&2; exit 1; }

mk_pkg() {
  rm -rf "$TMP/$1"
  mkdir -p "$TMP/$1/tests" "$TMP/$1/lib"
  printf 'name = "%s"\nedition = "hanga-roa"\n' "$1" > "$TMP/$1/kai.toml"
  printf 'fn main() {\n  print("m")\n}\n' > "$TMP/$1/main.kai"
}

# Run `kai test .` in package $1 into $TMP/out and $TMP/err; the status in $TMP/rc.
run() {
  rc=0
  (cd "$TMP/$1" && "$KAI" test . > "$TMP/out" 2> "$TMP/err") || rc=$?
  echo "$rc" > "$TMP/rc"
}

# 1 — a file's run is its own build's blocks in its own build's order, though
# another file loads the same modules in another order.
mk_pkg order
printf 'pub fn x() : Int = 1\n\ntest "x block" {\n  assert x() == 1\n}\n' > "$TMP/order/lib/x.kai"
printf 'pub fn y() : Int = 2\n\ntest "y block" {\n  assert y() == 2\n}\n' > "$TMP/order/lib/y.kai"
printf 'import lib.x\n\ntest "a" {\n  assert x.x() == 1\n}\n' > "$TMP/order/tests/a_test.kai"
printf 'import lib.y\nimport lib.x\n\ntest "b" {\n  assert y.y() + x.x() == 3\n}\n' > "$TMP/order/tests/b_test.kai"
run order
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "the ordered suite did not pass"; }
got="$(sed -n '/b_test.kai$/,$p' "$TMP/err" | grep -E '^  ok' | tr -s ' ' | tr '\n' '|')"
[ "$got" = " ok y block| ok x block| ok b|" ] || fail "b_test ran '$got'; want y block, x block, b"
got="$(sed -n '/a_test.kai$/,/b_test.kai$/p' "$TMP/err" | grep -E '^  ok' | tr -s ' ' | tr '\n' '|')"
[ "$got" = " ok x block| ok a|" ] || fail "a_test ran '$got'; want x block, a"

# 2 — a file that does not build alone fails as it does alone: the files before
# it ran, the ones after did not, though the shared build would compile it.
mk_pkg leak
printf 'pub fn base() : Int = 10\n' > "$TMP/leak/lib/core.kai"
printf 'import lib.core\n\ntest "a" {\n  assert core.base() == 10\n}\n' > "$TMP/leak/tests/a_test.kai"
printf 'test "b" {\n  assert core.base() == 10\n}\n' > "$TMP/leak/tests/b_test.kai"
printf 'test "c" {\n  assert 1 == 1\n}\n' > "$TMP/leak/tests/c_test.kai"
run leak
[ "$(cat "$TMP/rc")" -ne 0 ] || fail "a file that does not build alone passed"
grep -q "cannot find \`core\`" "$TMP/err" || fail "the file's own error was not reported"
grep -q "ok   a" "$TMP/err" || fail "the file before it did not run"
if grep -q "ok   c" "$TMP/err"; then fail "the file after it ran"; fi

# 3 — files that each build alone but not together fail the run, before any
# test runs: each binds one C symbol under its own signature.
mk_pkg dup
mkdir -p "$TMP/dup/c"
printf '#include <stdint.h>\nint32_t grouped_probe(void) { return 7; }\n' > "$TMP/dup/c/probe.c"
printf '\n[native]\nsources = ["c/probe.c"]\n' >> "$TMP/dup/kai.toml"
printf 'extern "C"("grouped_probe") fn probe() : I32 / Ffi\n\ntest "a" {\n  assert 1 == 1\n}\n' > "$TMP/dup/tests/a_test.kai"
printf 'extern "C"("grouped_probe") fn probe() : Int / Ffi\n\ntest "b" {\n  assert 1 == 1\n}\n' > "$TMP/dup/tests/b_test.kai"
run dup
[ "$(cat "$TMP/rc")" -ne 0 ] || fail "test files that do not build together passed"
grep -q "conflicting declarations of extern function" "$TMP/err" || fail "the shared build's error was not reported"
if grep -q "ok   a" "$TMP/err"; then fail "a test ran although the shared build failed"; fi

# 4 — an unused binding in an imported module's block names that module's file.
mk_pkg warn
printf 'pub fn z() : Int = 1\n\ntest "z" {\n  let idle = 1\n  assert z() == 1\n}\n' > "$TMP/warn/lib/z.kai"
printf 'import lib.z\n\ntest "a" {\n  assert z.z() == 1\n}\n' > "$TMP/warn/tests/a_test.kai"
(cd "$TMP/warn" && "$KAI" test tests/a_test.kai > /dev/null 2> "$TMP/err") || fail "the warning suite failed"
grep -A1 "unused binding \`idle\`" "$TMP/err" | grep -q "lib/z.kai:4:" || fail "the block's unused binding did not name lib/z.kai"

# 5 — a module that does not parse fails the build though nothing uses it.
mk_pkg broken
printf 'pub fn later(x: Int) : Int = todo!()\n' > "$TMP/broken/lib/bad.kai"
printf 'import lib.bad\n\nfn main() {\n  print("ran")\n}\n' > "$TMP/broken/main.kai"
rc=0; (cd "$TMP/broken" && "$KAI" run main.kai > "$TMP/out" 2>&1) || rc=$?
[ "$rc" -ne 0 ] || fail "a module that does not parse built"
if grep -q "^ran$" "$TMP/out"; then fail "the program ran despite the parse error"; fi

# 6 — a file's own function is the one its blocks call, though a protocol op
# has the same name.
mk_pkg shadow
printf 'fn one() : Int = 41\n\ntest "own one" {\n  assert one() + 1 == 42\n}\n' > "$TMP/shadow/tests/a_test.kai"
printf 'test "b" {\n  assert 1 + 1 == 2\n}\n' > "$TMP/shadow/tests/b_test.kai"
run shadow
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "a block called the protocol op instead of its file's function"; }

# 7 — a file that opens with a block reaches its own function, not the one of
# the same name in the file loaded before it.
mk_pkg first
printf 'fn helper() : Int = 1\n\ntest "a" {\n  assert helper() == 1\n}\n' > "$TMP/first/tests/a_test.kai"
printf 'test "b" {\n  assert helper() == 2\n}\n\nfn helper() : Int = 2\n' > "$TMP/first/tests/b_test.kai"
run first
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "a block reached the function of the file loaded before its own"; }

# 8 — a block reads a type name as its own file does, and a record's field
# keeps the type its module wrote, though a core effect has the same name.
mk_pkg homonym
printf 'pub type State = Cold | Ready\n\npub type Session = { state: State }\n\npub fn state_name(s: State) : String = match s {\n  Cold -> "cold"\n  Ready -> "ready"\n}\n' > "$TMP/homonym/lib/session.kai"
printf 'import lib.session\n\ntest "field" {\n  assert session.state_name(session.Session { state: session.Ready }.state) == "ready"\n}\n\ntest "annotation" {\n  let s : State = session.Cold\n  assert session.state_name(s) == "cold"\n}\n' > "$TMP/homonym/tests/a_test.kai"
printf 'test "b" {\n  assert 1 + 1 == 2\n}\n' > "$TMP/homonym/tests/b_test.kai"
run homonym
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "a package type named like a core effect was two types"; }
grep -q "ok   field" "$TMP/err" || fail "the record field test did not run"
grep -q "ok   annotation" "$TMP/err" || fail "the annotation test did not run"

# 9 — the shared build's entry point is its runner: each file's own `main`
# keeps the row its file gives it when built alone, and its blocks call it.
mk_pkg entry
printf 'fn main() {\n  print("a main ran")\n}\n\ntest "a calls its main" {\n  main()\n  assert 1 + 1 == 2\n}\n' > "$TMP/entry/tests/a_test.kai"
printf 'pub fn main() : Int = 7\n\ntest "b calls its main" {\n  assert main() == 7\n}\n' > "$TMP/entry/tests/b_test.kai"
printf 'fn main() : Unit / Stdout {\n  Stdout.print("c main ran")\n}\n\ntest "c calls its main" {\n  main()\n  assert 3 + 3 == 6\n}\n' > "$TMP/entry/tests/c_test.kai"
run entry
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "test files that call their own main did not pass together"; }
for t in a b c; do
  grep -q "ok   $t calls its main" "$TMP/err" || fail "the block of ${t}_test that calls its main did not pass"
done
got="$(grep 'main ran$' "$TMP/out" | tr '\n' '|')"
[ "$got" = "a main ran|c main ran|" ] || fail "the files' mains printed '$got'; want each once, in file order"

# 10 — a test file that imports another builds alone with that file as a
# module, where a `main` that leaves its row out is an ordinary function:
# the importer fails its own check in the shared run as it does alone.
mk_pkg cross
printf 'import b_test\n\ntest "a calls b main" {\n  b_test.main()\n  assert 1 + 1 == 2\n}\n' > "$TMP/cross/tests/a_test.kai"
printf 'pub fn main() {\n  print("b main ran")\n}\n\ntest "b" {\n  assert 2 + 2 == 4\n}\n' > "$TMP/cross/tests/b_test.kai"
imported_main="\`main\` is an entry point only in the root module; this file is imported here"
(cd "$TMP/cross" && "$KAI" test tests/b_test.kai > /dev/null 2> "$TMP/err") || { cat "$TMP/err"; fail "a test file with its own main failed alone"; }
rc=0; (cd "$TMP/cross" && "$KAI" test tests/a_test.kai > /dev/null 2> "$TMP/err") || rc=$?
[ "$rc" -ne 0 ] || fail "an imported main with no row passed in the importer's own build"
grep -qF "$imported_main" "$TMP/err" || fail "the importer's own build did not say the main is imported"
run cross
[ "$(cat "$TMP/rc")" -ne 0 ] || fail "an imported main with no row passed in the shared run"
grep -q "effect not handled: Stdout" "$TMP/err" || fail "the shared run did not report the importer's own error"
grep -qF "$imported_main" "$TMP/err" || fail "the shared run did not say the main is imported"
if grep -q "ok   b" "$TMP/err"; then fail "the file after the failing importer ran"; fi
# With its row declared the same `main` is callable from both builds.
printf 'pub fn main() : Unit / Stdout {\n  Stdout.print("b main ran")\n}\n\ntest "b" {\n  assert 2 + 2 == 4\n}\n' > "$TMP/cross/tests/b_test.kai"
(cd "$TMP/cross" && "$KAI" test tests/a_test.kai > "$TMP/out" 2> "$TMP/err") || { cat "$TMP/err"; fail "an imported main with a row failed in the importer's own build"; }
alone="$(grep -c '^b main ran$' "$TMP/out")"
run cross
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "an imported main with a row failed in the shared run"; }
grep -q "ok   a calls b main" "$TMP/err" || fail "the importer's block did not run in the shared run"
[ "$(grep -c '^b main ran$' "$TMP/out")" -eq "$alone" ] || fail "the imported main ran a different number of times in the shared run"

# 11 — a file typed as a root is typed again where it is a module: a later
# importer is rejected though the shared build already accepted that `main`.
mk_pkg warm
printf 'pub fn main() {\n  print("b main ran")\n}\n\ntest "b" {\n  assert 2 + 2 == 4\n}\n' > "$TMP/warm/tests/b_test.kai"
printf 'test "c" {\n  assert 3 + 3 == 6\n}\n' > "$TMP/warm/tests/c_test.kai"
run warm
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "the suite did not pass before the importer existed"; }
printf 'import b_test\n\ntest "a calls b main" {\n  b_test.main()\n  assert 1 + 1 == 2\n}\n' > "$TMP/warm/tests/a_test.kai"
run warm
[ "$(cat "$TMP/rc")" -ne 0 ] || fail "an imported main typed earlier as a root was accepted as a module"
grep -qF "$imported_main" "$TMP/err" || fail "the importer added later did not say the main is imported"

# 12 — an importer's clean check does not outlive the edit that makes the
# imported `main` perform with no row.
mk_pkg stale
printf 'import b_test\n\ntest "a calls b main" {\n  assert b_test.main() == 0\n}\n' > "$TMP/stale/tests/a_test.kai"
printf 'pub fn main() : Int {\n  0\n}\n\ntest "b" {\n  assert 2 + 2 == 4\n}\n' > "$TMP/stale/tests/b_test.kai"
run stale
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "an imported main that performs nothing did not pass"; }
printf 'pub fn main() : Int {\n  print("b main ran")\n  0\n}\n\ntest "b" {\n  assert 2 + 2 == 4\n}\n' > "$TMP/stale/tests/b_test.kai"
run stale
[ "$(cat "$TMP/rc")" -ne 0 ] || fail "the importer's earlier check outlived the edit to the imported main"
grep -qF "$imported_main" "$TMP/err" || fail "the importer checked again did not say the main is imported"

# 13 — two test files that each declare an effect and a type of one name keep
# them apart in the shared build: each handler answers its own file's effect.
mk_pkg twins
cat > "$TMP/twins/tests/a_test.kai" <<'EOF'
effect Log {
  say(s: String) : Int
}

type Item = { n: Int }

fn use_log() : Int / Log = Log.say("alpha")

test "a handles its own Log" {
  let it = Item { n: 1 }
  let r = handle { use_log() } with Log { say(s, resume) -> resume(string_length(s) + it.n) }
  assert r == 6
}
EOF
cat > "$TMP/twins/tests/b_test.kai" <<'EOF'
effect Log {
  say(s: String, n: Int) : String
}

type Item = { name: String }

fn use_log() : String / Log = Log.say("beta", 2)

test "b handles its own Log" {
  let it = Item { name: "!" }
  let r = handle { use_log() } with Log { say(s, n, resume) -> resume("#{s}#{n}#{it.name}") }
  assert r == "beta2!"
}
EOF
run twins
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "test files declaring one effect name did not pass together"; }
for t in a b; do
  grep -q "ok   $t handles its own Log" "$TMP/err" || fail "the block of ${t}_test that handles its own Log did not pass"
done

echo "grouped_test_builds: OK"
