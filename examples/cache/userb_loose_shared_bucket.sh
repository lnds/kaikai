#!/bin/sh
# A loose file's cache lives in the shared cache, one bucket per absolute
# source directory: two `main.kai` of the same name in different
# directories get two buckets and each prints its own output, and nothing
# is written beside the source.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

fail() { echo "userb_loose_shared_bucket FAIL — $1"; exit 1; }

mkdir -p "$WORK/a" "$WORK/b" "$WORK/xdg"
for d in a b; do
  if [ "$d" = a ]; then v=1; else v=2; fi
  cat > "$WORK/$d/dep.kai" <<EOF
pub fn value() : Int = $v
EOF
  cat > "$WORK/$d/main.kai" <<'EOF'
import dep
fn main() : Unit / Console { print(int_to_string(dep.value())) }
EOF
done

run() {
  XDG_CACHE_HOME="$WORK/xdg" KAI_CACHE_TRIM=0 "$KAI" run "$WORK/$1/main.kai" 2>/dev/null || true
}

for round in cold warm; do
  [ "$(run a)" = "1" ] || fail "$round: a/main.kai did not print 1"
  [ "$(run b)" = "2" ] || fail "$round: b/main.kai did not print 2"
done

buckets="$(ls -d "$WORK"/xdg/kai/core/*/loose/* 2>/dev/null | wc -l | tr -d ' ')"
[ "$buckets" = "2" ] || fail "expected 2 buckets under the shared cache, found $buckets"

for d in a b; do
  [ ! -e "$WORK/$d/.kai-cache" ] || fail "$d: a .kai-cache was created beside the source"
done

echo "userb_loose_shared_bucket OK — same-named loose files get separate shared buckets and correct output"
exit 0
