# Debug image for kaikai concurrency bugs that only reproduce under load.
#
# The host macOS toolchain cannot attach to a hung kaikai process
# (SIP/code-signing block lldb -p / sample), and TSAN-deadlock is a
# no-op there. This Linux image ships the debuggers that DO capture a
# hung multi-thread process: gdb (thread apply all bt on a live/attached
# process), rr (deterministic record-replay — the tool for lost-wakeup
# and other liveness races), and valgrind's helgrind/drd (lock-order and
# data-race checkers). ASAN/TSAN work here too.
#
# The image carries only the TOOLCHAIN. The kaikai bootstrap runs inside
# the container against a bind-mounted worktree, so the same image serves
# any state of the source. Build once, reuse across sessions.
#
#   docker build -f docker/mn-debug.Dockerfile -t kaikai-mn-debug .
#   tools/mn-debug.sh <command>          # see that script for the run flags
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain: C compiler + make for the bootstrap; the debuggers; rr's
# and gdb's ptrace needs are satisfied at run time via --cap-add. LLVM 18
# is present so the native backend can be built too, though the reactor
# deadlock reproduces on the portable C backend (the reactor is C emitted
# into runtime.h, backend-independent).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang-18 \
        llvm-18 \
        llvm-18-dev \
        libzstd-dev \
        zlib1g-dev \
        gdb \
        rr \
        valgrind \
        gawk \
        file \
        procps \
        ca-certificates \
    && ln -sf /usr/bin/clang-18 /usr/local/bin/clang \
    && ln -sf /usr/bin/llvm-config-18 /usr/local/bin/llvm-config \
    && rm -rf /var/lib/apt/lists/*

# rr needs a low perf_event_paranoid; set it defensively (the host sysctl
# is authoritative, but this documents the requirement).
ENV CC=cc
WORKDIR /work

CMD ["/bin/bash"]
