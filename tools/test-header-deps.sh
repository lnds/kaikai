#!/usr/bin/env bash
# Contract gate for the bootstrap chain's declared header prerequisites.
#
# Every stage compiles C that `#include`s a header make cannot see: stage 1
# and stage 2 compile a GENERATED .c whose `#include "runtime.h"` resolves
# through -I order, and stage 0 compiles hand-written sources against its own
# headers. A rule that omits the header still builds correctly from scratch —
# CI never notices — but locally reports "up to date" after a header-only
# edit, and the next gate silently measures the previous runtime.
#
# The reverse omission costs too: naming a header the translation unit never
# reaches turns an unrelated edit into a full -O2 rebuild of the compiler. So
# both directions are asserted.
#
# Static query of the make database (`make -q -p`), which runs no recipe and
# writes nothing: milliseconds, no kaic2 dependency, no side effect on the
# tree — tier 0.

set -uo pipefail
cd "$(dirname "$0")/.."

fail=0

# The prerequisite line make itself reports for a target, whether it comes
# from an explicit rule or an applied pattern rule.
prereqs() {
  local dir="$1" target="$2" escaped
  escaped="$(printf '%s' "$2" | sed 's/[.[\*^$]/\\&/g')"
  make -q -p -C "$dir" "$target" 2>/dev/null | sed -n "s/^$escaped: //p" | head -1
}

declares() {
  case " $(prereqs "$1" "$2") " in *" $3 "*) return 0 ;; *) return 1 ;; esac
}

# The header this target compiles against must be a prerequisite.
expect() {
  if declares "$1" "$2" "$3"; then
    echo "  ok   $1/$2 declares $3"
  else
    echo "  FAIL $1/$2: '$3' missing from prerequisites: $(prereqs "$1" "$2")"
    fail=1
  fi
}

# A header this target never reaches must NOT be one: it would turn unrelated
# edits into a rebuild.
reject() {
  if declares "$1" "$2" "$3"; then
    echo "  FAIL $1/$2 over-declares '$3' — unrelated edits force a rebuild"
    fail=1
  else
    echo "  ok   $1/$2 does not over-declare $3"
  fi
}

echo "== test-header-deps =="

# stage 0's objects are compiled from hand-written sources; runtime.h is
# emitted into generated programs and never compiled into kaic0.
expect stage0 main.o lexer.h
reject stage0 main.o runtime.h

# stage 1 links its generated C with `-I ../stage0` only.
expect stage1 kaic1 ../stage0/runtime.h

# stage 2 leads with `-I .`, so its generated C binds to stage2/runtime.h and
# never reaches the stage 0 header behind it.
expect stage2 kaic2     runtime.h
reject stage2 kaic2     ../stage0/runtime.h
expect stage2 kaic2-dev runtime.h
reject stage2 kaic2-dev ../stage0/runtime.h

if [ "$fail" -eq 0 ]; then
  echo "test-header-deps OK — a header-only edit rebuilds every artifact that embeds it"
else
  echo "test-header-deps FAILED"
fi
exit "$fail"
