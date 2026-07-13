#!/usr/bin/env bash
# Static classification gate for the runtime's mutable file-scope globals.
#
# The M:N scheduler partitions every mutable global into one of a fixed
# set of classes (docs/mn-scheduler-design.md §1). A global that should be
# `_Thread_local` but is left shared is a data race that passes every
# functional test under one thread and corrupts silently under load —
# stdout-diff cannot see it. This gate makes the partition TOTAL BY
# CONSTRUCTION: every mutable global in the runtimes must appear in the
# allow-list `tools/runtime-globals.allow` annotated with its class, and
# every allow-list entry must still exist in the source. A new
# unclassified global fails the build; a removed one flags a stale entry.
#
# Classes (the allow-list's second column):
#   tls            Class A — per-scheduler-thread; must be _Thread_local.
#   immutable      Class B — write-once at startup, read-only after.
#   immortal       Class B — rc=INT32_MAX, shared read-only across threads.
#   shared-locked  Class C — process-global mutable; mutex or atomic.
#   reactor-owned  Class C — single reactor owner; mutated via its interface.
#   scratch        per-function return/scratch buffer; no cross-thread
#                  identity (referenced by exactly one function).
#
# Wired into the root Makefile as `test-runtime-global-audit`, part of the
# tier-1 chain. Pure shell — no compiler needed, so it runs fast and gates
# a source-level property, not a build artifact.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
ALLOW="$ROOT/tools/runtime-globals.allow"

# The runtimes carrying mutable state. stage2/runtime.h is the C-backend
# runtime AND the one stage0/runtime_llvm.c #includes, so a TLS decision
# there covers both backends; runtime_llvm.c adds only its dispatch
# catalogs. stage0/runtime.h is the older stage0 mirror.
FILES=(
  "stage2/runtime.h"
  "stage0/runtime_llvm.c"
  "stage0/runtime.h"
)

# Capture the mode up front — the allow-list parser below reassigns $1
# via `set --`, so read the flag before it is clobbered.
MODE="${1:-}"

if [ ! -f "$ALLOW" ]; then
  echo "runtime-global-audit: missing allow-list $ALLOW" >&2
  exit 2
fi

# enumerate <file>: print the identifier of every mutable file-scope global
# DEFINITION (not `extern` declarations, not functions, not macros, not
# function-local statics, not typedefs). A definition is a column-0 line,
# optionally `static`, with a type + identifier (+ optional array dims /
# initializer), ending in `;`. Skip: `(` on the line (function / macro),
# `#` at column 0 (preprocessor), a leading `}` (a `} Name;` struct/enum
# closer names a TYPE, not a variable), and `typedef` (a type alias).
# Indented `static` is a function-local, not a global — the column-0 anchor
# excludes it.
# A definition may wrap: `[static] Type` on one line, then an indented
# `name[DIMS];` on the next. Join such continuations first — a line that is
# a bare type token (a leading `static`/`const`/type word, no `;`, no `(`,
# no `#`, no `{`) fused with the following indented line — so the one-line
# matcher below sees the whole declaration.
enumerate() {
  local f="$1"
  awk '
    prev != "" {
      sub(/^[[:space:]]+/, "", $0)
      print prev " " $0
      prev = ""
      next
    }
    /^(static +)?(const +)?[A-Za-z_][A-Za-z0-9_]*[[:space:]]*$/ &&
      $0 !~ /[;(){}#]/ { prev = $0; next }
    { print }
  ' "$f" \
    | grep -E '^(static +)?(const +)?[A-Za-z_][A-Za-z0-9_]*[A-Za-z0-9_ ]*[ *]+[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])*( *=[^;]*)?;[[:space:]]*(/\*.*)?$' \
    | grep -vE '^[[:space:]]*(extern |typedef )' \
    | grep -vE '\(' \
    | grep -vE '^#' \
    | grep -vE '^\}' \
    | sed -E 's/^(static +)?(const +)?[A-Za-z_][A-Za-z0-9_]*[A-Za-z0-9_ ]*[ *]+([A-Za-z_][A-Za-z0-9_]*)(\[[^]]*\])*( *=.*)?;.*/\3/' \
    | sort -u
}

# The allow-list is `<symbol> <class>` lines; `#` comments and blank lines
# are ignored. Build the set of allowed symbols and validate the class.
declare -A ALLOWED_CLASS
VALID_CLASSES="tls immutable immortal shared-locked reactor-owned scratch"
bad_class=0
while IFS= read -r line; do
  line="${line%%#*}"
  # shellcheck disable=SC2086
  set -- $line
  [ "$#" -eq 0 ] && continue
  if [ "$#" -ne 2 ]; then
    echo "runtime-global-audit: malformed allow-list line: $line" >&2
    bad_class=1
    continue
  fi
  sym="$1"; cls="$2"
  case " $VALID_CLASSES " in
    *" $cls "*) ALLOWED_CLASS["$sym"]="$cls" ;;
    *) echo "runtime-global-audit: unknown class '$cls' for '$sym'" >&2; bad_class=1 ;;
  esac
done < "$ALLOW"

# Enumerate every global across the runtimes (deduped across files: the
# same symbol appears in stage2 and its stage0 mirror).
declare -A SEEN
for f in "${FILES[@]}"; do
  [ -f "$f" ] || continue
  while IFS= read -r sym; do
    [ -z "$sym" ] && continue
    SEEN["$sym"]=1
  done < <(enumerate "$f")
done

unclassified=0
for sym in "${!SEEN[@]}"; do
  if [ -z "${ALLOWED_CLASS[$sym]:-}" ]; then
    echo "runtime-global-audit: UNCLASSIFIED mutable global '$sym'" >&2
    echo "  → add it to tools/runtime-globals.allow with one of: $VALID_CLASSES" >&2
    unclassified=$((unclassified + 1))
  fi
done

stale=0
for sym in "${!ALLOWED_CLASS[@]}"; do
  if [ -z "${SEEN[$sym]:-}" ]; then
    echo "runtime-global-audit: STALE allow-list entry '$sym' (no longer defined)" >&2
    stale=$((stale + 1))
  fi
done

total="${#SEEN[@]}"
echo "runtime-global-audit: globals=$total classified=$((total - unclassified)) unclassified=$unclassified stale=$stale"
main_ok=0
{ [ "$unclassified" -eq 0 ] && [ "$stale" -eq 0 ] && [ "$bad_class" -eq 0 ]; } || main_ok=1

# --self-test proves the gate BREAKS on a new unclassified global: inject one
# into a scratch copy of the primary runtime, re-enumerate, and require the
# probe to surface as unclassified. A gate that cannot fail is not a gate.
if [ "$MODE" = "--self-test" ]; then
  tmp="$(mktemp)"
  cp "stage2/runtime.h" "$tmp"
  printf '\nstatic int kai_audit_selftest_probe = 0;\n' >> "$tmp"
  if enumerate "$tmp" | grep -qx 'kai_audit_selftest_probe'; then
    echo "runtime-global-audit: self-test OK (an unclassified global is detected)"
    rm -f "$tmp"
  else
    echo "runtime-global-audit: SELF-TEST FAILED — enumerator missed an injected global" >&2
    rm -f "$tmp"
    exit 1
  fi
fi

exit "$main_ok"
