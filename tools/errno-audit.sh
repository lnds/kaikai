#!/bin/bash
# The runtime reads and writes `errno` only through kai_errno() / kai_set_errno().
#
# A fiber parks on one worker thread and resumes on another, and the compiler
# may keep the previous thread's `errno` address across the park. A raw `errno`
# in runtime code therefore reads another thread's value. The two accessors
# (stage2/runtime/host.h) are noinline, so the address is resolved on the
# thread running the call.
#
# Usage:
#   tools/errno-audit.sh              audit the runtime sources
#   tools/errno-audit.sh --self-test  prove the audit discriminates
set -u
export LC_ALL=C
cd "$(dirname "$0")/.."

# Prints "<file>:<line>: <text>" for every raw `errno` in code: comments,
# string literals and preprocessor lines are ignored, and so are the bodies of
# the two accessors.
scan() {
  awk '
    FNR == 1 { incomment = 0; inacc = 0; depth = 0 }
    {
      line = $0
      out = ""
      i = 1
      n = length(line)
      while (i <= n) {
        two = substr(line, i, 2)
        ch = substr(line, i, 1)
        if (incomment) {
          if (two == "*/") { incomment = 0; i += 2 } else i++
        } else if (two == "/*") { incomment = 1; i += 2 }
        else if (two == "//") break
        else if (ch == "\"" || ch == "\x27") {
          q = ch; i++
          while (i <= n && substr(line, i, 1) != q) { if (substr(line, i, 1) == "\\") i++; i++ }
          i++
          out = out " "
        } else { out = out ch; i++ }
      }
      if (out ~ /^[ \t]*#/) next
      if (out ~ /(^|[^A-Za-z0-9_])kai_(set_)?errno[ \t]*\([^)]*\)[ \t]*\{/) { inacc = 1; depth = 0 }
      if (inacc) {
        o = gsub(/\{/, "{", out); c = gsub(/\}/, "}", out)
        depth += o - c
        if (depth <= 0 && (o > 0 || c > 0)) inacc = 0
        next
      }
      if (out ~ /(^|[^A-Za-z0-9_])errno([^A-Za-z0-9_]|$)/) print FILENAME ":" FNR ": " $0
    }' "$@"
}

if [ "${1:-}" = "--self-test" ]; then
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  cat > "$tmp/bad.h" <<'E'
static int f(void) { return errno; }
E
  cat > "$tmp/bad_write.h" <<'E'
static void f(void) {
    errno = 0;
}
E
  cat > "$tmp/good.h" <<'E'
#include <errno.h>
/* errno in a comment is fine */
static __attribute__((noinline)) int kai_errno(void) {
    return errno;
}
static int f(void) { int e = kai_errno(); const char *s = "errno"; return e + saved_errno + s[0]; }
E
  [ -n "$(scan "$tmp/bad.h")" ] || { echo "errno-audit self-test FAIL: a raw read passed"; exit 1; }
  [ -n "$(scan "$tmp/bad_write.h")" ] || { echo "errno-audit self-test FAIL: a raw write passed"; exit 1; }
  [ -z "$(scan "$tmp/good.h")" ] || { echo "errno-audit self-test FAIL: clean file rejected"; scan "$tmp/good.h"; exit 1; }
  echo "errno-audit self-test OK"
  exit 0
fi

files=$(ls stage2/runtime/*.h stage2/runtime.h stage0/runtime_llvm.c stage2/runtime_owner_c.c stage2/runtime_llvm_native_shim.c 2>/dev/null)
bad=$(scan $files)
if [ -n "$bad" ]; then
  echo "errno-audit FAIL — raw errno in the runtime; use kai_errno() / kai_set_errno():"
  echo "$bad" | sed 's/^/  /'
  exit 1
fi
echo "errno-audit OK ($(echo "$files" | wc -w | tr -d ' ') files)"
