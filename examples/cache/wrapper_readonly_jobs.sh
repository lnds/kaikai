#!/bin/sh
# A cache whose slot directory cannot be written must still build, not wait for a slot.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
CACHE="$PROJ/cache"
LIMIT="${LIMIT:-120}"

unlock() { chflags -R nouchg "$PROJ" 2>/dev/null || true; chmod -R u+w "$PROJ" 2>/dev/null || true; }
cleanup() { unlock; rm -rf "$PROJ" 2>/dev/null || sudo -n rm -rf "$PROJ"; }
trap cleanup EXIT INT TERM

printf 'fn main() = println("hello")\n' > "$PROJ/hello.kai"

# Fails when the build outlives LIMIT seconds.
build() {
  XDG_CACHE_HOME="$CACHE" "$KAI" build "$PROJ/hello.kai" -o "$PROJ/hello" 2>"$PROJ/build.err" 1>/dev/null &
  pid=$!
  n=0
  while kill -0 "$pid" 2>/dev/null; do
    n=$((n + 1))
    if [ "$n" -gt "$LIMIT" ]; then
      kill "$pid" 2>/dev/null || true
      echo "wrapper_readonly_jobs FAIL — build hung on a read-only cache"; exit 1
    fi
    sleep 1
  done
  wait "$pid" || { echo "wrapper_readonly_jobs FAIL — build failed"; cat "$PROJ/build.err"; exit 1; }
  [ "$("$PROJ/hello")" = "hello" ] || { echo "wrapper_readonly_jobs FAIL — wrong output"; exit 1; }
}

# Immutable flag or root ownership: refuses the owner too.
lock_hard() {
  if command -v chflags >/dev/null 2>&1; then
    chflags -R uchg "$@"
  elif sudo -n true 2>/dev/null; then
    sudo chown -R root "$@"
    sudo chmod -R a-w "$@"
  else
    return 1
  fi
}

# Empty cache with only an unwritable jobs directory.
mkdir -p "$CACHE/kai/jobs"
lock_hard "$CACHE/kai/jobs" || chmod a-w "$CACHE/kai/jobs"
build

# Cache filled first, then locked whole.
unlock
rm -rf "$CACHE" 2>/dev/null || sudo -n rm -rf "$CACHE"
mkdir -p "$CACHE"
build
if ! lock_hard "$CACHE"; then
  if [ -n "${CI:-}" ]; then
    echo "wrapper_readonly_jobs FAIL — CI has no way to make the cache read-only"; exit 1
  fi
  echo "wrapper_readonly_jobs SKIP — filled read-only cache: no immutable flag or passwordless sudo"
  exit 0
fi
rm -f "$PROJ/hello"
build

echo "wrapper_readonly_jobs OK — builds on a read-only cache"
exit 0
