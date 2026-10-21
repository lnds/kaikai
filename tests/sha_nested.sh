#!/bin/sh
# A digest taken in the middle of another stream leaves both exactly as
# they would be alone, and a stream fed after its digest was taken aborts.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

for f in "$ROOT"/tools/kai/*.kai; do
  [ "$(basename "$f")" = main.kai ] || ln -s "$f" "$work/$(basename "$f")"
done
cp "$ROOT/tools/kai/cli_sys.c" "$work/shim.c"

cat > "$work/nested.kai" <<'EOF'
import cli_hash
import cli_sys

fn main() : Unit / Stdout + Ffi {
  let h = cli_sys.sha_new()
  cli_sys.sha_str(h, "outer-1")
  let inner = cli_hash.digest([cli_hash.Text("inner")])
  cli_sys.sha_str(h, "outer-2")
  Stdout.print(cli_sys.sha_hex(h))
  Stdout.print(inner)
}
EOF

cat > "$work/reuse.kai" <<'EOF'
import cli_sys

fn main() : Unit / Ffi {
  let h = cli_sys.sha_new()
  cli_sys.sha_str(h, "x")
  let _ = cli_sys.sha_hex(h)
  cli_sys.sha_str(h, "y")
}
EOF

build() {
  CFLAGS="$work/shim.c" KAI_BACKEND=c "$ROOT/bin/kai" build "$work/$1.kai" -o "$work/$1" > "$work/$1.log" 2>&1 \
    || { cat "$work/$1.log"; exit 1; }
}
build nested
build reuse

sha() { printf '%s' "$1" | shasum -a 256 | cut -d' ' -f1; }
fail=0

got="$("$work/nested")"
want="$(sha outer-1outer-2)
$(sha inner)"
if [ "$got" != "$want" ]; then
  echo "  FAIL nested: got '$got', expected '$want'"
  fail=$((fail + 1))
fi

rc=0
{ "$work/reuse" || rc=$?; } > "$work/reuse.out" 2>&1
if [ "$rc" = 0 ] || ! grep -q "fed after its digest" "$work/reuse.out"; then
  echo "  FAIL reuse: status $rc, output '$(cat "$work/reuse.out")'"
  fail=$((fail + 1))
fi

[ "$fail" = 0 ] && echo "sha_nested: ok"
exit "$fail"
