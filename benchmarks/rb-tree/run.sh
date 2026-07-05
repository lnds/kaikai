#!/usr/bin/env bash
# rb-tree benchmark harness — kaikai vs Koka vs hand-written C.
#
# Columns, ONE algorithm (Okasaki/Lean4 red-black tree) and ONE
# driver (Numerical-Recipes LCG keystream, modulus 2^31-1, N inserts).
# The sources are byte-for-byte the same algorithm; only the language
# (and, for kaikai, the backend) differs. See README.md for the method
# and the last recorded result.
#
# kaikai ships two backends from the same front-end:
#   kaikai-c      kaic2 -> C            -> $CC -O2 (default backend)
#   kaikai-native in-process libLLVM    -> native object (--backend=native)
# Both rows come from the identical .kai source; the only difference is
# the code-generation path, so the gap between them is a pure backend
# measurement. (The native column needs a kaic2 built with libLLVM:
# `make -C stage2 KAI_LLVM=1`.)
#
# Usage:
#   ./run.sh            # wall-clock (median of 5) + peak RSS on this host
#   ./run.sh 100000     # override N (default 1,000,000)
#
# Wall-clock on a laptop is noisy (cache + scheduler); for an EXACT,
# host-independent instruction count use callgrind in Docker (see README
# "Instruction count" section) — that is the number the perf lanes gate on.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$REPO_ROOT/benchmarks/rb-tree"
KAIC2="$REPO_ROOT/stage2/kaic2"
KAI_SRC="$REPO_ROOT/examples/perceus/rb_tree_bench.kai"
C_SRC="$REPO_ROOT/examples/perceus/rb_tree_bench_c.c"
KK_SRC="$HERE/rbbench.kk"
N="${1:-1000000}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CC="${CC:-cc}"
RUNS=7

say() { printf '%s\n' "$*" >&2; }

# --- build kaikai (C backend) column ---------------------------------------
if [[ ! -x "$KAIC2" ]]; then
  say "kaic2 not built — run 'make -C stage2 kaic2' first"; exit 1
fi
say "building kaikai-c column (kaic2 -> C -> $CC -O2) ..."
"$KAIC2" --path "$REPO_ROOT/stdlib" --path "$REPO_ROOT/examples/perceus" "$KAI_SRC" > "$WORK/rb_kaikai.c"
# Override N in the emitted C (the .kai hardcodes 1,000,000).
if [[ "$N" != "1000000" ]]; then
  sed -i.bak "s/kair_n = 1000000LL/kair_n = ${N}LL/" "$WORK/rb_kaikai.c"
fi
"$CC" -O2 -I "$REPO_ROOT/stage2" -I "$REPO_ROOT/stage0" "$WORK/rb_kaikai.c" -o "$WORK/rb_kaikai" -lm

# --- build kaikai (native backend) column (optional) -----------------------
# Same .kai, same front-end; only the code-generation path differs. The
# native backend builds the LLVM module in-process via the C API and emits a
# native object (no .ll text, no clang), linked against runtime_llvm.c. Needs
# a kaic2 built with libLLVM (`make -C stage2 KAI_LLVM=1`); skipped otherwise.
# N override: the native path has no text IR to sed, so we run the default N
# (1,000,000) for this column and skip it when a custom N is requested.
HAVE_NATIVE=0
KAI_BIN="$REPO_ROOT/bin/kai"
if [[ "$N" == "1000000" ]] && "$KAI_BIN" build --backend=native "$KAI_SRC" \
     -o "$WORK/rb_kaikai_native" >"$WORK/native.err" 2>&1; then
  say "building kaikai-native column (in-process libLLVM -> native object) ..."
  HAVE_NATIVE=1
elif [[ "$N" != "1000000" ]]; then
  say "skipping kaikai-native column (custom N has no text IR to rewrite)"
else
  say "kaikai-native unavailable — skipping column (build kaic2 with make KAI_LLVM=1)"; head -5 "$WORK/native.err" >&2
fi

# --- build C column --------------------------------------------------------
say "building C column ($CC -O2) ..."
if [[ "$N" != "1000000" ]]; then
  sed "s/int64_t n = 1000000/int64_t n = ${N}/" "$C_SRC" > "$WORK/rb_c.c"
else
  cp "$C_SRC" "$WORK/rb_c.c"
fi
"$CC" -O2 "$WORK/rb_c.c" -o "$WORK/rb_c"

# --- build Koka column (optional) ------------------------------------------
HAVE_KOKA=0
if command -v koka >/dev/null 2>&1; then
  say "building Koka column (koka -O2) ..."
  if [[ "$N" != "1000000" ]]; then
    sed "s/val n = 1000000/val n = ${N}/" "$KK_SRC" > "$WORK/rbbench.kk"
  else
    cp "$KK_SRC" "$WORK/rbbench.kk"
  fi
  ( cd "$WORK" && koka -O2 -o "$WORK/rb_koka" rbbench.kk >/dev/null 2>&1 ) && chmod +x "$WORK/rb_koka" && HAVE_KOKA=1 \
    || say "koka build failed — skipping Koka column"
else
  say "koka not on PATH — skipping Koka column (install from koka-lang.github.io)"
fi

# --- measure: median wall (s) + peak RSS (MB) ------------------------------
# /usr/bin/time -p prints 'real <s>'; -l adds 'maximum resident set size'.
#
# Warm-up + interleaving. Measuring one column's RUNS back-to-back before the
# next makes the FIRST column pay cold-cache/cold-frequency while the rest run
# warm — a systematic bias that makes the wall figures swing run-to-run.
# Instead every binary gets one discarded warm-up run, then the timed runs
# interleave round-robin so cache / scheduler / CPU-frequency noise falls
# evenly on all columns and the medians are stable across re-runs.

# One '/usr/bin/time -p' run of $bin; echoes the 'real' seconds.
time_once() { /usr/bin/time -p "$1" >/dev/null 2>"$WORK/t"; grep '^real' "$WORK/t" | awk '{print $2}'; }

median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }

peak_rss_mb() {
  /usr/bin/time -l "$1" >/dev/null 2>"$WORK/t" || true
  awk '/maximum resident set size/{printf "%.1f", $1/1048576}' "$WORK/t"
}

# Columns present this run, as parallel indexed arrays (bash 3.2 has no
# associative arrays). SAMPLES[i] accumulates column i's timed runs.
PATHS=("$WORK/rb_c" "$WORK/rb_kaikai")
[[ "$HAVE_NATIVE" == 1 ]] && PATHS+=("$WORK/rb_kaikai_native")
[[ "$HAVE_KOKA"   == 1 ]] && PATHS+=("$WORK/rb_koka")
NCOL=${#PATHS[@]}
SAMPLES=()

say "measuring (1 warm-up + median of $RUNS interleaved wall runs + peak RSS) ..."

# Warm-up pass: prime cache/branch-predictor for every column, discard timings.
i=0; while [[ $i -lt $NCOL ]]; do time_once "${PATHS[$i]}" >/dev/null; i=$((i + 1)); done
# Timed passes: interleave columns each round so noise spreads evenly.
r=0; while [[ $r -lt $RUNS ]]; do
  i=0; while [[ $i -lt $NCOL ]]; do
    SAMPLES[$i]="${SAMPLES[$i]:-} $(time_once "${PATHS[$i]}")"
    i=$((i + 1))
  done
  r=$((r + 1))
done

# Columns are appended in a fixed order; map each back by index.
ci=0
C_W=$(median ${SAMPLES[$ci]});          C_R=$(peak_rss_mb "$WORK/rb_c");        ci=$((ci + 1))
KAI_W=$(median ${SAMPLES[$ci]});        KAI_R=$(peak_rss_mb "$WORK/rb_kaikai"); ci=$((ci + 1))
if [[ "$HAVE_NATIVE" == 1 ]]; then
  KNT_W=$(median ${SAMPLES[$ci]}); KNT_R=$(peak_rss_mb "$WORK/rb_kaikai_native"); ci=$((ci + 1))
fi
if [[ "$HAVE_KOKA" == 1 ]]; then
  KK_W=$(median ${SAMPLES[$ci]});  KK_R=$(peak_rss_mb "$WORK/rb_koka");          ci=$((ci + 1))
fi

ratio() { awk -v a="$1" -v b="$2" 'BEGIN{ if (b+0==0) print "—"; else printf "%.2fx", a/b }'; }

echo
echo "rb-tree bench — N=$N inserts — $(uname -sm), $CC -O2"
echo "--------------------------------------------------------------"
printf "%-12s  %10s  %8s  %8s  %10s\n" "column" "wall(med)" "vs C" "vs Koka" "RSS(MB)"
printf "%-12s  %9ss  %8s  %8s  %10s\n" "C"           "$C_W"   "1.00x" "$( [[ $HAVE_KOKA == 1 ]] && ratio "$C_W" "$KK_W" || echo '—')" "$C_R"
if [[ "$HAVE_KOKA" == 1 ]]; then
printf "%-12s  %9ss  %8s  %8s  %10s\n" "Koka"        "$KK_W"  "$(ratio "$KK_W" "$C_W")" "1.00x" "$KK_R"
fi
printf "%-12s  %9ss  %8s  %8s  %10s\n" "kaikai-c"    "$KAI_W" "$(ratio "$KAI_W" "$C_W")" "$( [[ $HAVE_KOKA == 1 ]] && ratio "$KAI_W" "$KK_W" || echo '—')" "$KAI_R"
if [[ "$HAVE_NATIVE" == 1 ]]; then
printf "%-12s  %9ss  %8s  %8s  %10s\n" "kaikai-native" "$KNT_W" "$(ratio "$KNT_W" "$C_W")" "$( [[ $HAVE_KOKA == 1 ]] && ratio "$KNT_W" "$KK_W" || echo '—')" "$KNT_R"
fi
echo
if [[ "$HAVE_NATIVE" == 1 ]]; then
echo "Note: kaikai-c and kaikai-native share the front-end but NOT the code path."
echo "On this bench native retires ~6x more instructions than the C backend (real"
echo "codegen gap, tracked in the perf lanes — see README 'Wall-clock vs"
echo "instruction count'), so its larger wall is expected, not measurement noise."
echo
fi
echo "kaikai @ $(git -C "$REPO_ROOT" rev-parse --short HEAD)"
