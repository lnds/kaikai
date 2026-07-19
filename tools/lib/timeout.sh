#!/usr/bin/env bash
# Portable bounded-run shim. Sourced, not executed.
#
# `timeout(1)` is GNU coreutils: present on Linux CI, absent from a stock
# macOS. A harness that shells out to it there gets exit 127 (command not
# found) on EVERY run, which reads as a hang and turns the whole gate into
# noise. Resolve the implementation once at source time and expose one
# function with GNU's contract.
#
#   kai_timeout <seconds> <cmd> [args...]
#
# Exit status: 124 if the deadline fired, otherwise the command's own status
# (128+N when it died on signal N). The child is killed with SIGKILL, not
# SIGTERM: a fiber runtime wedged in a scheduler loop may never reach a
# handler, and this shim is used to prove termination.
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
    gnu)      timeout -s KILL "$secs" "$@" ;;
    gtimeout) gtimeout -s KILL "$secs" "$@" ;;
    perl)     perl -e "$_KAI_TIMEOUT_PERL" "$secs" "$@" ;;
    *)        "$@" ;;
  esac
}
