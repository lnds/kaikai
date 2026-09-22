#!/bin/sh
# Gate for the kai binary (tools/kai): `kai env`, the plugin contract, and
# the dispatch of an unknown verb to `kai-<verb>` on PATH — in a dev
# checkout and in an installed prefix.

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

printf 'test-kai-cli: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
