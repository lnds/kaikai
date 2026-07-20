#!/bin/sh
# gen-runtime-bc.sh — generate the native-runtime bitcode at BUILD TIME.
#
# P2 (docs/native-codegen-perf-plan.md §P2): the native backend links
# `stage0/runtime_llvm.bc` into the in-process LLVM module BEFORE the O2
# pass so the optimiser inlines the runtime ops (kaix_cons, the list spine,
# the arithmetic helpers) into hot heap-bound loops.
#
# WHY BUILD-TIME, NOT VENDORED. The bitcode encodes target-specific layout
# decisions (struct padding, alloca sizes, GEP byte offsets) under the data
# layout it was compiled for. A single committed .bc is NOT portable: a
# Mach-O (mac) bitcode linked into an ELF (Linux) module would mis-codegen
# silently (the GEP offsets are wrong, no crash, no diagnostic). So the .bc
# is generated locally, on the build machine, with that machine's native
# data layout — correct by construction. It is NOT committed (.gitignore'd);
# `make` regenerates it whenever its inputs change.
#
# WHY clang 18, GATED. The in-process parser is libLLVM 18 (the version
# kaic2 statically links). LLVM's bitcode rule is writer <= reader: a .bc
# from a NEWER clang (Apple clang 21, Homebrew clang 22) fails to parse with
# `Unknown attribute kind` / `expected type`. So this script REFUSES to
# generate from any clang that is not major 18 — better no .bc (P2 opts out,
# the build falls back to linking runtime_llvm.c with cc: same behaviour,
# just no inlining) than an unparseable one that breaks the native build.
#
# USAGE
#   tools/gen-runtime-bc.sh                 # (re)generate if stale; no-op if fresh
#   tools/gen-runtime-bc.sh --force         # always regenerate
#   tools/gen-runtime-bc.sh --status        # machine state (see below); exit 0
#   tools/gen-runtime-bc.sh --status-line   # same state as one human line + remedy
#   CLANG18=/path/to/clang-18 tools/gen-runtime-bc.sh
#
# THREE STATES, NOT TWO. "optout" alone hides whether the host CAN run P2:
# a host with clang 18 and no .bc is one `make` away from active, while a
# host without clang 18 needs an install. --status prints
#   active                 the .bc is fresh; the native link inlines the runtime
#   optout needs-regen     clang 18 resolvable, .bc missing or stale
#   optout no-clang18      no clang 18 on this host
# The first word keeps the original active/optout vocabulary, so callers that
# test it are unaffected by the added reason field.
#
# EXIT: 0 on success OR clean opt-out (no clang 18). Non-zero only on a real
# generation error (clang present but the compile failed). The build treats
# a missing .bc as "P2 off", never as a hard failure — except the release /
# CI job, which asserts P2 is active where it MUST be (see the release
# workflow + tools/assert-runtime-bc.sh).

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIME_C="$ROOT/stage0/runtime_llvm.c"
RUNTIME_H="$ROOT/stage2/runtime.h"      # runtime_llvm.c resolves <runtime.h> here (stage2 -I leads)
BC_OUT="$ROOT/stage0/runtime_llvm.bc"
STAMP="$ROOT/stage0/runtime_llvm.bc.stamp"   # input hash the current .bc was built from
# Separate-compilation twin of the bitcode: same source, compiled with
# KAI_SEPARATE_COMPILATION so the runtime's state globals are `external`
# (owned by the runtime TU at link). The native-modular backend merges THIS
# into each partition so O2 inlines the `kaix_*` hot ops without duplicating
# runtime state across the N partition objects.
BC_INLINE="$ROOT/stage0/runtime_inline.bc"

mode="${1:-}"

# The bitcode is a pure function of these two inputs (runtime_llvm.c includes
# only <runtime.h>; runtime.h includes no other project header). The hash is
# the staleness key — and changes whenever the runtime changes, so a stale
# .bc is regenerated rather than silently linking an old runtime.
#
# macOS ships `shasum` (Perl); Debian/Ubuntu ship `sha256sum` (coreutils) and
# not always `shasum`. Resolve whichever exists so the stamp works on both
# the dev host and the CI/Docker Linux runner.
if command -v sha256sum >/dev/null 2>&1; then
  SHA256="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
  SHA256="shasum -a 256"
else
  echo "gen-runtime-bc: neither sha256sum nor shasum found; cannot compute the staleness stamp." >&2
  exit 2
fi
input_hash() {
  $SHA256 "$RUNTIME_C" "$RUNTIME_H" | $SHA256 | awk '{print $1}'
}

resolve_clang() {
  if [ -n "${CLANG18:-}" ]; then echo "$CLANG18"; return 0; fi
  for c in \
    /opt/homebrew/opt/llvm@18/bin/clang \
    /usr/local/opt/llvm@18/bin/clang \
    clang-18 \
    /usr/lib/llvm-18/bin/clang \
    cc
  do
    if command -v "$c" >/dev/null 2>&1; then
      v="$("$c" --version 2>/dev/null | sed -n '1s/.*version \([0-9]*\)\..*/\1/p')"
      if [ "$v" = "18" ]; then echo "$c"; return 0; fi
    fi
  done
  return 1
}

p2_state() {
  if [ -f "$BC_OUT" ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$(input_hash)" ]; then
    echo "active"
  elif [ -n "$(resolve_clang || true)" ]; then
    echo "optout needs-regen"
  else
    echo "optout no-clang18"
  fi
}

# One line a build banner / version field / benchmark header can print
# verbatim: the state, what it costs, and the single command that fixes it.
p2_status_line() {
  case "$(p2_state)" in
    active)
      echo "ACTIVE (the native link inlines the runtime before O2)" ;;
    "optout needs-regen")
      echo "OPTOUT: bitcode not generated, but clang 18 IS installed ($(resolve_clang || true)) — heap-bound native code runs several times slower than CI's until you run: make KAI_LLVM=1 kaic2" ;;
    *)
      echo "OPTOUT: no clang 18 on this host — heap-bound native code runs several times slower than CI's; install one: brew install llvm@18 / apt-get install clang-18" ;;
  esac
}

case "$mode" in
  --status)      p2_state;       exit 0 ;;
  --status-line) p2_status_line; exit 0 ;;
esac

CLANG="$(resolve_clang || true)"
if [ -z "$CLANG" ]; then
  # No clang 18 — clean opt-out. Drop any stale .bc so the native link does
  # not pick up a runtime that no longer matches the sources.
  rm -f "$BC_OUT" "$BC_INLINE" "$STAMP"
  echo "gen-runtime-bc: no clang 18 found; native P2 bitcode disabled (build falls back to cc-links-runtime_llvm.c)." >&2
  echo "gen-runtime-bc:   install one to enable P2 — brew install llvm@18  /  apt-get install clang-18" >&2
  exit 0
fi

want="$(input_hash)"
if [ "$mode" != "--force" ] && [ -f "$BC_OUT" ] && [ -f "$BC_INLINE" ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want" ]; then
  exit 0    # fresh — nothing to do
fi

# HOT/OWNER SPLIT. Both bitcodes are compiled -DKAI_HOT_ONLY: clang -O2
# hoists the thread pointer across swapcontext, which is unsound for a fiber
# work-stolen onto another OS thread (see runtime_llvm.c's header note). Under
# KAI_HOT_ONLY this TU exposes ONLY the leaf value/RC/arithmetic ops that never
# reach swapcontext — safe to inline. The scheduler + everything that suspends
# a fiber comes from the cc-compiled runtime OWNER object (gcc keeps the thread
# pointer honest), linked by bin/kai on both native paths. Because main + the
# scheduler are now owner-only, the hot bitcode carries no internal runtime
# state either, so both twins compile with KAI_SEPARATE_COMPILATION (state
# `external`, owner-defined).
#
# Same -I order as the cc link (stage2 ahead of stage0) so <runtime.h> binds
# to the Koka runtime, and the same -O2 the C path gets, so the runtime's own
# static helpers are pre-optimised. The target data layout is the build
# host's native one (clang's default) — exactly what the in-process module
# carries on this platform, so the link is layout-correct by construction.
"$CLANG" -std=c99 -Wno-unused-function -Wno-unused-variable -O2 -emit-llvm -c \
  -DKAI_HOT_ONLY=1 -DKAI_SEPARATE_COMPILATION=1 \
  -I "$ROOT/stage2" -I "$ROOT/stage0" \
  "$RUNTIME_C" -o "$BC_OUT"

# The native-modular twin. Identical flags today (the split made the two
# bitcodes content-identical), kept as a distinct artifact so the two native
# link paths can evolve independently and their staleness keys stay separate.
"$CLANG" -std=c99 -Wno-unused-function -Wno-unused-variable -O2 -emit-llvm -c \
  -DKAI_HOT_ONLY=1 -DKAI_SEPARATE_COMPILATION=1 \
  -I "$ROOT/stage2" -I "$ROOT/stage0" \
  "$RUNTIME_C" -o "$BC_INLINE"

# Soundness gate for the split: the hot bitcode must contain NO function that
# reaches swapcontext. If clang compiled a suspend-point op into it, that op
# would be miscompiled under work-stealing. A defined-or-referenced swapcontext
# in the bitcode means a KAI_HOT_ONLY gate is missing — fail the build loudly
# rather than ship an unsound runtime.
if command -v llvm-nm >/dev/null 2>&1; then LLVM_NM=llvm-nm
elif command -v llvm-nm-18 >/dev/null 2>&1; then LLVM_NM=llvm-nm-18
else LLVM_NM=""; fi
if [ -n "$LLVM_NM" ]; then
  for bc in "$BC_OUT" "$BC_INLINE"; do
    if "$LLVM_NM" "$bc" 2>/dev/null | grep -q swapcontext; then
      echo "gen-runtime-bc: FATAL — $bc references swapcontext; a KAI_HOT_ONLY gate is missing in runtime_llvm.c." >&2
      rm -f "$BC_OUT" "$BC_INLINE" "$STAMP"
      exit 3
    fi
  done
fi

echo "$want" > "$STAMP"
echo "gen-runtime-bc: generated $BC_OUT + $BC_INLINE ($("$CLANG" --version | sed -n 1p))" >&2
