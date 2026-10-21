#!/bin/sh
# A first run over an empty shared cache builds: the daily trim that run
# starts never removes the object dir the build has just made.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
"$ROOT/tools/kaic2-native-capable.sh" "$ROOT/stage2/kaic2" > /dev/null 2>&1 || {
  echo "native-cold-cache SKIP (kaic2 has no native backend)"
  exit 0
}
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0
for n in 1 2 3; do
  rm -rf "$WORK/pkg" "$WORK/cache"
  cp -R "$ROOT/tests/test_only_root" "$WORK/pkg"
  if ! (cd "$WORK/pkg" && XDG_CACHE_HOME="$WORK/cache" KAI_BACKEND=native "$KAI" test > "$WORK/out" 2>&1); then
    echo "native-cold-cache FAIL (run $n)"; cat "$WORK/out"; fail=1
  fi
done
[ "$fail" = 0 ] && echo "native-cold-cache OK"
exit "$fail"
