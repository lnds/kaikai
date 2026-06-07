#!/usr/bin/env bash
# rb-tree benchmark harness — kaikai vs Koka vs hand-written C.
#
# Columns, ONE algorithm (Okasaki/Lean4 red-black tree) and ONE
# driver (Numerical-Recipes LCG keystream, modulus 2^31-1, N inserts).
# The sources are byte-for-byte the same algorithm; only the language
# (and, for kaikai, the backend) differs. See README.md for the method
# and the last recorded result.
#
# kaikai ships two native backends from the same front-end:
#   kaikai-c    kaic2 -> C   -> $CC -O2   (default backend)
#   kaikai-llvm kaic2 -> .ll -> clang -O2 (--emit=llvm + runtime_llvm.c)
# Both rows come from the identical .kai source; the only difference is
# the code-generation path, so the gap between them is a pure backend
# measurement.
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
CLANG="${CLANG:-clang}"
RUNS=5

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

# --- build kaikai (LLVM backend) column (optional) -------------------------
# Same .kai, same front-end; only --emit=llvm + clang differs. The LLVM IR
# links against stage0/runtime_llvm.c (the kaix_* shim), not the C runtime.
HAVE_LLVM=0
if command -v "$CLANG" >/dev/null 2>&1; then
  say "building kaikai-llvm column (kaic2 --emit=llvm -> $CLANG -O2) ..."
  if "$KAIC2" --path "$REPO_ROOT/stdlib" --path "$REPO_ROOT/examples/perceus" \
       --emit=llvm "$KAI_SRC" > "$WORK/rb_kaikai.ll" 2>"$WORK/llvm.err"; then
    # Override N in the emitted IR (the fill_loop upper bound is the only
    # call-site N; the .kai hardcodes 1,000,000).
    if [[ "$N" != "1000000" ]]; then
      sed -i.bak "s/@kai_fill_loop(\(.*\)i64 1000000)/@kai_fill_loop(\1i64 ${N})/" "$WORK/rb_kaikai.ll"
    fi
    if "$CLANG" -O2 -w -I "$REPO_ROOT/stage2" -I "$REPO_ROOT/stage0" "$WORK/rb_kaikai.ll" \
         "$REPO_ROOT/stage0/runtime_llvm.c" -o "$WORK/rb_kaikai_llvm" -lm 2>>"$WORK/llvm.err"; then
      HAVE_LLVM=1
    else
      say "clang link of LLVM IR failed — skipping kaikai-llvm column"; head -5 "$WORK/llvm.err" >&2
    fi
  else
    say "kaic2 --emit=llvm errored — skipping kaikai-llvm column"; head -5 "$WORK/llvm.err" >&2
  fi
else
  say "$CLANG not on PATH — skipping kaikai-llvm column"
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
median_wall() {
  local bin="$1" t
  local -a xs=()
  for _ in $(seq "$RUNS"); do
    t=$(/usr/bin/time -p "$bin" >/dev/null 2>"$WORK/t"; grep '^real' "$WORK/t" | awk '{print $2}')
    xs+=("$t")
  done
  printf '%s\n' "${xs[@]}" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'
}
peak_rss_mb() {
  local bin="$1"
  /usr/bin/time -l "$bin" >/dev/null 2>"$WORK/t" || true
  awk '/maximum resident set size/{printf "%.1f", $1/1048576}' "$WORK/t"
}

say "measuring (median of $RUNS wall runs + peak RSS) ..."
C_W=$(median_wall "$WORK/rb_c");        C_R=$(peak_rss_mb "$WORK/rb_c")
KAI_W=$(median_wall "$WORK/rb_kaikai"); KAI_R=$(peak_rss_mb "$WORK/rb_kaikai")
if [[ "$HAVE_LLVM" == 1 ]]; then
  KLL_W=$(median_wall "$WORK/rb_kaikai_llvm"); KLL_R=$(peak_rss_mb "$WORK/rb_kaikai_llvm")
fi
if [[ "$HAVE_KOKA" == 1 ]]; then
  KK_W=$(median_wall "$WORK/rb_koka");  KK_R=$(peak_rss_mb "$WORK/rb_koka")
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
if [[ "$HAVE_LLVM" == 1 ]]; then
printf "%-12s  %9ss  %8s  %8s  %10s\n" "kaikai-llvm" "$KLL_W" "$(ratio "$KLL_W" "$C_W")" "$( [[ $HAVE_KOKA == 1 ]] && ratio "$KLL_W" "$KK_W" || echo '—')" "$KLL_R"
fi
echo
echo "kaikai @ $(git -C "$REPO_ROOT" rev-parse --short HEAD)"
