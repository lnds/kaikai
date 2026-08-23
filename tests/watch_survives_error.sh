#!/bin/sh
# `kai watch` must survive a compile error.
#
# The contract the watcher advertises is that a build failure prints
# Build FAILED and the session keeps waiting for the next save. The
# native build paths report a compile error by `exit`ing rather than
# returning, so a watcher that calls the build directly dies with it —
# the first broken save ends the session instead of reporting it. The
# build therefore runs in a subshell.
#
# The watcher idles forever by design, so the run is bounded and the
# assertion is read from its log, never from an exit status.
#
# Needs a kaic2 with libLLVM: on a C-only build the native paths are not
# the ones taken, and the C path returns rather than exiting.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
FX="$ROOT/examples/modules/issue-1881-native-diag-path"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

# The fixture always fails to compile, so the probe's exit status says
# nothing; what distinguishes a C-only kaic2 is the sentinel it prints
# instead of a diagnostic.
"$KAI" build --backend=native "$FX/solo.kai" -o "$work/probe" 2>"$work/probe.err" || true
if grep -q "not built into this compiler\|native backend is not built" "$work/probe.err" 2>/dev/null; then
  echo "watch-survives-error SKIP (native backend not built into this kaic2)"
  exit 0
fi

proj="$work/proj"
mkdir -p "$proj"
cp "$FX/solo.kai" "$proj/watched.kai"

"$KAI" watch "$proj/watched.kai" >"$work/watch.log" 2>&1 </dev/null &
wpid=$!
left=90
while [ "$left" -gt 0 ]; do
  grep -Fq "Build FAILED" "$work/watch.log" 2>/dev/null && break
  kill -0 "$wpid" 2>/dev/null || break
  sleep 1
  left=$((left - 1))
done
# Still alive here means the watcher outlived the failed build, which is
# the property under test.
alive=0
kill -0 "$wpid" 2>/dev/null && alive=1
kill -9 "$wpid" 2>/dev/null || true
wait "$wpid" 2>/dev/null || true

if ! grep -Fq "Build FAILED" "$work/watch.log"; then
  echo "watch-survives-error FAIL (a compile error did not report Build FAILED)"
  cat "$work/watch.log"
  exit 1
fi
if [ "$alive" != "1" ]; then
  echo "watch-survives-error FAIL (the watcher died on the failed build)"
  cat "$work/watch.log"
  exit 1
fi
if ! grep -Fq "$proj/watched.kai:" "$work/watch.log"; then
  echo "watch-survives-error FAIL (the diagnostic does not name the user's source)"
  cat "$work/watch.log"
  exit 1
fi
echo "watch-survives-error OK"
