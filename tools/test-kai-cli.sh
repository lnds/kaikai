#!/bin/sh
# Gate for the kai binary (tools/kai): `kai env`, the plugin contract, the
# dispatch of an unknown verb to `kai-<verb>` on PATH (`kai upgrade` among
# them), build/run and the dev-loop verbs — in a dev checkout and in an
# installed prefix.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
PASS=0
FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() { printf '  FAIL  %s\n' "$1"; FAIL=$((FAIL + 1)); }
ok()   { printf '  ok    %s\n' "$1"; PASS=$((PASS + 1)); }

# expect <label> <want-status> <want-output> <command...>: stdout+stderr
# must equal <want-output> exactly.
expect() {
  label="$1"; want_status="$2"; want="$3"; shift 3
  status=0
  got="$("$@" 2>&1)" || status=$?
  if [ "$status" -ne "$want_status" ]; then
    fail "$label (exit $status, want $want_status)"
    printf '%s\n' "$got" | sed 's/^/        /'
  elif [ "$got" != "$want" ]; then
    fail "$label (output differs)"
    printf '        want: %s\n        got:  %s\n' "$want" "$got"
  else
    ok "$label"
  fi
}

# expect_line <label> <want-status> <want-first-line> <command...>
expect_line() {
  label="$1"; want_status="$2"; want="$3"; shift 3
  status=0
  got="$("$@" 2>&1)" || status=$?
  first="$(printf '%s\n' "$got" | sed -n 1p)"
  if [ "$status" -ne "$want_status" ] || [ "$first" != "$want" ]; then
    fail "$label (exit $status, first line '$first')"
  else
    ok "$label"
  fi
}

case "$(uname -s)" in
  Darwin) tid="$(stat -f '%m-%z' "$ROOT/stage2/kaic2")" ;;
  *)      tid="$(stat -c '%Y-%s' "$ROOT/stage2/kaic2")" ;;
esac

# The dev checkout's resolution, with no overrides in play.
unset KAI_STDLIB KAIKAI_HOME
expect "env: dev checkout" 0 "KAIKAI_HOME=$ROOT
KAI_STDLIB=$ROOT/stdlib
KAI_TOOLCHAIN_ID=$tid" "$KAI" env
expect "env <name>: one value per line" 0 "$tid
$ROOT" "$KAI" env KAI_TOOLCHAIN_ID KAIKAI_HOME
expect "env: unknown name" 2 \
  "kai: error: unknown variable 'NOPE' (known: KAIKAI_HOME, KAI_STDLIB, KAI_TOOLCHAIN_ID)" \
  "$KAI" env KAI_STDLIB NOPE
expect "env: KAI_STDLIB overrides" 0 "/elsewhere" env KAI_STDLIB=/elsewhere "$KAI" env KAI_STDLIB
expect "env: KAIKAI_HOME names another prefix" 0 "$TMP/home" \
  env KAIKAI_HOME="$TMP/home" "$KAI" env KAIKAI_HOME

# Plugins: argv verbatim, the `kai env` variables exported, the exit
# status passed through.
mkdir -p "$TMP/plugins" "$TMP/noexec"
cat > "$TMP/plugins/kai-hello" <<'EOF'
#!/bin/sh
for a in "$@"; do printf '[%s]' "$a"; done
printf '\n%s|%s|%s\n' "$KAIKAI_HOME" "$KAI_STDLIB" "$KAI_TOOLCHAIN_ID"
exit 7
EOF
chmod +x "$TMP/plugins/kai-hello"
printf '#!/bin/sh\necho hijacked\n' > "$TMP/plugins/kai-fmt"
chmod +x "$TMP/plugins/kai-fmt"
printf '#!/bin/sh\necho ran\n' > "$TMP/noexec/kai-quiet"

expect "plugin: argv, env, exit status" 7 "[a b][--flag][]
$ROOT|$ROOT/stdlib|$tid" env PATH="$TMP/plugins:$PATH" "$KAI" hello "a b" --flag ""
expect_line "plugin: never shadows a core verb" 0 "usage: kai fmt [--width N] <file.kai>            # rewrite file in place" \
  env PATH="$TMP/plugins:$PATH" "$KAI" fmt --help
expect_line "plugin: a non-executable file is skipped" 2 "kai: error: unknown command: quiet" \
  env PATH="$TMP/noexec:$PATH" "$KAI" quiet
expect_line "unknown command" 2 "kai: error: unknown command: frobnicate" "$KAI" frobnicate
expect_line "a flag is never a plugin" 2 "kai: error: unknown command: --hello" \
  env PATH="$TMP/plugins:$PATH" "$KAI" --hello
expect_line "help" 0 " _      _ _      _" "$KAI" help
expect_line "no command: usage, exit 2" 2 " _      _ _      _" "$KAI"

# build/run through the binary.
mkdir -p "$TMP/prog"
cat > "$TMP/prog/args.kai" <<'KAI'
import os.args
fn main() : Int / Stdout + Env = {
  args.argv() |> foreach(a => Stdout.print("[#{a}]"))
  7
}
KAI
printf 'fn main() : Unit / Stdout = Stdout.print("hi")\n' > "$TMP/prog/hello.kai"
expect "run (c): args verbatim, exit status passed through" 7 "[a b]
[--x]" "$KAI" run --backend=c "$TMP/prog/args.kai" "a b" --x
# The default backend: native, or C with a note on a kaic2 without libLLVM.
status=0
got="$("$KAI" run "$TMP/prog/args.kai" "a b" --x 2>/dev/null)" || status=$?
if [ "$status" -eq 7 ] && [ "$got" = "[a b]
[--x]" ]; then ok "run (default backend): args, exit status"; else fail "run (default backend): exit $status, got '$got'"; fi
# Parallel compiles into a cold cache: every status must reach kai even though
# the runtime reaps children whenever a fiber parks on file I/O.
stats="$(cd "$TMP/prog" && KAI_MODULAR=1 KAI_MODULAR_STATS=1 KAI_MODULAR_JOBS=8 \
  KAI_MODULAR_CACHE_DIR="$TMP/mc" "$KAI" build --backend=c hello.kai -o hello 2>&1 && ./hello)" || true
case "$stats" in
  *"cache hits=0 compiled="*"hi") ok "build: parallel c-modular compiles into a cold cache" ;;
  *) fail "build: parallel c-modular compiles into a cold cache"; printf '%s\n' "$stats" | sed 's/^/        /' ;;
esac
traces="$(KAI_TRACE_RC=1 "$KAI" run "$TMP/prog/hello.kai" 2>&1 | grep -c 'KAI_TRACE_RC\] alloc_total=' || true)"
if [ "$traces" = "1" ]; then ok "run: the RC trace is the program's alone"; else fail "run: $traces RC trace lines, want 1"; fi

# The dev-loop verbs over a package: entry, tests/ sibling, discovered *_test.kai.
Q="$TMP/ws/pkg"
mkdir -p "$Q/tests" "$TMP/ws/lib/tests"
printf 'name = "tp"\n' > "$Q/kai.toml"
printf 'pub fn one() : Int = 1\n' > "$Q/util.kai"
printf 'import util\nfn main() : Int = util.one() - 1\ntest "entry" {\n  assert util.one() == 1\n}\n' > "$Q/main.kai"
printf 'import util\ntest "extra" {\n  assert util.one() == 1\n}\n' > "$Q/extra_test.kai"
printf 'test "sibling" {\n  assert true\n}\nfn main() : Int = 0\n' > "$Q/tests/a.kai"
printf 'name = "tl"\n' > "$TMP/ws/lib/kai.toml"
printf 'pub fn one() : Int = 1\n' > "$TMP/ws/lib/lib.kai"
printf 'import lib\ntest "lib" {\n  assert lib.one() == 1\n}\n' > "$TMP/ws/lib/lib_test.kai"
printf 'fn main() : Int = 0\n' > "$TMP/ws/lib/tests/t.kai"
got="$(cd "$Q" && "$KAI" test --backend=c 2>&1)" && status=0 || status=$?
case "$status:$got" in
  *"not reachable"*) fail "test: the entry is never reported unreachable"; printf '%s\n' "$got" | sed 's/^/        /' ;;
  0:*"/tests/a.kai"*"== kai test extra_test.kai"*) ok "test: entry, tests/ sibling, discovered *_test.kai" ;;
  *) fail "test: package run (exit $status)"; printf '%s\n' "$got" | sed 's/^/        /' ;;
esac
expect_line "test --json --only: no match is exit 1" 1 "kai: no test matched --only" \
  sh -c 'cd "$1" && { "$2" test --backend=c --json --only nomatch 2>err >/dev/null; rc=$?; tail -1 err; exit $rc; }' _ "$Q" "$KAI"
expect_line "test --only: no match is exit 1" 1 "kai: no test matched --only" \
  sh -c 'cd "$1" && { "$2" test --backend=c --only nomatch >err 2>&1; rc=$?; tail -1 err; exit $rc; }' _ "$Q" "$KAI"
expect_line "test ./...: every package" 0 "kai: all package tests passed (2 package(s))" \
  sh -c 'cd "$1" && { "$2" test --backend=c ./... >"$3" 2>&1; rc=$?; tail -1 "$3"; exit $rc; }' _ "$TMP/ws" "$KAI" "$TMP/rec.log"
expect "typecheck: a library checks every module it owns" 0 "kai: lib.kai
kai: lib_test.kai
kai: tests/t.kai" sh -c 'cd "$1" && "$2" typecheck' _ "$TMP/ws/lib" "$KAI"
expect "bench: --iters must be positive" 2 \
  "kai: error: --iters value must be a positive integer (got: 0)" "$KAI" bench --iters 0 x.kai
expect "check: --backend is validated" 2 \
  "kai: error: --backend must be 'c' or 'native' (got: llvm)" "$KAI" check --backend llvm x.kai

# An installed prefix: the binary resolves the prefix it sits in, never
# the checkout it was built in, and ignores a KAIKAI_HOME naming another.
P="$TMP/prefix"
mkdir -p "$P/bin" "$P/libexec/kaikai" "$P/share/kaikai/stdlib"
cp "$ROOT/bin/kai" "$P/bin/kai"
cp "$ROOT/tools/kai/kai" "$P/libexec/kaikai/kai"
printf '#!/bin/sh\n' > "$P/libexec/kaikai/kaic2"
chmod +x "$P/bin/kai" "$P/libexec/kaikai/kaic2"
case "$(uname -s)" in
  Darwin) ptid="$(stat -f '%m-%z' "$P/libexec/kaikai/kaic2")" ;;
  *)      ptid="$(stat -c '%Y-%s' "$P/libexec/kaikai/kaic2")" ;;
esac
expect "env: installed prefix" 0 "KAIKAI_HOME=$P
KAI_STDLIB=$P/share/kaikai/stdlib
KAI_TOOLCHAIN_ID=$ptid" env KAIKAI_HOME="$TMP/home" "$P/bin/kai" env
ln -s "$P/bin/kai" "$TMP/kai-link"
expect "env: through a symlinked bin/kai" 0 "$P" "$TMP/kai-link" env KAIKAI_HOME
rm "$P/libexec/kaikai/kai"
expect_line "installed prefix without the binary" 2 \
  "kai: error: kai binary missing at $P/libexec/kaikai/kai — installation is corrupt" "$P/bin/kai" env

# `kai upgrade` is a plugin the toolchain ships: found before PATH, it
# upgrades the prefix kai exports.
C="$TMP/Cellar/kaikai/0.0.1"
mkdir -p "$C/bin" "$C/libexec/kaikai/plugins" "$C/share/kaikai/stdlib"
cp "$ROOT/bin/kai" "$C/bin/kai"
cp "$ROOT/tools/kai/kai" "$C/libexec/kaikai/kai"
cp "$ROOT/tools/kai/plugins/kai-upgrade" "$C/libexec/kaikai/plugins/kai-upgrade"
printf '#!/bin/sh\n' > "$C/libexec/kaikai/kaic2"
chmod +x "$C/bin/kai" "$C/libexec/kaikai/kaic2"
expect "upgrade: the plugin acts on the prefix kai exports" 0 "kai is installed via Homebrew ($C/bin/kai).
Run 'brew upgrade kaikai' to update it." "$C/bin/kai" upgrade
printf '#!/bin/sh\necho hijacked\n' > "$TMP/plugins/kai-upgrade"
chmod +x "$TMP/plugins/kai-upgrade"
expect_line "plugin: the toolchain's own comes before PATH" 0 "usage: kai upgrade" \
  env PATH="$TMP/plugins:$PATH" "$KAI" upgrade --help

printf 'test-kai-cli: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
