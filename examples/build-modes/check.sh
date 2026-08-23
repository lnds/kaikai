#!/bin/sh
# Build-modes acceptance check (#500). Verifies the three native build
# profiles on the panic_trace fixture:
#   --debug   : binary (or its .dSYM) carries DWARF whose line table names
#               the .kai source, AND a panic prints a kaikai-source stack
#               trace naming the .kai file.
#   --release : binary is stripped and measurably smaller than the default.
#   default   : still builds and runs (no regression).
#
# Usage: check.sh <path-to-bin/kai> [workdir]
# Exits 0 on pass, non-zero on the first failed assertion (with a message).
#
# Skips gracefully (exit 0 with a note) when the native backend is absent
# (a cc-only kaic2): DWARF is a native-only feature, so a C-only build has
# nothing to assert. CI runs this only on the KAI_LLVM=1 native build.
set -eu

KAI="${1:?usage: check.sh <bin/kai> [workdir]}"
WORK="${2:-$(mktemp -d)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/panic_trace.kai"
mkdir -p "$WORK"

fail() { echo "build-modes check FAIL: $1" >&2; exit 1; }

# Detect native availability: a default build that prints the C-only note
# means no native backend → skip (DWARF is native-only).
if "$KAI" build "$SRC" -o "$WORK/probe" 2>"$WORK/probe.err"; then
  if grep -q "native backend unavailable" "$WORK/probe.err"; then
    echo "build-modes check SKIP: native backend not built (cc-only kaic2)"
    exit 0
  fi
else
  fail "default build errored: $(cat "$WORK/probe.err")"
fi

# --- default: runs, exits non-zero via the panic, prints the message ---
def_out="$("$WORK/probe" 2>&1 || true)"
echo "$def_out" | grep -q "before panic" || fail "default binary did not run the body"
echo "$def_out" | grep -q "panic: boom" || fail "default binary did not panic"
def_size=$(wc -c < "$WORK/probe")

# --- --release: stripped, smaller than default ---
"$KAI" build --release "$SRC" -o "$WORK/rel" 2>"$WORK/rel.err" || fail "release build errored: $(cat "$WORK/rel.err")"
rel_size=$(wc -c < "$WORK/rel")
[ "$rel_size" -lt "$def_size" ] || fail "release ($rel_size) not smaller than default ($def_size)"
"$WORK/rel" >/dev/null 2>&1 || true   # must still run (panics, so non-zero is fine)
echo "build-modes check: release $rel_size < default $def_size bytes (stripped) OK"

# --- default and --release must not share a cached core object ---
# They emit at different codegen levels, so their objects differ. The shared
# core-object cache is keyed by the backend tag; if the tag omitted the
# codegen level, whichever profile built first would serve its object to the
# other and silently hand a release build unoptimised core code. Interleaving
# the profiles must not perturb either one's output.
#
# Both baselines are taken warm (each profile has already populated its own
# cache entry above), because a cold-cache and a warm-cache link of the same
# input differ in bytes anyway — the Mach-O link stamps a build UUID — while
# keeping the same size. Size is what discriminates a served-from-the-wrong-
# profile object here.
"$KAI" build "$SRC" -o "$WORK/probe_w" 2>/dev/null || fail "default warm rebuild errored"
"$KAI" build --release "$SRC" -o "$WORK/rel_w" 2>/dev/null || fail "release warm rebuild errored"
def_warm=$(wc -c < "$WORK/probe_w"); rel_warm=$(wc -c < "$WORK/rel_w")
"$KAI" build "$SRC" -o "$WORK/probe2" 2>"$WORK/probe2.err" || fail "default rebuild after release errored: $(cat "$WORK/probe2.err")"
"$KAI" build --release "$SRC" -o "$WORK/rel2" 2>"$WORK/rel2.err" || fail "release rebuild after default errored: $(cat "$WORK/rel2.err")"
[ "$(wc -c < "$WORK/probe2")" -eq "$def_warm" ] || fail "default build changed size after a release build interleaved (core-object cache key ignores the codegen level)"
[ "$(wc -c < "$WORK/rel2")" -eq "$rel_warm" ] || fail "release build changed size after a default build interleaved (core-object cache key ignores the codegen level)"
[ "$rel_warm" -ne "$def_warm" ] || fail "release and default produced identical-size binaries (the codegen level is not reaching the emitter)"
echo "$("$WORK/probe2" 2>&1 || true)" | grep -q "panic: boom" || fail "default rebuild did not panic"
echo "build-modes check: default and release keep distinct core objects across interleaved builds OK"

# --- --debug: DWARF present + panic prints a .kai:line stack trace ---
"$KAI" build --debug "$SRC" -o "$WORK/dbg" 2>"$WORK/dbg.err" || fail "debug build errored: $(cat "$WORK/dbg.err")"

# DWARF presence: dwarfdump over the binary (ELF: in-binary) or its .dSYM
# (Mach-O: dsymutil bundle). The line table must name the .kai source.
dw=""
if command -v dwarfdump >/dev/null 2>&1; then
  if [ -d "$WORK/dbg.dSYM" ]; then dw=$(dwarfdump --debug-line "$WORK/dbg.dSYM" 2>/dev/null || true)
  else dw=$(dwarfdump --debug-line "$WORK/dbg" 2>/dev/null || true); fi
elif command -v objdump >/dev/null 2>&1; then
  dw=$(objdump --dwarf=decodedline "$WORK/dbg" 2>/dev/null || true)
elif command -v readelf >/dev/null 2>&1; then
  dw=$(readelf --debug-dump=decodedline "$WORK/dbg" 2>/dev/null || true)
fi
[ -n "$dw" ] || fail "no DWARF dumper available (dwarfdump/objdump/readelf) to verify --debug"
echo "$dw" | grep -q "panic_trace.kai" || fail "--debug DWARF line table does not name panic_trace.kai"
echo "build-modes check: --debug DWARF line table names panic_trace.kai OK"

# Panic stack trace: the --debug binary's panic must print a kaikai-source
# trace naming the .kai file (resolved via atos/addr2line over the DWARF).
dbg_rc=0
dbg_out="$("$WORK/dbg" 2>&1)" || dbg_rc=$?
# A panic exits 1. Anything else means the trace path itself died — a signal
# status (128+n) is what a stack walk that runs off the fiber stack looks like.
[ "$dbg_rc" -eq 1 ] || fail "--debug panic exited $dbg_rc, expected 1 (the backtrace path crashed)"
echo "$dbg_out" | grep -q "panic: boom" || fail "--debug binary did not panic"
echo "$dbg_out" | grep -q "stack trace (kaikai source)" || fail "--debug panic printed no stack-trace header"
echo "$dbg_out" | grep -q "panic_trace.kai" || fail "--debug panic stack trace did not resolve to panic_trace.kai"
echo "build-modes check: --debug panic stack trace resolves to panic_trace.kai OK"

echo "build-modes check PASS"
