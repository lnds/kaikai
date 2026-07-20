#!/usr/bin/env bash
# Portable bounded-run shim. Source it for `kai_timeout`, or execute it
# directly as a drop-in `timeout <seconds> <cmd> [args...]`.
#
# `timeout(1)` is GNU coreutils: present on Linux CI, absent from a stock
# macOS. A harness that shells out to it there gets exit 127 (command not
# found) on EVERY run, which reads as a hang and turns the whole gate into
# noise. Resolve the implementation once at source time and expose one
# function with GNU's contract.
#
#   kai_timeout <seconds> <cmd> [args...]
#
# Exit status: 124 if the deadline fired, 137 if the child had to be killed
# with SIGKILL, otherwise the command's own status (128+N when it died on
# signal N).
#
# The deadline sends SIGTERM and escalates to SIGKILL after a 2s grace, so a
# fiber runtime wedged in a scheduler loop still cannot outlive it. `-s KILL`
# is simpler but coreutils reports 137 for ANY SIGKILLed child, which makes
# 124 unreachable and collapses "deadline fired" into "had to be killed" —
# including a kill from elsewhere, e.g. the OOM killer. The perl fallback has
# no escalation phase: it kills outright and always reports 124.
#
# KAI_TIMEOUT_KIND names the backing implementation (gnu|gtimeout|perl|none)
# so a caller can report degraded coverage instead of silently mismeasuring.

if command -v timeout >/dev/null 2>&1; then
  KAI_TIMEOUT_KIND=gnu
elif command -v gtimeout >/dev/null 2>&1; then
  KAI_TIMEOUT_KIND=gtimeout
elif command -v perl >/dev/null 2>&1; then
  KAI_TIMEOUT_KIND=perl
else
  KAI_TIMEOUT_KIND=none
fi

# Deadline enforced by SIGALRM in the parent; the child is exec'd directly so
# no intermediate shell swallows the signal or the exit status.
_KAI_TIMEOUT_PERL='
my $secs = shift @ARGV;
my $pid = fork();
die "fork: $!" unless defined $pid;
if ($pid == 0) { exec { $ARGV[0] } @ARGV; exit 127; }
$SIG{ALRM} = sub { kill "KILL", $pid; waitpid($pid, 0); exit 124; };
alarm $secs;
waitpid($pid, 0);
alarm 0;
my $st = $?;
exit($st & 127 ? 128 + ($st & 127) : $st >> 8);
'

kai_timeout() {
  local secs="$1"; shift
  case "$KAI_TIMEOUT_KIND" in
    gnu)      timeout -k 2 "$secs" "$@" ;;
    gtimeout) gtimeout -k 2 "$secs" "$@" ;;
    perl)     perl -e "$_KAI_TIMEOUT_PERL" "$secs" "$@" ;;
    *)        "$@" ;;
  esac
}

# Direct-execution detection must stay POSIX: Makefile recipes source this
# file under /bin/sh (dash on Linux), where ${BASH_SOURCE[0]} is a parse
# error, not just an empty value.
case "$0" in
  *timeout.sh)
    if [ "$KAI_TIMEOUT_KIND" = none ]; then
      echo "warning: no timeout implementation (timeout/gtimeout/perl); running unbounded" >&2
    fi
    kai_timeout "$@"
    ;;
esac
