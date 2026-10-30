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
#                     from this tree's HEAD, each in a worktree of this run's own
#                     temp dir (removed on exit) with the same recipe: seeded
#                     by the stored build of its merge-base with origin/main
#                     (tools/prebuilt-kaic2.sh), then two kaic2-fast hops, so
#                     the binaries differ only by source. Uncommitted changes
#                     are not measured.
#   --base-tree <dir> Tree whose stage2/main.kai is compiled and whose stdlib
#                     the base reads (default: the tree holding <base-kaic2>,
#                     else this one).
#   --branch-tree <dir>
#                     Tree whose stdlib the branch reads (default: the tree
#                     holding <branch-kaic2>, else this one).
#   --rounds <n>      Alternating measured rounds after one warm-up (default 3).
#   --cache <mode>    cold | warm: compile with the per-project cache on
#                     (--user-cache-dir, shared with --core-cache-dir), one
#                     directory per side and probe. cold empties it before every
#                     run; warm primes it in the warm-up and reuses it.
#   --markdown        Print the table as GitHub markdown.
#   --fail-above <pct>
#                     Exit 1 when self-compile or the empty program is "up" by
#                     more than <pct> percent.
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
CACHE_MODE=""
FLOOR_PCT=0.1
FAIL_ABOVE=""
pos=()
while [ $# -gt 0 ]; do
  case "$1" in
    --base) BASE_REF="${2:?--base needs a ref}"; shift 2 ;;
    --base-tree) BASE_TREE="${2:?--base-tree needs a dir}"; shift 2 ;;
    --branch-tree) BRANCH_TREE="${2:?--branch-tree needs a dir}"; shift 2 ;;
    --rounds) ROUNDS="${2:?--rounds needs a number}"; shift 2 ;;
    --cache) CACHE_MODE="${2:?--cache needs cold or warm}"; shift 2 ;;
    --markdown) MARKDOWN=1; shift ;;
    --fail-above) FAIL_ABOVE="${2:?--fail-above needs a percentage}"; shift 2 ;;
    -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
    *) pos+=("$1"); shift ;;
  esac
done

case "$CACHE_MODE" in ""|cold|warm) ;; *) echo "cost-ab: --cache takes cold or warm" >&2; exit 2 ;; esac

die() { echo "cost-ab: $*" >&2; exit 2; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cost-ab.XXXXXX")"
echo $$ > "$WORK/pid"

# A run dir holds its own worktrees, registered in $HERE's repository.
drop_run() {
  local t
  for t in "$1"/tree-*; do
    [ -d "$t" ] && git -C "$HERE" worktree remove --force "$t" >/dev/null 2>&1
  done
  rm -rf "$1"
  git -C "$HERE" worktree prune
}
trap 'drop_run "$WORK"' EXIT
trap 'exit 130' INT TERM

# Run dirs of killed runs: their pid is gone. A live run's dir is never touched.
for d in "${TMPDIR:-/tmp}"/cost-ab.*/; do
  [ -f "$d/pid" ] && [ "${d%/}" != "$WORK" ] || continue
  kill -0 "$(cat "$d/pid")" 2>/dev/null || drop_run "${d%/}"
done

# A compiler built from a ref: two fast hops, so its source is compiled by a
# compiler of its own code. Base and branch go through the same recipe, each
# seeded by the stored build of the ref's merge-base with origin/main: a seed
# from any other commit may not compile this tree or run under its wrapper.
build_ref() { # role ref
  local role="$1" ref="$2" sha mb seed wt
  sha="$(git -C "$HERE" rev-parse --verify "$ref^{commit}")" || die "unknown ref $ref"
  mb="$(git -C "$HERE" merge-base "$sha" origin/main)" || die "$ref has no merge-base with origin/main"
  seed="$("$HERE/tools/prebuilt-kaic2.sh" path "$mb")" || die "no stored build of ${mb:0:12}"
  wt="$WORK/tree-$role"
  git -C "$HERE" worktree add -q --detach "$wt" "$sha" >&2
  cp "$seed/stage2/kaic2" "$wt/stage2/kaic2"
  echo "cost-ab: building $ref (${sha:0:12}) seeded by ${mb:0:12}, two kaic2-fast hops" >&2
  (cd "$wt" && make bin/kai >&2 && make kaic2-fast >&2 && make kaic2-fast >&2) || die "build of $ref failed"
  echo "$wt"
}

if [ -n "$BASE_REF" ]; then
  [ -z "$(git -C "$HERE" status --porcelain --untracked-files=no)" ] || echo "cost-ab: warning: uncommitted changes are not measured (the branch is built from HEAD)" >&2
  BASE_TREE="$(build_ref base "$BASE_REF")"
  BRANCH_TREE="$(build_ref branch HEAD)"
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

: > "$WORK/empty.kai"

# Instructions retired where the host exposes them, else CPU milliseconds
# (virtualised Linux runners have no hardware counters). Each writes the
# count to $WORK/count and the compiler's stderr to $WORK/err, and returns the
# compiler's exit status: a compile that fails measures nothing.
UNIT=M
case "$(uname -s)" in
  Darwin) DIV=1000000
    measure_cmd() {
      local rc=0
      /usr/bin/time -l "$@" >/dev/null 2>"$WORK/err" || rc=$?
      awk '/instructions retired/ {print $1}' "$WORK/err" > "$WORK/count"
      return $rc
    } ;;
  Linux)
    perf_count() {
      local rc=0
      perf stat -x, -o "$WORK/perf" -e instructions:u "$@" >/dev/null 2>"$WORK/err" || rc=$?
      awk -F, '/instructions/ && $1 ~ /^[0-9]+$/ {print $1}' "$WORK/perf" > "$WORK/count"
      return $rc
    }
    if command -v perf >/dev/null && perf_count true && [ -s "$WORK/count" ]; then
      DIV=1000000; measure_cmd() { perf_count "$@"; }
    else
      UNIT=ms; DIV=1
      measure_cmd() {
        local rc=0
        /usr/bin/time -f '%U %S' -o "$WORK/time" "$@" >/dev/null 2>"$WORK/err" || rc=$?
        awk 'END {printf "%d", ($1 + $2) * 1000}' "$WORK/time" > "$WORK/count"
        return $rc
      }
    fi ;;
  *) die "unsupported platform" ;;
esac

# The self-compile input must be a tree both compilers accept: a change that
# rejects old source (a naming or syntax rule) cannot compile the base tree.
# The base tree is preferred; the head tree stands in when the branch compiler
# rejects it; when both compilers reject both, there is nothing to measure.
accepts() {
  local tree="$1" bin stdlib
  for side in base branch; do
    if [ "$side" = base ]; then bin="$BASE_BIN"; stdlib="$tree/stdlib"; else bin="$BRANCH_BIN"; stdlib="$tree/stdlib"; fi
    (cd "$tree/stage2" && KAI_THREADS=1 KAIKAI_STDLIB_PATH="$stdlib" "$bin" --edition "$(cat "$tree/EDITION")" --path . --path "$stdlib" main.kai >/dev/null 2>"$WORK/accept-$side.err") \
      || { REJECTED_BY="$side"; REJECT_ERR="$WORK/accept-$side.err"; return 1; }
  done
}
INPUT_TREE="$BASE_TREE"; INPUT_WHY="the base tree"
if ! accepts "$BASE_TREE"; then
  echo "cost-ab: the $REJECTED_BY compiler rejects the base tree; first errors:" >&2
  head -12 "$REJECT_ERR" >&2
  BRANCH_HEAD_TREE="${BRANCH_TREE:-$HERE}"
  accepts "$BRANCH_HEAD_TREE" || { echo "cost-ab: the $REJECTED_BY compiler rejects the head tree too; first errors:" >&2; head -12 "$REJECT_ERR" >&2; die "no tree both compilers accept"; }
  INPUT_TREE="$BRANCH_HEAD_TREE"; INPUT_WHY="the head tree (the base tree is rejected)"
fi

# $1=binary $2=probe (selfhost or a .kai path) $3=stdlib $4=side -> cost in $UNIT
run_probe() {
  local bin="$1" file="$2" stdlib="$3" side="$4" dir n cflags=()
  if [ -n "$CACHE_MODE" ]; then
    local cdir="$WORK/cache-$side-$(printf '%s' "$file" | cksum | cut -d' ' -f1)"
    [ "$CACHE_MODE" = cold ] && rm -rf "$cdir"
    mkdir -p "$cdir"
    cflags=(--user-cache --user-cache-dir "$cdir" --core-cache-dir "$cdir")
  fi
  if [ "$file" = selfhost ]; then dir="$INPUT_TREE/stage2"; file=main.kai
  else dir="$(dirname "$file")"; file="$(basename "$file")"; fi
  # A kaic2 reads the core from the stdlib path baked at its build; a binary
  # built elsewhere (a CI artifact) must be pointed at this side's tree.
  (cd "$dir" && KAI_THREADS=1 KAIKAI_STDLIB_PATH="$stdlib" measure_cmd "$bin" --edition "$EDITION" --path . --path "$stdlib" ${cflags[@]+"${cflags[@]}"} "$file") \
    || { head -30 "$WORK/err" >&2; die "$bin failed on $file"; }
  n="$(cat "$WORK/count")"
  [ -n "$n" ] || die "no measurement from $bin on $file"
  echo $((n / DIV))
}

over=0
probes=(selfhost "$WORK/empty.kai")
for p in "${progs[@]+"${progs[@]}"}"; do probes+=("$(cd "$(dirname "$p")" && pwd)/$(basename "$p")"); done

label() { case "$1" in selfhost) echo "self-compile";; "$WORK/empty.kai") echo "empty";; *) basename "$1";; esac; }
stats() { # min mean of args
  printf '%s\n' "$@" | awk 'NR==1{m=$1} $1<m{m=$1} {s+=$1} END{printf "%d %d", m, s/NR}'
}

echo "cost-ab: base=$BASE_BIN branch=$BRANCH_BIN input=$INPUT_TREE ($INPUT_WHY) stdlib base=$BASE_STDLIB branch=$BRANCH_STDLIB rounds=$ROUNDS load=$(uptime | sed 's/.*averages*: //')" >&2

if [ "$MARKDOWN" = 1 ]; then
  echo "| probe | base min / mean ($UNIT) | branch min / mean ($UNIT) | delta ($UNIT) | delta (%) | verdict |"
  echo "|---|---:|---:|---:|---:|---|"
else
  printf '%-28s %22s %22s %12s %8s  %s\n' probe "base min/mean ($UNIT)" "branch min/mean ($UNIT)" "delta ($UNIT)" "delta %" verdict
fi

for p in "${probes[@]}"; do
  run_probe "$BASE_BIN" "$p" "$BASE_STDLIB" base >/dev/null
  run_probe "$BRANCH_BIN" "$p" "$BRANCH_STDLIB" branch >/dev/null
  b=(); r=()
  for i in $(seq 1 "$ROUNDS"); do
    if [ $((i % 2)) -eq 1 ]; then
      b+=("$(run_probe "$BASE_BIN" "$p" "$BASE_STDLIB" base)"); r+=("$(run_probe "$BRANCH_BIN" "$p" "$BRANCH_STDLIB" branch)")
    else
      r+=("$(run_probe "$BRANCH_BIN" "$p" "$BRANCH_STDLIB" branch)"); b+=("$(run_probe "$BASE_BIN" "$p" "$BASE_STDLIB" base)")
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
  if [ -n "$FAIL_ABOVE" ] && [ "$verdict" = up ] && { [ "$p" = selfhost ] || [ "$p" = "$WORK/empty.kai" ]; } \
     && awk -v p="$pct" -v f="$FAIL_ABOVE" 'BEGIN{exit !(p > f)}'; then
    verdict="up, over the ${FAIL_ABOVE}% budget"; over=1
  fi
  if [ "$MARKDOWN" = 1 ]; then
    echo "| $(label "$p") | $bmin / $bmean | $rmin / $rmean | $(printf '%+d' "$delta") | $pct | $verdict ($band) |"
  else
    printf '%-28s %22s %22s %12s %8s  %s (%s)\n' "$(label "$p")" "$bmin / $bmean" "$rmin / $rmean" "$(printf '%+d' "$delta")" "$pct" "$verdict" "$band"
  fi
done
[ "$over" = 0 ]
