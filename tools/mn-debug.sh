#!/usr/bin/env bash
# Drive the kaikai concurrency-debug container (docker/mn-debug.Dockerfile).
#
# The host macOS toolchain cannot attach to a hung kaikai process
# (SIP/signing block lldb -p / sample); this runs the same code on Linux
# under gdb / rr / valgrind, which do. The worktree is bind-mounted so the
# container compiles the current source with its own Linux toolchain.
#
# Usage:
#   tools/mn-debug.sh build              # bootstrap kaic2 (C-only) + bin/kai inside the container
#   tools/mn-debug.sh shell              # interactive shell (toolchain + debuggers)
#   tools/mn-debug.sh loop [N] [THREADS] # build the repro, run N times, report OK/hang/crash/bad
#   tools/mn-debug.sh gdb  [THREADS]     # run the repro under gdb; on hang, Ctrl-C then `thread apply all bt`
#   tools/mn-debug.sh rr   [THREADS]     # record the repro under rr (deterministic replay)
#   tools/mn-debug.sh helgrind [THREADS] # run the repro under valgrind helgrind (lock-order / races)
#   tools/mn-debug.sh drd  [THREADS]     # run the repro under valgrind drd
#   tools/mn-debug.sh bench [THREADS]    # run tools/run-mn-reactor-bench.sh inside the container
#
# ptrace: gdb and rr need CAP_SYS_PTRACE and an unconfined seccomp profile
# (both passed below). rr ALSO needs a low perf_event_paranoid on the VM
# kernel; Docker Desktop rejects `--sysctl kernel.perf_event_paranoid=1`, so
# rr is only usable on a host/VM where that sysctl can be lowered.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
IMAGE="kaikai-mn-debug"
DOCKERFILE="docker/mn-debug.Dockerfile"

# The repro: the F2 I/O+CPU fixture. Its golden output is the single line in
# the companion .out.expected; the loop reads it inside the container and
# compares, to tell a clean run from a hang (ec≠0), a crash (SIGABRT/SIGSEGV),
# or a corrupted run (wrong output).
REPRO="examples/effects/mn_reactor_io_cpu_mix.kai"

ensure_image() {
  if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "building $IMAGE ..." >&2
    docker build -f "$DOCKERFILE" -t "$IMAGE" .
  fi
}

# Run a script (passed as $1) inside the container: worktree at /work,
# ptrace + unconfined seccomp on. The script is written to a temp file under
# the mounted ROOT and executed as /work/<file> — sidesteps all the shell-
# quoting hazards of a multi-line `bash -lc "..."`. A TTY is allocated only
# for an interactive terminal, so scripted runs stay headless.
in_container() {
  ensure_image
  # The script is executed from a file, not stdin, so no -i is needed for
  # scripted runs. Allocate a full TTY (-it) only when stdin is a terminal,
  # so interactive gdb/shell work; headless runs pass neither and never
  # block waiting on a stdin that is not there.
  local ti=""
  if [ -t 0 ]; then ti="-it"; fi
  local base=".mn-debug-run.$$.sh"
  printf '%s\n' "$1" > "$ROOT/$base"
  local rc=0
  docker run --rm $ti \
    --cap-add=SYS_PTRACE \
    --security-opt seccomp=unconfined \
    -v "$ROOT":/work \
    -w /work \
    "$IMAGE" bash "/work/$base" || rc=$?
  rm -f "$ROOT/$base"
  return $rc
}

# Preamble prepended to every container script: bootstrap a C-only kaic2,
# robust to the bind-mount cross-arch trap. The worktree may hold a macOS
# (Mach-O) kaic2 from a host build, which the Linux container cannot exec and
# `make` thinks is up-to-date by timestamp. If stage2/kaic2 is not a Linux
# ELF, wipe the arch-specific artifacts and rebuild. Then build the repro.
PREAMBLE='set -e
if ! { [ -x stage2/kaic2 ] && file stage2/kaic2 | grep -q ELF; }; then
  find stage0 stage1 stage2 -name "*.o" -delete 2>/dev/null || true
  rm -f stage0/kaic0 stage1/kaic1 stage2/kaic2 2>/dev/null || true
  make KAI_LLVM=0 kaic2
fi
./bin/kai build --backend=c "'"$REPRO"'" -o /tmp/repro'

cmd="${1:-shell}"; shift || true

case "$cmd" in
  build)
    in_container "$PREAMBLE
chmod +x bin/kai
echo 'bootstrap OK'"
    ;;

  shell)
    in_container "exec bash"
    ;;

  loop)
    N="${1:-50}"; TH="${2:-4}"
    in_container "$PREAMBLE
set +e   # the loop runs the repro expecting failures (hang/crash); do not abort on them
golden=\"\$(cat '${REPRO%.kai}.out.expected')\"
ok=0; hangs=0; aborts=0; bad=0
for i in \$(seq 1 $N); do
  out=\"\$(KAI_THREADS=$TH timeout -s KILL 5 /tmp/repro 2>/dev/null)\"; ec=\$?
  if [ \$ec -eq 134 ] || [ \$ec -eq 139 ]; then aborts=\$((aborts+1))
  elif [ \$ec -ne 0 ]; then hangs=\$((hangs+1))
  elif [ \"\$out\" != \"\$golden\" ]; then bad=\$((bad+1))
  else ok=\$((ok+1)); fi
done
echo \"=== $N runs at KAI_THREADS=$TH: \$ok OK, \$hangs hangs, \$aborts crashes (SIGABRT/SIGSEGV), \$bad bad-output ===\"
[ \$ok -eq $N ] && echo PASS || echo FAIL"
    ;;

  gdb)
    TH="${1:-4}"
    in_container "$PREAMBLE
echo 'On hang: Ctrl-C, then: thread apply all bt'
KAI_THREADS=$TH gdb -q /tmp/repro"
    ;;

  rr)
    TH="${1:-4}"
    in_container "$PREAMBLE
echo 'Recording under rr. Replay after: rr replay -> thread apply all bt'
KAI_THREADS=$TH rr record /tmp/repro"
    ;;

  helgrind)
    TH="${1:-2}"
    in_container "$PREAMBLE
KAI_THREADS=$TH valgrind --tool=helgrind --error-exitcode=99 /tmp/repro"
    ;;

  drd)
    TH="${1:-2}"
    in_container "$PREAMBLE
KAI_THREADS=$TH valgrind --tool=drd --error-exitcode=99 /tmp/repro"
    ;;

  bench)
    TH="${1:-4}"
    in_container "$PREAMBLE
KAI_THREADS=$TH tools/run-mn-reactor-bench.sh"
    ;;

  *)
    echo "unknown command: $cmd" >&2
    sed -n '9,17p' "$0" >&2
    exit 2
    ;;
esac
