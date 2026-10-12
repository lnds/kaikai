#!/bin/bash
# Logging cc shim: times each call; a link gets a timer object so the binary logs its own run time.
S=$(dirname "$(readlink -f "$0")")
link=1; out=""; prev=""; csrc=""; opt="-O?"; lines=0; impls=0; sep=0; asan=0
for a in "$@"; do
  case "$a" in -c|-E|-S|-M|-MM|-fsyntax-only|--version|-v|-dumpversion|-dumpmachine|-print-*|-shared|-nostdlib) link=0;; esac
  case "$a" in -O*) opt="$a";; -DKAI_SEPARATE_COMPILATION=1) sep=1;; -fsanitize=*) asan=1;; esac
  [ "$prev" = "-o" ] && out="$a"
  case "$a" in *.c) [ "$prev" != "-o" ] && [ -f "$a" ] && { csrc="$a"; lines=$(( lines + $(wc -l < "$a") )); n=$(grep -o 'kai_register_impls(_kai_impl_table_entries, [0-9]*' "$a" | grep -o '[0-9]*$'); impls=$(( impls + ${n:-0} )); };; esac
  prev="$a"
done
extra=()
[ $link = 1 ] && [ -n "$out" ] && extra=("$S/runtime_timer.o")
export KPERF_INFO="link=$link src=${csrc:+1} lines=$lines impls=$impls opt=$opt sep=$sep san=$asan"
"$S/cpuexec" cc "${KPERF_REAL_CC:-clang}" "$@" "${extra[@]}"; rc=$?
exit $rc
