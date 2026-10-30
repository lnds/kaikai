#!/usr/bin/env python3
"""Run a command and fail when its peak resident set exceeds a limit.

usage: max-rss-check.py <limit-mb> <command> [args...]
The command's stdout and stderr pass through; its exit status must be 0.
"""
import resource
import subprocess
import sys


def main():
    limit_mb = int(sys.argv[1])
    rc = subprocess.call(sys.argv[2:])
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    peak_mb = peak / (1024 * 1024) if sys.platform == "darwin" else peak / 1024
    print("peak-rss: %.1f MB (limit %d MB)" % (peak_mb, limit_mb))
    if rc != 0:
        sys.exit(rc)
    if peak_mb > limit_mb:
        print("peak-rss FAIL: over the limit")
        sys.exit(1)


main()
