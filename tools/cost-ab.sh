#!/bin/bash
# Compile-cost A/B of two kaic2 binaries on identical inputs.
#
# Both binaries compile the BASE tree's stage2/main.kai (self-compile) and an
# empty program, plus any program given. A binary that compiled its own tree
# would measure the branch's source growth along with its compiler cost. Each
# side reads its OWN tree's stdlib (kaic2 bakes its stdlib path at build time,
# so --path is always explicit): a stdlib or core change is part of what a
# user's compile costs.
#
# Usage:
#   tools/cost-ab.sh [options] <base-kaic2> <branch-kaic2> [program.kai ...]
#   tools/cost-ab.sh [options] --base <git-ref> [program.kai ...]
#
# Options:
#   --base <ref>      Build the base compiler from <ref> and the branch compiler
#                     from this tree's HEAD, each in a cached worktree with the
#                     same recipe (seeded by this tree's stage2/kaic2, two
#                     kaic2-fast hops), so the binaries differ only by source.
#                     Uncommitted changes are not measured.
#   --base-tree <dir> Tree whose stage2/main.kai is compiled and whose stdlib
#                     the base reads (default: the tree holding <base-kaic2>,
#                     else this one).
#   --branch-tree <dir>
#                     Tree whose stdlib the branch reads (default: the tree
#                     holding <branch-kaic2>, else this one).
#   --rounds <n>      Alternating measured rounds after one warm-up (default 3).
#   --markdown        Print the table as GitHub markdown.
#
# Unit: millions of instructions retired (macOS, Linux with perf), else CPU
# milliseconds.
#
# Verdict: "up" when every branch round is above every base round, "down" when
# every branch round is below every base round, else "noise"; a separation under 0.1% is
# noise too (run-to-run jitter of a self-compile on one binary is ~0.03%). Run on a quiet
# machine (load < 3); instructions retired are stable under load but not immune.
set -eu

HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROUNDS=3
BASE_REF=""
BASE_TREE=""
BRANCH_TREE=""
MARKDOWN=0
FLOOR_PCT=0.1
pos=()
while [ $# -gt 0 ]; do
  case "$1" in
    --base) BASE_REF="${2:?--base needs a ref}"; shift 2 ;;
    --base-tree) BASE_TREE="${2:?--base-tree needs a dir}"; shift 2 ;;
    --branch-tree) BRANCH_TREE="${2:?--branch-tree needs a dir}"; shift 2 ;;
    --rounds) ROUNDS="${2:?--rounds needs a number}"; shift 2 ;;
    --markdown) MARKDOWN=1; shift ;;
    -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
    *) pos+=("$1"); shift ;;
  esac
done

die() { echo "cost-ab: $*" >&2; exit 2; }

# A compiler built from a ref: two fast hops, so its source is compiled by a
# compiler of its own code. Base and branch go through the same recipe.
build_ref() {
  local ref="$1" sha wt
  sha="$(git -C "$HERE" rev-parse --verify "$ref^{commit}")" || die "unknown ref $ref"
  wt="${COST_AB_CACHE:-${TMPDIR:-/tmp}/cost-ab}/tree-${sha:0:12}"
  if [ ! -x "$wt/stage2/kaic2" ] || [ ! -f "$wt/.cost-ab-built" ]; then
    rm -rf "$wt"
    mkdir -p "$(dirname "$wt")"
    git -C "$HERE" worktree prune
    git -C "$HERE" worktree add -q --detach "$wt" "$sha" >&2
    [ -x "$HERE/stage2/kaic2" ] || die "no $HERE/stage2/kaic2 to seed the base build; run make kaic2"
    cp "$HERE/stage2/kaic2" "$wt/stage2/kaic2"
    echo "cost-ab: building $ref (${sha:0:12}) with two kaic2-fast hops" >&2
    (cd "$wt" && make bin/kai >&2 && make kaic2-fast >&2 && make kaic2-fast >&2) || die "build of $ref failed"
    touch "$wt/.cost-ab-built"
  fi
  echo "$wt"
}

if [ -n "$BASE_REF" ]; then
  [ -z "$(git -C "$HERE" status --porcelain --untracked-files=no)" ] || echo "cost-ab: warning: uncommitted changes are not measured (the branch is built from HEAD)" >&2
  BASE_TREE="$(build_ref "$BASE_REF")"
  BRANCH_TREE="$(build_ref HEAD)"
  BASE_BIN="$BASE_TREE/stage2/kaic2"
  BRANCH_BIN="$BRANCH_TREE/stage2/kaic2"
  progs=("${pos[@]+"${pos[@]}"}")
else
  [ ${#pos[@]} -ge 2 ] || die "usage: $0 [options] <base-kaic2> <branch-kaic2> [program.kai ...] | --base <ref>"
  BASE_BIN="${pos[0]}"; BRANCH_BIN="${pos[1]}"
  progs=("${pos[@]:2}")
  [ -n "$BASE_TREE" ] || BASE_TREE="$(cd "$(dirname "$BASE_BIN")/.." 2>/dev/null && pwd)"
  [ -f "$BASE_TREE/stage2/main.kai" ] || BASE_TREE="$HERE"
  [ -n "$BRANCH_TREE" ] || BRANCH_TREE="$(cd "$(dirname "$BRANCH_BIN")/.." 2>/dev/null && pwd)"
  [ -d "$BRANCH_TREE/stdlib" ] || BRANCH_TREE="$HERE"
fi
abs() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
[ -x "$BASE_BIN" ] && BASE_BIN="$(abs "$BASE_BIN")"
[ -x "$BRANCH_BIN" ] && BRANCH_BIN="$(abs "$BRANCH_BIN")"
[ -x "$BASE_BIN" ] || die "not executable: $BASE_BIN"
[ -x "$BRANCH_BIN" ] || die "not executable: $BRANCH_BIN"
[ -f "$BASE_TREE/stage2/main.kai" ] || die "no stage2/main.kai in $BASE_TREE"
EDITION="$(cat "$BASE_TREE/EDITION")"
BASE_STDLIB="$BASE_TREE/stdlib"
BRANCH_STDLIB="$BRANCH_TREE/stdlib"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cost-ab.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
: > "$WORK/empty.kai"

# Instructions retired where the host exposes them, else CPU milliseconds
# (virtualised Linux runners have no hardware counters).
UNIT=M
case "$(uname -s)" in
  Darwin) DIV=1000000
    measure_cmd() { /usr/bin/time -l "$@" 2>&1 >/dev/null | awk '/instructions retired/ {print $1}'; } ;;
  Linux)
    perf_count() { perf stat -x, -e instructions:u "$@" 2>&1 >/dev/null | awk -F, '/instructions/ && $1 ~ /^[0-9]+$/ {print $1}'; }
    if command -v perf >/dev/null && [ -n "$(perf_count true)" ]; then
      DIV=1000000; measure_cmd() { perf_count "$@"; }
    else
      UNIT=ms; DIV=1
      measure_cmd() { /usr/bin/time -f '%U %S' "$@" 2>&1 >/dev/null | awk 'END {printf "%d", ($1 + $2) * 1000}'; }
    fi ;;
  *) die "unsupported platform" ;;
esac

# $1=binary $2=probe (selfhost or a .kai path) $3=stdlib -> cost in $UNIT
run_probe() {
  local bin="$1" file="$2" stdlib="$3" dir n
  if [ "$file" = selfhost ]; then dir="$BASE_TREE/stage2"; file=main.kai
  else dir="$(dirname "$file")"; file="$(basename "$file")"; fi
  n="$(cd "$dir" && KAI_THREADS=1 measure_cmd "$bin" --edition "$EDITION" --path . --path "$stdlib" "$file")"
  [ -n "$n" ] || die "no measurement from $bin on $file"
  echo $((n / DIV))
}

probes=(selfhost "$WORK/empty.kai")
for p in "${progs[@]+"${progs[@]}"}"; do probes+=("$(cd "$(dirname "$p")" && pwd)/$(basename "$p")"); done

label() { case "$1" in selfhost) echo "self-compile";; "$WORK/empty.kai") echo "empty";; *) basename "$1";; esac; }
stats() { # min mean of args
  printf '%s\n' "$@" | awk 'NR==1{m=$1} $1<m{m=$1} {s+=$1} END{printf "%d %d", m, s/NR}'
}

echo "cost-ab: base=$BASE_BIN branch=$BRANCH_BIN input=$BASE_TREE stdlib base=$BASE_STDLIB branch=$BRANCH_STDLIB rounds=$ROUNDS load=$(uptime | sed 's/.*averages*: //')" >&2

if [ "$MARKDOWN" = 1 ]; then
  echo "| probe | base min / mean ($UNIT) | branch min / mean ($UNIT) | delta ($UNIT) | delta (%) | verdict |"
  echo "|---|---:|---:|---:|---:|---|"
else
  printf '%-28s %22s %22s %12s %8s  %s\n' probe "base min/mean ($UNIT)" "branch min/mean ($UNIT)" "delta ($UNIT)" "delta %" verdict
fi

for p in "${probes[@]}"; do
  run_probe "$BASE_BIN" "$p" "$BASE_STDLIB" >/dev/null
  run_probe "$BRANCH_BIN" "$p" "$BRANCH_STDLIB" >/dev/null
  b=(); r=()
  for i in $(seq 1 "$ROUNDS"); do
    if [ $((i % 2)) -eq 1 ]; then
      b+=("$(run_probe "$BASE_BIN" "$p" "$BASE_STDLIB")"); r+=("$(run_probe "$BRANCH_BIN" "$p" "$BRANCH_STDLIB")")
    else
      r+=("$(run_probe "$BRANCH_BIN" "$p" "$BRANCH_STDLIB")"); b+=("$(run_probe "$BASE_BIN" "$p" "$BASE_STDLIB")")
    fi
  done
  read -r bmin bmean <<<"$(stats "${b[@]}")"
  read -r rmin rmean <<<"$(stats "${r[@]}")"
  bmax="$(printf '%s\n' "${b[@]}" | sort -n | tail -1)"
  rmax="$(printf '%s\n' "${r[@]}" | sort -n | tail -1)"
  if [ "$rmin" -gt "$bmax" ]; then verdict=up
  elif [ "$rmax" -lt "$bmin" ]; then verdict=down
  else verdict=noise; fi
  delta=$((rmean - bmean))
  pct="$(awk -v d="$delta" -v b="$bmean" 'BEGIN{printf "%+.2f", b ? 100*d/b : 0}')"
  [ "$verdict" != noise ] && awk -v p="$pct" -v f="$FLOOR_PCT" 'BEGIN{exit !((p < 0 ? -p : p) < f)}' && verdict=noise
  band="base ${bmin}..${bmax}, branch ${rmin}..${rmax}"
  if [ "$MARKDOWN" = 1 ]; then
    echo "| $(label "$p") | $bmin / $bmean | $rmin / $rmean | $(printf '%+d' "$delta") | $pct | $verdict ($band) |"
  else
    printf '%-28s %22s %22s %12s %8s  %s (%s)\n' "$(label "$p")" "$bmin / $bmean" "$rmin / $rmean" "$(printf '%+d' "$delta")" "$pct" "$verdict" "$band"
  fi
done
