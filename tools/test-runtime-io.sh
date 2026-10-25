#!/bin/sh
# tools/test-runtime-io.sh <c|native> — default Stdin handler byte order
# when read_line and read_bytes share one buffered stream.

set -eu

BACKEND="${1:?usage: test-runtime-io.sh <c|native>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/stage2/build/runtime-io-$BACKEND"
mkdir -p "$OUT"

build() {
  KAI_BACKEND="$BACKEND" "$ROOT/bin/kai" build "$ROOT/examples/runtime/$1.kai" -o "$OUT/$1" 2> "$OUT/$1.err" \
    || { echo "runtime-io BUILD FAIL ($BACKEND): $1"; cat "$OUT/$1.err"; exit 1; }
}

build stdin_buffered_mix
printf 'first\nabcdefgh\n\nlast\r\ntail-no-newline' | "$OUT/stdin_buffered_mix" > "$OUT/stdin_buffered_mix.out"
diff "$ROOT/examples/runtime/stdin_buffered_mix.out.expected" "$OUT/stdin_buffered_mix.out" \
  || { echo "runtime-io DIFF ($BACKEND): stdin_buffered_mix"; exit 1; }

# stdout off a TTY is block-buffered but flushed before the process waits.
build stdout_before_sleep
for threads in 1 4; do
  : > "$OUT/sleep.out"
  KAI_THREADS=$threads "$OUT/stdout_before_sleep" >> "$OUT/sleep.out" &
  pid=$!
  n=0
  while [ ! -s "$OUT/sleep.out" ] && [ "$n" -lt 300 ]; do sleep 0.1; n=$((n + 1)); done
  kill -TERM "$pid" 2> /dev/null || true
  wait "$pid" 2> /dev/null || true
  [ "$(cat "$OUT/sleep.out")" = "before sleep" ] \
    || { echo "runtime-io FAIL ($BACKEND, KAI_THREADS=$threads): output written before a sleep did not reach the pipe (waited ${n} ticks, $(wc -c < "$OUT/sleep.out") bytes)"; exit 1; }
done

# A fiber blocked in Stdin.read_line parks: another fiber's output reaches the
# pipe while the writer still holds stdin open.
build stdin_read_parks_fiber
for threads in 1 4; do
  : > "$OUT/park.out"
  (echo hi; sleep 20) | KAI_THREADS=$threads "$OUT/stdin_read_parks_fiber" >> "$OUT/park.out" &
  pid=$!
  n=0
  while [ ! -s "$OUT/park.out" ] && [ "$n" -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
  kill -TERM "$pid" 2> /dev/null || true
  wait "$pid" 2> /dev/null || true
  [ "$(cat "$OUT/park.out")" = "got hi" ] \
    || { echo "runtime-io FAIL ($BACKEND, KAI_THREADS=$threads): a fiber blocked in Stdin.read_line stalled the others (waited ${n} ticks)"; exit 1; }
done

# stdout lines printed before a panic precede the panic message on a shared pipe.
build stdout_before_panic
"$OUT/stdout_before_panic" > "$OUT/panic.out" 2>&1 || true
[ "$(sed -n 1,2p "$OUT/panic.out" | tr '\n' ' ')" = "one two " ] && sed -n 3p "$OUT/panic.out" | grep -q '^panic: boom' \
  || { echo "runtime-io FAIL ($BACKEND): stdout/stderr order around a panic"; cat "$OUT/panic.out"; exit 1; }

# One write per buffer, not per line.
build stdout_many_lines
"$OUT/stdout_many_lines" > "$OUT/many.out"
[ "$(wc -l < "$OUT/many.out" | tr -d ' ')" = "200000" ] \
  || { echo "runtime-io FAIL ($BACKEND): stdout_many_lines lost lines"; exit 1; }
if command -v strace > /dev/null 2>&1; then
  strace -f -c -e trace=write -o "$OUT/many.strace" "$OUT/stdout_many_lines" > /dev/null
  w=$(awk '$NF == "write" { print $4 }' "$OUT/many.strace")
  [ "${w:-0}" -lt 1000 ] \
    || { echo "runtime-io FAIL ($BACKEND): $w write(2) calls for 200000 lines"; exit 1; }
fi

echo "runtime-io OK ($BACKEND)"
