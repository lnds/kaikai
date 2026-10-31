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
  [ "$n" = wasi_fibers ] && continue
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

if "$KAI" build --target=wasm32-wasi examples/wasi/wasi_refused_signal.kai -o "$TMP/refused" 2>"$TMP/refused.err"; then
  bad "a main with Signal builds"
elif grep -q "$(cat examples/wasi/wasi_refused_signal.err.expected)" "$TMP/refused.err"; then
  ok "Signal in main is refused"
else
  bad "the refusal does not name the target"; cat "$TMP/refused.err"
fi

if command -v wasm-merge >/dev/null; then
  "$KAI" build --target=wasm32-wasi examples/wasi/wasi_fibers.kai -o "$TMP/fibers" 2>"$TMP/fibers.err" \
    && ok "a program with fibers builds" || { bad "a program with fibers builds"; cat "$TMP/fibers.err"; }
  if wasmtime run -W stack-switching=y /dev/null 2>&1 | grep -q "not supported"; then
    if "$KAI" run --target=wasm32-wasi examples/wasi/wasi_fibers.kai 2>&1 | grep -q "stack-switching proposal"; then
      ok "an engine without stack switching is named"
    else
      bad "an engine without stack switching is named"
    fi
  elif [ "$(wasmtime run -W stack-switching=y --dir=. "$TMP/fibers.wasm" 2>&1)" = "$(cat examples/wasi/wasi_fibers.out.expected)" ]; then
    ok "fibers run under the engine"
  else
    bad "fibers run under the engine"
  fi
  if grep -aq needs_suspension "$TMP/built/app.wasm"; then
    bad "a program that never switches imports the context switch"
  else
    ok "a program that never switches imports no context switch"
  fi
  if PATH="/usr/bin:/bin" "$KAI" build --target=wasm32-wasi examples/wasi/wasi_fibers.kai -o "$TMP/nm" 2>"$TMP/nm.err"; then
    bad "a fiber program builds without wasm-merge"
  elif grep -q "wasm-merge" "$TMP/nm.err"; then
    ok "a missing wasm-merge is named"
  else
    bad "the missing wasm-merge diagnostic"; cat "$TMP/nm.err"
  fi
else
  say "skip: fibers (wasm-merge not on PATH)"
fi

if PATH="/usr/bin:/bin" "$KAI" run --target=wasm32-wasi examples/wasi/run/exit_status.kai 2>"$TMP/nort.err"; then
  bad "run without wasmtime succeeds"
elif grep -q "needs \`wasmtime\` on PATH" "$TMP/nort.err"; then
  ok "a missing runtime is named"
else
  bad "the missing-runtime diagnostic"; cat "$TMP/nort.err"
fi

if "$KAI" build --backend=native --target=wasm32-wasi examples/wasi/run/exit_status.kai -o "$TMP/native" 2>"$TMP/native.err"; then
  "$KAI" run --backend=native --target=wasm32-wasi examples/wasi/run/exit_status.kai >"$TMP/native.out" 2>&1
  if [ "$?" = 7 ] && [ "$(cat "$TMP/native.out")" = "exiting" ]; then ok "native backend builds and runs"; else bad "native backend run"; cat "$TMP/native.out"; fi
  # The core-object cache is shared by every target: switching targets with a warm cache must not reuse another's object.
  if "$KAI" build --backend=native examples/wasi/run/exit_status.kai -o "$TMP/host" 2>/dev/null \
    && "$KAI" build --backend=native --target=wasm32-wasi examples/wasi/run/exit_status.kai -o "$TMP/again" 2>/dev/null \
    && [ "$(wasmtime run "$TMP/again.wasm" 2>&1)" = "exiting" ]; then
    ok "a warm core cache keeps host and wasi objects apart"
  else
    bad "a warm core cache keeps host and wasi objects apart"
  fi
  if CFLAGS="-O2 -DKAI_HOST_FIBERS=1" "$KAI" build --backend=native --target=wasm32-wasi examples/wasi/run/exit_status.kai -o "$TMP/guard" 2>"$TMP/guard.err"; then
    bad "native build with other KAI_* macros than the bitcode's"
  elif grep -q "bitcode" "$TMP/guard.err"; then
    ok "bitcode macro mismatch is refused"
  else
    ok "bitcode absent: no macro guard to exercise"
  fi
elif grep -q "WebAssembly LLVM target\|native backend is not built" "$TMP/native.err"; then
  say "skip: native wasm (no WebAssembly LLVM target in this kaic2)"
else
  bad "native backend build for wasm32-wasi"; cat "$TMP/native.err"
fi

exit "$fail"
