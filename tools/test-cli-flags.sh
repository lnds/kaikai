#!/bin/sh
# CLI flag-hygiene gate for the bin/kai driver.
#
# Property: an argument that starts with '-' and is not a recognised
# flag of the subcommand fails fast with an "unknown flag" diagnostic
# and exit 2. It must never be consumed as a source path, package
# name, module, or topic, and never be silently ignored (the failure
# shape: `kai check --backend=c f.kai` made the flag the source path
# and died later inside basename).
#
# `kai test` is exempt: it forwards unrecognised flags to the compiler
# by design (issue #318), so they never become paths there. `kai run`
# forwards arguments AFTER the spec to the compiled program; flags
# BEFORE it must be recognised. `kai lsp` execs the LSP server, which
# owns its own arguments.
#
# Also pins the backend contract: every verb that takes --backend
# accepts c|native and refuses anything else, and a verb refuses an
# argument it has no use for rather than dropping it.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
PASS=0
FAIL=0

fail() { printf '  FAIL  %s\n' "$1"; FAIL=$((FAIL + 1)); }
ok()   { printf '  ok    %s\n' "$1"; PASS=$((PASS + 1)); }

# expect_reject <label> <expected-substring> <kai args...>
expect_reject() {
  label="$1"; want="$2"; shift 2
  status=0
  out="$("$KAI" "$@" 2>&1)" || status=$?
  if [ "$status" -ne 2 ]; then
    fail "$label (exit $status, want 2)"
    printf '%s\n' "$out" | sed 's/^/        /'
    return
  fi
  case "$out" in
    *"$want"*) ok "$label" ;;
    *)
      fail "$label (missing '$want' in output)"
      printf '%s\n' "$out" | sed 's/^/        /' ;;
  esac
}

BOGUS="--no-such-flag"

expect_reject "build rejects unknown flag"     "unknown flag" build "$BOGUS" x.kai
expect_reject "run rejects unknown flag"       "unknown flag" run "$BOGUS" x.kai
expect_reject "bench rejects unknown flag"     "unknown option" bench "$BOGUS" x.kai
expect_reject "check rejects unknown flag"     "unknown flag" check "$BOGUS" x.kai
expect_reject "typecheck rejects unknown flag" "unknown flag" typecheck "$BOGUS" x.kai
expect_reject "lint rejects unknown flag"      "unknown flag" lint "$BOGUS" x.kai
expect_reject "fmt rejects unknown flag"       "unknown flag" fmt "$BOGUS" x.kai
expect_reject "migrate rejects unknown flag"   "unknown flag" migrate "$BOGUS" x.kai
expect_reject "mutate rejects unknown flag"    "unknown flag" mutate "$BOGUS"
expect_reject "mutate rejects unknown operator" "unknown operator" mutate --operator nosuchop
expect_reject "mutate --apply requires --module" "requires --module" mutate --apply 0
expect_reject "mutate --apply takes a site id" "takes a site id" mutate --apply x --module x.kai
expect_reject "mutate --apply runs nothing" "takes only --module" mutate --apply 0 --module x.kai --list
expect_reject "watch rejects unknown flag"     "unknown flag" watch "$BOGUS" x.kai
expect_reject "init rejects unknown flag"      "unknown flag" init "$BOGUS"
expect_reject "fetch rejects unknown flag"     "unknown flag" fetch "$BOGUS"
expect_reject "install rejects unknown flag"   "unknown flag" install "$BOGUS"
expect_reject "add rejects unknown flag"       "unknown flag" add "$BOGUS"
expect_reject "update rejects unknown flag"    "unknown flag" update "$BOGUS"
expect_reject "show rejects unknown flag"      "unknown flag" show "$BOGUS"
expect_reject "doc rejects unknown flag"       "unknown flag" doc "$BOGUS"
expect_reject "info rejects unknown flag"      "unknown flag" info "$BOGUS"
expect_reject "upgrade rejects unknown flag"   "unknown flag" upgrade "$BOGUS"
expect_reject "upgrade rejects stray args"     "takes no arguments" upgrade nonsense

# kai check backend contract (issue #1750).
expect_reject "check refuses a bogus backend"  "must be 'c' or 'native'" check --backend=metal x.kai
expect_reject "check --backend without value"  "needs an argument" check --backend

# --backend missing its value is a parse error, not an unbound-variable
# crash, on every subcommand that consumes it.
expect_reject "test --backend without value"   "needs an argument" test --backend
expect_reject "bench --backend without value"  "needs an argument" bench --backend
expect_reject "watch --backend without value"  "needs an argument" watch --backend
expect_reject "test --only without value"      "needs an argument" test --only

# A --backend value is validated on every verb that takes one, never
# degraded to the default in silence.
for verb in build run bench test watch; do
  expect_reject "$verb refuses a bogus backend" "must be 'c' or 'native' (got: metal)" "$verb" --backend=metal x.kai
done
expect_reject "test refuses an empty backend"  "must be 'c' or 'native' (got: )" test --backend= x.kai

# Arguments a verb has no use for are an error, never dropped.
expect_reject "fetch refuses stray args"       "'kai fetch' takes no arguments, got 'x'" fetch x
expect_reject "show refuses stray args"        "'kai show' takes no arguments, got 'x'" show x
expect_reject "install --list refuses --force" "--list takes no other arguments" install --list --force
expect_reject "install --list refuses a spec"  "--list takes no other arguments" install --list .
expect_reject "info --list refuses stray args" "--list takes no other arguments, got 'x'" info --list x
expect_reject "info -k takes one keyword"      "-k takes one keyword, got also 'b'" info -k a b

# Reversion proof for the original repro: an explicit --backend=c runs
# the check runner exactly like the flagless form — no basename noise,
# exit 0, the summary line present.
label="check --backend=c runs the C check runner"
status=0
out="$("$KAI" check --backend=c "$ROOT/examples/stdlib/check_basic.kai" 2>&1)" || status=$?
case "$out" in *"usage: basename"*) basename_noise=1 ;; *) basename_noise=0 ;; esac
if [ "$status" -eq 0 ] && [ "$basename_noise" -eq 0 ] \
   && printf '%s' "$out" | grep -q "checks passed"; then
  ok "$label"
else
  fail "$label (exit $status)"
  printf '%s\n' "$out" | sed 's/^/        /'
fi

printf 'test-cli-flags: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
