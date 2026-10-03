#!/bin/sh
# A broken module the program imports fails the build on every build path.
#
# The negative runner drives kaic2 directly. `kai` adds what it does not:
# the per-package user cache and the partitioned builds (native-modular,
# C modular), each of which loads modules on its own path. Every
# `examples/negative/modules/broken_import_*` fixture is built through
# `kai` on each path, cold and then warm, and must reject with an
# ordinary exit code, its expected diagnostic and none of its forbidden
# lines. Each fixture is copied into a package so the user cache is on.
#
# Without a native backend in kaic2 the native paths report SKIP.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

fail=0

native=1
printf 'fn main() : Unit / Stdout = Stdout.print("")\n' > "$work/probe.kai"
if ! "$KAI" build --backend=native "$work/probe.kai" -o "$work/probe" 2>"$work/probe.err"; then
  if grep -q "not built into this compiler\|native backend is not built" "$work/probe.err"; then
    native=0
  else
    echo "broken-import-paths FAIL: the native probe did not build"; cat "$work/probe.err"; exit 1
  fi
fi

# check <path-label> <fixture-dir> <env...>
check() {
  label="$1"; fx="$2"; shift 2
  name=$(basename "$fx")
  proj="$work/$label-$name"
  rm -rf "$proj"
  cp -R "$fx" "$proj"
  # A package, so `kai` turns its user cache on.
  printf 'name = "%s"\nentry = "main.kai"\n' "$name" > "$proj/kai.toml"
  flags=""
  [ -f "$proj/main.flags" ] && flags=$(cat "$proj/main.flags")
  for pass in cold warm; do
    err="$proj/$pass.err"
    rc=0
    case "$flags" in
      *--test*) (cd "$proj" && env "$@" "$KAI" test main.kai) >/dev/null 2>"$err" || rc=$? ;;
      *)        (cd "$proj" && env "$@" "$KAI" build . -o "$proj/out") >/dev/null 2>"$err" || rc=$? ;;
    esac
    if [ "$rc" -eq 0 ] || [ "$rc" -gt 127 ]; then
      echo "  FAIL $label $name ($pass): exit $rc"; head -3 "$err"; fail=$((fail + 1)); return
    fi
    if ! grep -qF -- "$(head -1 "$proj/main.err.expected")" "$err"; then
      echo "  FAIL $label $name ($pass): expected diagnostic missing"; head -3 "$err"; fail=$((fail + 1)); return
    fi
    if [ -f "$proj/main.forbidden" ]; then
      while IFS= read -r line; do
        [ -z "$line" ] && continue
        if grep -qF -- "$line" "$err"; then
          echo "  FAIL $label $name ($pass): forbidden \`$line\` present"; fail=$((fail + 1)); return
        fi
      done < "$proj/main.forbidden"
    fi
  done
  echo "  ok   $label $name"
}

for fx in "$ROOT"/examples/negative/modules/broken_import_*; do
  check c-modular "$fx" KAI_BACKEND=c KAI_MODULAR=1
  if [ "$native" = 1 ]; then
    check native-modular "$fx" KAI_BACKEND=native KAI_NATIVE_MODULAR=1
    check native-whole "$fx" KAI_BACKEND=native KAI_NATIVE_MODULAR=0
  fi
done
[ "$native" = 1 ] || echo "  SKIP native paths (native backend not built into this kaic2)"

if [ "$fail" -ne 0 ]; then
  echo "broken-import-paths FAIL ($fail)"
  exit 1
fi
echo "broken-import-paths OK"
