#!/usr/bin/env bash
# Build each examples/web fixture for wasm32-browser and compare what it prints with
# its .out.expected: under node, then in each headless browser on PATH's drivers.
#
#   tools/test-web.sh [fixture.kai ...]
#   BROWSERS="chrome firefox"   browsers to drive (default: those whose driver is on PATH)
#   KAI=bin/kai                 the wrapper under test
#
# The cache starts empty: the first build fetches the pinned toolchain, which is the path a
# user takes. KAI_WASI_SDK names a toolchain instead.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="${KAI:-$ROOT/bin/kai}"
TMP="$(mktemp -d)"
trap 'kill $SERVER 2>/dev/null; rm -rf "$TMP"' EXIT
SERVER=""
fail=0

say() { printf '%s\n' "$*"; }
bad() { say "FAIL: $*"; fail=1; }

if [ -z "${KAI_WASI_SDK:-}" ]; then
  export XDG_CACHE_HOME="$TMP/cache"
fi

if [ $# -gt 0 ]; then
  FIXTURES=("$@")
else
  FIXTURES=("$ROOT"/examples/web/*.kai "$ROOT"/examples/web_js/*.kai "$ROOT"/examples/web_js/snake/snake.kai "$ROOT"/examples/web_js_pkg/main.kai "$ROOT"/examples/web_js_deps/shouted.kai)
fi

if [ -z "${BROWSERS+x}" ]; then
  BROWSERS=""
  command -v chromedriver >/dev/null && BROWSERS="chrome"
  command -v geckodriver >/dev/null && BROWSERS="$BROWSERS firefox"
fi

compare() { # name leg actual-file expected-file
  if diff <(sed -e 's/[[:space:]]*$//' "$3") <(sed -e 's/[[:space:]]*$//' "$4") >"$TMP/diff" 2>&1; then
    say "ok:   $1 [$2]"
  else
    bad "$1 [$2]"
    cat "$TMP/diff"
  fi
}

# node before 24.15 needs a flag for exception handling with exnref, and one for JSPI; the stub stands in for the page's document.
run_module() {
  local flags
  for flags in "" "--experimental-wasm-exnref" "--experimental-wasm-exnref --experimental-wasm-jspi"; do
    if node --import "$ROOT/tools/web-stub.mjs" $flags "$1" >"$TMP/attempt.out" 2>"$TMP/node.err"; then
      cat "$TMP/attempt.out"
      return 0
    fi
  done
  return 1
}

build_all() {
  mkdir -p "$TMP/out"
  for f in "${FIXTURES[@]}"; do
    cp "$(dirname "$f")"/*.json "$TMP/out/" 2>/dev/null
  done
  for f in "${FIXTURES[@]}"; do
    n="$(basename "$f" .kai)"
    if ! "$KAI" build --target=wasm32-browser "$f" -o "$TMP/out/$n" 2>"$TMP/build-$n.err"; then
      bad "build $n"
      cat "$TMP/build-$n.err"
    elif grep -qE '^(error|warning):' "$TMP/build-$n.err"; then
      bad "build $n printed a diagnostic"
      cat "$TMP/build-$n.err"
    fi
  done
}

run_node() {
  for f in "${FIXTURES[@]}"; do
    n="$(basename "$f" .kai)"
    [ -f "$TMP/out/$n.mjs" ] || continue
    [ -f "${f%.kai}.browser-only" ] && continue
    run_module "$TMP/out/$n.mjs" >"$TMP/$n.node"
    want="${f%.kai}.node.expected"
    [ -f "$want" ] || want="${f%.kai}.out.expected"
    compare "$n" node "$TMP/$n.node" "$want"
  done
}

# A fixture with a .ticks.expected sleeps while a host timer ticks every 5 ms: the timeline
# must show the ticks between the program's lines, under node and in each browser. Ticks
# before the first line and after the last are page load and settle time, and dropped.
collapse_ticks() {
  awk '
    function flush() { if (n >= 3) print "ticks"; else if (n > 0) print "few ticks"; n = 0 }
    $0 == "tick" { n++; next }
    { if (seen) flush(); n = 0; seen = 1; print }
  '
}

run_ticks_node() {
  for f in "${FIXTURES[@]}"; do
    n="$(basename "$f" .kai)"
    [ -f "${f%.kai}.ticks.expected" ] && [ -f "$TMP/out/$n.wasm" ] || continue
    node --experimental-wasm-exnref --experimental-wasm-jspi "$ROOT/tools/web-ticks.mjs" "$TMP/out/$n.wasm" 2>/dev/null \
      | collapse_ticks >"$TMP/$n.ticks.node"
    compare "$n ticks" node "$TMP/$n.ticks.node" "${f%.kai}.ticks.expected"
  done
}

# The page gets a ticker that writes into the element the program prints to.
ticker_page() { # name
  sed 's|<script type="module"|<script>setInterval(() => { document.getElementById("kai-stdout").textContent += "tick\\n"; }, 5);</script>\
&|' "$TMP/out/$1.html" >"$TMP/out/$1.ticks.html"
}

serve() {
  python3 -m http.server 8765 --bind 127.0.0.1 --directory "$TMP/out" >/dev/null 2>&1 &
  SERVER=$!
  sleep 1
}

run_browsers() {
  serve
  for b in $BROWSERS; do
    for f in "${FIXTURES[@]}"; do
      n="$(basename "$f" .kai)"
      [ -f "$TMP/out/$n.html" ] || continue
      query=""; [ -f "${f%.kai}.query" ] && query="?$(cat "${f%.kai}.query")"
      steps=(); [ -f "${f%.kai}.steps.js" ] && steps=("${f%.kai}.steps.js")
      python3 -I "$ROOT/tools/web-driver.py" "$b" "http://127.0.0.1:8765/$n.html$query" "${steps[@]}" >"$TMP/$n.$b" 2>"$TMP/$n.$b.err" \
        || { cat "$TMP/$n.$b.err"; bad "$n [$b] driver"; continue; }
      if [ "$b" != chrome ] && grep -q "Promise Integration" "$TMP/$n.$b"; then
        say "skip: $n [$b] (this browser has no JSPI)"
        continue
      fi
      compare "$n" "$b" "$TMP/$n.$b" "${f%.kai}.out.expected"
      [ -f "${f%.kai}.ticks.expected" ] || continue
      ticker_page "$n"
      python3 -I "$ROOT/tools/web-driver.py" "$b" "http://127.0.0.1:8765/$n.ticks.html" 2>"$TMP/$n.$b.err" \
        | collapse_ticks >"$TMP/$n.ticks.$b"
      compare "$n ticks" "$b" "$TMP/$n.ticks.$b" "${f%.kai}.ticks.expected"
    done
  done
}

# A checksum that does not match the pin must stop the build, never fall back.
check_wrong_checksum() {
  local cache="$TMP/cache-bad" bogus="$TMP/bogus.tar.gz"
  printf 'not a toolchain\n' >"$bogus"
  if env -u KAI_WASI_SDK XDG_CACHE_HOME="$cache" KAI_WASI_SDK_URL="file://$bogus" "$KAI" build --target=wasm32-browser \
      "${FIXTURES[0]}" -o "$TMP/bad/app" >"$TMP/bad.out" 2>&1; then
    bad "a wrong checksum built a module"
  elif grep -q "does not match its pinned checksum" "$TMP/bad.out" && ! find "$cache" -name clang 2>/dev/null | grep -q .; then
    say "ok:   wrong checksum stops the build"
  else
    bad "wrong checksum gave the wrong diagnostic"
    cat "$TMP/bad.out"
  fi
}

# A program whose `main` performs a refused effect fails the build and leaves no module.
check_refusal_fails() {
  local d="$TMP/refused"
  mkdir -p "$d"
  cp "$ROOT/examples/negative/wasm/wasm_main_stdin.kai" "$d/app.kai"
  if (cd "$d" && "$KAI" build --target=wasm32-browser app.kai -o out/app >"$d/build.out" 2>&1); then
    bad "a refused program built"
  elif grep -q "performs effect \`Stdin\`" "$d/build.out" && [ ! -e "$d/out/app.wasm" ]; then
    say "ok:   refused main fails the build and writes no module"
  else
    bad "refused main: wrong diagnostic or a module was written"
    cat "$d/build.out"
  fi
}

# The installed layout finds the host under share/kaikai/web.
check_installed_layout() {
  local p="$TMP/prefix" n
  n="$(basename "${FIXTURES[0]}" .kai)"
  mkdir -p "$p/bin" "$p/libexec/kaikai" "$p/share/kaikai/web" "$p/share/kaikai/include"
  cp "$KAI" "$p/bin/kai"
  cp "$ROOT/stage2/kaic2" "$p/libexec/kaikai/kaic2"
  ln -s "$ROOT/stdlib" "$p/share/kaikai/stdlib"
  cp -R "$ROOT/stage2/runtime" "$ROOT/stage2/runtime.h" "$ROOT/stage2/runtime_owner_c.c" "$ROOT/stage0/runtime_llvm.c" "$p/share/kaikai/include/"
  cp "$ROOT/VERSION" "$ROOT/EDITION" "$p/share/kaikai/"
  cp "$ROOT/stage2/web/kai_host.mjs" "$p/share/kaikai/web/"
  if "$p/bin/kai" build --target=wasm32-browser "${FIXTURES[0]}" -o "$TMP/inst/$n" 2>"$TMP/inst.err" \
      && [ -s "$TMP/inst/kai_host.mjs" ]; then
    say "ok:   installed layout"
  else
    bad "installed layout"
    cat "$TMP/inst.err"
  fi
}

# A nonzero exit of main reaches the console and the node exit status.
check_exit_status() {
  local src="$ROOT/examples/web_exit/exit_status.kai"
  if ! "$KAI" build --target=wasm32-browser "$src" -o "$TMP/exit/exit_status" 2>"$TMP/exit.err"; then
    bad "exit status build"
    cat "$TMP/exit.err"
    return
  fi
  node --import "$ROOT/tools/web-stub.mjs" --experimental-wasm-exnref --experimental-wasm-jspi "$TMP/exit/exit_status.mjs" >/dev/null 2>"$TMP/exit.err"
  local code=$?
  if [ "$code" = 3 ] && grep -q "exited with status 3" "$TMP/exit.err"; then
    say "ok:   nonzero exit is reported"
  else
    bad "nonzero exit is reported (status $code)"
    cat "$TMP/exit.err"
  fi
}

# An engine without JSPI runs a program that never suspends, and refuses one that does.
check_no_jspi() {
  local pure="$TMP/out/web_arithmetic" spawning="$TMP/out/web_spawn"
  [ -f "$pure.mjs" ] && [ -f "$spawning.mjs" ] || return 0
  if node --import "$ROOT/tools/web-nojspi.mjs" --import "$ROOT/tools/web-stub.mjs" --experimental-wasm-exnref "$pure.mjs" >"$TMP/nojspi.out" 2>"$TMP/nojspi.err" \
      && diff -q "$TMP/nojspi.out" "$ROOT/examples/web/web_arithmetic.out.expected" >/dev/null; then
    say "ok:   pure program runs without JSPI"
  else
    bad "pure program without JSPI"
    cat "$TMP/nojspi.err"
  fi
  if node --import "$ROOT/tools/web-nojspi.mjs" --import "$ROOT/tools/web-stub.mjs" --experimental-wasm-exnref "$spawning.mjs" >/dev/null 2>"$TMP/nojspi.err"; then
    bad "a spawning program ran without JSPI"
  elif grep -q "Promise Integration" "$TMP/nojspi.err"; then
    say "ok:   spawning program is refused without JSPI"
  else
    bad "spawning program without JSPI: no diagnostic"
    cat "$TMP/nojspi.err"
  fi
}

build_all
[ -f "$TMP/build-$(basename "${FIXTURES[0]}" .kai).err" ] && grep -q "fetching wasi-sdk" "$TMP/build-$(basename "${FIXTURES[0]}" .kai).err" \
  && say "ok:   toolchain fetched from an empty cache"
run_node
run_ticks_node
run_browsers
check_wrong_checksum
check_refusal_fails
check_installed_layout
check_exit_status
check_no_jspi
exit $fail
