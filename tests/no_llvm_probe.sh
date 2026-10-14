#!/bin/sh
# An implicit native build asks kaic2 once whether it carries libLLVM,
# before compiling anything: a kaic2 without it goes straight to the C
# backend, with no native compile first; one with it builds native. An
# explicit backend never asks, and a settled kaic2 is asked once. The
# checkout's kaic2 runs behind a wrapper that logs every invocation.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

D="$work/dev"
mkdir -p "$D/bin" "$D/stage0" "$D/stage2"
cp "$ROOT/bin/kai" "$D/bin/kai"
ln -s "$ROOT/stdlib" "$D/stdlib"
ln -s "$ROOT/EDITION" "$D/EDITION"
ln -s "$ROOT/VERSION" "$D/VERSION"
for f in "$ROOT"/stage0/*; do ln -s "$f" "$D/stage0/"; done
for f in "$ROOT"/stage2/*; do [ "$(basename "$f")" = kaic2 ] || ln -s "$f" "$D/stage2/"; done
log="$work/kaic2.log"
cat > "$D/stage2/kaic2" <<EOF
#!/bin/sh
echo "\$*" >> "$log"
exec "$ROOT/stage2/kaic2" "\$@"
EOF
chmod +x "$D/stage2/kaic2"

lacks=false
[ -n "$("$ROOT/stage2/kaic2" --backend-tag)" ] || lacks=true

fail=0
count() { grep -c -e "$1" "$log" || true; }
# $1 label, $2 source stem, rest: build flags.
build() {
  label="$1"; stem="$2"; shift 2
  : > "$log"
  printf 'fn main() {\n  println("%s")\n}\n' "$stem" > "$work/$stem.kai"
  (cd "$work" && XDG_CACHE_HOME="$work/cache" "$D/bin/kai" build "$@" "$stem.kai" -o "$stem") 2> "$work/$stem.err" \
    || { echo "no-llvm-probe FAIL ($label): build failed"; cat "$work/$stem.err"; fail=1; return; }
  [ "$("$work/$stem")" = "$stem" ] || { echo "no-llvm-probe FAIL ($label): the binary did not run"; fail=1; }
}
expect() {
  [ "$2" = "$3" ] || { echo "no-llvm-probe FAIL ($1): want $2, got $3"; fail=1; }
}

build "implicit" implicit
expect "implicit: probes" 1 "$(count '^--backend-tag$')"
if $lacks; then
  expect "implicit: native compiles on a kaic2 without libLLVM" 0 "$(count '--emit=native')"
  expect "implicit: notes" 1 "$(grep -c 'native backend unavailable' "$work/implicit.err" || true)"
else
  [ "$(count '--emit=native')" -ge 1 ] || { echo "no-llvm-probe FAIL (implicit: no native compile with libLLVM)"; fail=1; }
fi

# Once kaic2 has settled, its answer is kept: the next run does not ask.
sleep 3
build "settled" settled
build "cached" cached
expect "cached: probes" 0 "$(count '^--backend-tag$')"
if $lacks; then
  expect "cached: native compiles on a kaic2 without libLLVM" 0 "$(count '--emit=native')"
fi

build "explicit c" explicit_c --backend=c
expect "explicit c: probes" 0 "$(count '^--backend-tag$')"
expect "explicit c: native compiles" 0 "$(count '--emit=native')"

[ "$fail" = 0 ] && echo "no-llvm-probe OK"
exit "$fail"
