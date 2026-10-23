#!/usr/bin/env bash
# `kai build` and `kai run` for wasm32-wasi: the examples/wasi fixtures under wasmtime, the
# program's arguments, environment, stdin and exit status, the single-module output, and the
# diagnostics for a missing runtime and a refused `main`.
#
#   tools/test-wasi.sh
#   KAI=bin/kai   the wrapper under test
#
# KAI_WASI_SDK names a toolchain; without it the first build fetches the pinned one.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="${KAI:-$ROOT/bin/kai}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

say() { printf '%s\n' "$*"; }
bad() { say "FAIL: $*"; fail=1; }
ok() { say "ok:   $*"; }

command -v wasmtime >/dev/null || { say "test-wasi: SKIP (wasmtime not on PATH)"; exit 0; }

cd "$ROOT" || exit 2

for f in examples/wasi/*.kai; do
  n="$(basename "$f" .kai)"
  [ -f "examples/wasi/$n.out.expected" ] || continue
  if KAI_WASI_DIRS=.:/tmp "$KAI" run --target=wasm32-wasi "$f" >"$TMP/$n.out" 2>"$TMP/$n.err" \
    && diff -q "examples/wasi/$n.out.expected" "$TMP/$n.out" >/dev/null; then
    ok "$n"
  else
    bad "$n"
    diff "examples/wasi/$n.out.expected" "$TMP/$n.out"
    cat "$TMP/$n.err"
  fi
done

out="$(printf 'first line\nsecond\n' | KAI_WASI_PROBE=seen "$KAI" run --target=wasm32-wasi examples/wasi/run/args_env_stdin.kai one "two words" 2>"$TMP/args.err")"
want=$'args: one,two words\nprobe: seen\nstdin: first line'
if [ "$out" = "$want" ]; then ok "args, env and stdin"; else bad "args, env and stdin"; say "$out"; cat "$TMP/args.err"; fi

"$KAI" run --target=wasm32-wasi examples/wasi/run/exit_status.kai >"$TMP/exit.out" 2>&1
rc=$?
if [ "$rc" = 7 ] && [ "$(cat "$TMP/exit.out")" = "exiting" ]; then ok "exit status"; else bad "exit status (rc=$rc)"; cat "$TMP/exit.out"; fi

mkdir "$TMP/scoped"
printf 'x' >"$TMP/scoped/probe"
cat >"$TMP/reach.kai" <<EOF
fn main() : Int / Stdout + File = {
  Stdout.print("scoped: #{File.read_file("$TMP/scoped/probe") |> is_ok()}")
  Stdout.print("outside: #{File.read_file("/etc/hosts") |> is_ok()}")
  0
}
EOF
out="$(KAI_WASI_DIRS="$TMP/scoped" "$KAI" run --target=wasm32-wasi "$TMP/reach.kai" 2>&1)"
if [ "$out" = $'scoped: true\noutside: false' ]; then ok "preopens follow KAI_WASI_DIRS"; else bad "preopens follow KAI_WASI_DIRS"; say "$out"; fi

mkdir "$TMP/built"
if "$KAI" build --target=wasm32-wasi examples/wasi/run/exit_status.kai -o "$TMP/built/app" 2>"$TMP/build.err" \
  && [ "$(ls "$TMP/built")" = "app.wasm" ]; then
  ok "build writes one module and nothing beside it"
else
  bad "build writes one module and nothing beside it"; ls "$TMP/built"; cat "$TMP/build.err"
fi

if "$KAI" build --target=wasm32-wasi examples/wasi/wasi_refused_spawn.kai -o "$TMP/refused" 2>"$TMP/refused.err"; then
  bad "a main with Spawn builds"
elif grep -q "fibers are not available on wasm32-wasi" "$TMP/refused.err"; then
  ok "Spawn in main is refused"
else
  bad "the refusal does not say fibers are unavailable"; cat "$TMP/refused.err"
fi

if PATH="/usr/bin:/bin" "$KAI" run --target=wasm32-wasi examples/wasi/run/exit_status.kai 2>"$TMP/nort.err"; then
  bad "run without wasmtime succeeds"
elif grep -q "needs \`wasmtime\` on PATH" "$TMP/nort.err"; then
  ok "a missing runtime is named"
else
  bad "the missing-runtime diagnostic"; cat "$TMP/nort.err"
fi

exit "$fail"
