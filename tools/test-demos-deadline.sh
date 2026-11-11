#!/bin/sh
# The demo harness bounds each demo: one that hangs is reported by name as a
# timeout, and the demos after it still run. The compiler and cc are stubbed,
# so this needs no built kaic2.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

cat > "$tmp/kaic2" <<'STUB'
#!/bin/sh
echo "/* stub */"
STUB

# Stands in for cc: the "binary" of the demo named hang never returns.
cat > "$tmp/cc" <<'STUB'
#!/bin/sh
while [ "$#" -gt 0 ]; do
  [ "$1" = "-o" ] && out="$2"
  shift
done
case "$out" in
  */hang-bin) printf '#!/bin/sh\nexec sleep 20\n' > "$out" ;;
  *)          printf '#!/bin/sh\necho ran\n' > "$out" ;;
esac
chmod +x "$out"
STUB
chmod +x "$tmp/kaic2" "$tmp/cc"

make -C "$ROOT/demos" --no-print-directory verify \
  DEMOS="hang quick" BUILD="$tmp/build" RUN_BUDGET=1 \
  KAIC2="$tmp/kaic2" CC="$tmp/cc" > "$tmp/table" 2>&1 || true

fail=0
expect() {
  if grep -q "$2" "$tmp/build/$1.status" 2>/dev/null; then
    echo "OK   $1: $(cat "$tmp/build/$1.status")"
  else
    echo "FAIL $1: want a status matching '$2', got '$(cat "$tmp/build/$1.status" 2>/dev/null || echo "<none>")'"
    fail=1
  fi
}
expect hang  '^FAIL runtime: TIMEOUT after 1s'
expect quick '^PASS (no golden)$'

if [ "$fail" = 0 ]; then
  echo "test-demos-deadline: OK"
else
  cat "$tmp/table"
  echo "test-demos-deadline: FAIL"
  exit 1
fi
