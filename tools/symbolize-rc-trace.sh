#!/usr/bin/env bash
# tools/symbolize-rc-trace.sh — issue #296
#
# Translate the addresses in a `[KAI_TRACE_RC] site` report into
# nearest-symbol names by reading `nm <binary>` and the slide line
# emitted by the runtime.
#
# Usage:
#   tools/symbolize-rc-trace.sh <binary> <trace.log>
#
# Reads the trace output from <trace.log>, parses the
# `[KAI_TRACE_RC] aslr_slide=0x..` line and every `site N 0x..`
# line, and rewrites each address to `<symbol> + 0x<offset>`.
# Output goes to stdout — pipe to less or save as needed.
#
# This is best-effort. The tracer captures __builtin_return_address(0)
# inside each KAI_RC_NOINLINE wrapper, which is the address of the
# instruction immediately AFTER the call. nm only knows function
# starts, so the result is "<caller> + 0x<offset>" — enough to point
# at the right function but not the line number. Run `atos -o
# <binary> -l <slide> <addr>` for line-level resolution if a dSYM is
# present and atos cooperates.

set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 <binary> <trace.log>" >&2
    exit 2
fi

BIN="$1"
LOG="$2"

if [ ! -f "$BIN" ]; then echo "error: binary $BIN not found" >&2; exit 2; fi
if [ ! -f "$LOG" ]; then echo "error: trace log $LOG not found" >&2; exit 2; fi

python3 - "$BIN" "$LOG" <<'PY'
import re, subprocess, sys

binary, log = sys.argv[1], sys.argv[2]

nm = subprocess.run(["nm", binary], capture_output=True, text=True, check=True).stdout
syms = []
for line in nm.splitlines():
    parts = line.split()
    if len(parts) < 3: continue
    if parts[1] in ("U", "u"): continue
    try: a = int(parts[0], 16)
    except ValueError: continue
    syms.append((a, parts[2]))
syms.sort()

def lookup(target):
    lo, hi = 0, len(syms)
    while lo < hi:
        m = (lo + hi) // 2
        if syms[m][0] <= target: lo = m + 1
        else: hi = m
    if lo == 0: return None
    a, name = syms[lo - 1]
    return f"{name} + 0x{target - a:x}"

slide = 0
slide_re = re.compile(r"aslr_slide=0x([0-9a-f]+)")
site_re  = re.compile(r"^(\[KAI_TRACE_RC\] site .* )(0x[0-9a-f]+)( .*)$")

with open(log) as f:
    for line in f:
        m = slide_re.search(line)
        if m:
            slide = int(m.group(1), 16)
            sys.stdout.write(line)
            continue
        m = site_re.match(line)
        if m:
            addr = int(m.group(2), 16) - slide
            sym = lookup(addr) or f"<unknown 0x{addr:x}>"
            sys.stdout.write(f"{m.group(1)}{m.group(2)} ({sym}){m.group(3)}\n")
            continue
        sys.stdout.write(line)
PY
