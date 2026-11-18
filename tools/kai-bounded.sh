#!/bin/sh
# kai-bounded.sh <seconds> <cmd> [args...]: bounded run that names the deadline on stderr.
# A command still running at the deadline has its thread stacks dumped to stderr before it is killed.
# Exit status: 124 on deadline, otherwise the command's own.

secs=$1; shift

dump_stacks() {
  pid=$1
  for t in /proc/"$pid"/task/*; do
    [ -d "$t" ] || continue
    echo "thread ${t##*/}: wchan=$(cat "$t/wchan" 2>/dev/null) state=$(sed -n 's/^State:[[:space:]]*//p' "$t/status" 2>/dev/null)"
  done
  if command -v gdb >/dev/null 2>&1; then
    gdb -p "$pid" -batch -ex 'thread apply all bt' 2>&1
  elif command -v eu-stack >/dev/null 2>&1; then
    eu-stack -p "$pid" 2>&1
  elif command -v lldb >/dev/null 2>&1; then
    lldb -p "$pid" -b -o 'thread backtrace all' 2>&1
  else
    echo "no debugger on PATH (gdb, eu-stack, lldb)"
  fi
}

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

"$@" <&0 &
pid=$!

(
  sleep "$secs" &
  nap=$!
  trap 'kill "$nap" 2>/dev/null; exit 0' TERM
  wait "$nap"
  kill -0 "$pid" 2>/dev/null || exit 0
  : > "$dir/fired"
  echo "--- thread stacks of pid $pid at the deadline ---" >&2
  dump_stacks "$pid" >&2
  echo "--- end of thread stacks ---" >&2
  kill -TERM "$pid" 2>/dev/null
  sleep 2
  kill -KILL "$pid" 2>/dev/null
) &
watcher=$!

rc=0
wait "$pid" || rc=$?
kill "$watcher" 2>/dev/null
wait "$watcher" 2>/dev/null

if [ -e "$dir/fired" ]; then
  echo "TIMEOUT after ${secs}s: $*" >&2
  exit 124
fi
exit "$rc"
