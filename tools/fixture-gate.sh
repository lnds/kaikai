#!/bin/sh
# tools/fixture-gate.sh — run one declared backend gate: fixtures built on
# the native backend or under ASAN+UBSan, run, and diffed against their goldens.
#
#   fixture-gate.sh <target>      run the row of stage2/gate-variants.txt for <target>
#   fixture-gate.sh --list        print every declared target
#
# Run from stage2/. Row: `<target> <native|asan> <fixture[,fixture...]> [option ...]`
# (fixture paths are relative to the repo root; a golden is the fixture's
# `.out.expected` sibling, `main.out.expected` for a package `main.kai`). Options:
#
#   env=K=V          set K for the run (repeatable)
#   merge            capture stderr into the compared output
#   tolerate         accept a non-zero exit of the run
#   timeout=N        kill the run after N seconds
#   llvm             skip unless the compiler was built with the native backend (KAI_LLVM=1)
#   strict           asan only: -fno-sanitize-recover=all on the make CFLAGS
#   nocellpool       asan only: build with -DKAI_NO_CELL_POOL
#   bound=K<N        (implies KAI_TRACE_RC=1) the `K=` counter of the RC report is below N
#   bound=K>N        ... above N
#   bound=free-eq-alloc    every allocation was freed
#   bound=free-near-alloc  frees are within 10 of the allocations
#
# Environment (set by stage2/Makefile): KAIC2 CC CFLAGS LDFLAGS_STACK LDLIBS_M TIMEOUT KAI_LLVM.

set -eu

ROW_FILE="${GATE_VARIANTS:-gate-variants.txt}"

if [ "${1:-}" = --list ]; then
  awk '$1 !~ /^#/ && NF { print $1 }' "$ROW_FILE"
  exit 0
fi

target=${1:?usage: fixture-gate.sh <target> | --list}
row=$(awk -v t="$target" '$1 == t' "$ROW_FILE")
[ -n "$row" ] || { echo "fixture-gate: no row for $target in $ROW_FILE" >&2; exit 2; }
# shellcheck disable=SC2086
set -- $row
shift
backend=$1; fixtures=$2; shift 2
label=${target#test-}

fail() { echo "$label FAIL ($1)"; shift; for f in "$@"; do cat "$f"; done; exit 1; }

skip_c_only=0; env_list=""; merge=0; tolerate=0; tmo=""; strict=0; cellpool=1; bounds=""
for o in "$@"; do
  case $o in
    env=*) env_list="$env_list ${o#env=}" ;;
    merge) merge=1 ;;
    tolerate) tolerate=1 ;;
    timeout=*) tmo=${o#timeout=} ;;
    llvm) skip_c_only=1 ;;
    strict) strict=1 ;;
    nocellpool) cellpool=0 ;;
    bound=*) bounds="$bounds ${o#bound=}" ;;
    *) echo "fixture-gate: $target: unknown option $o" >&2; exit 2 ;;
  esac
done

if [ $skip_c_only = 1 ] && [ "${KAI_LLVM:-}" != 1 ]; then
  echo "$label SKIP (C-only build; native backend not compiled)"
  exit 0
fi

case $backend in
  native) ;;
  asan)
    env_list="$env_list ASAN_OPTIONS=abort_on_error=0:halt_on_error=1:detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1" ;;
  *) echo "fixture-gate: $target: backend must be native or asan" >&2; exit 2 ;;
esac
[ -z "$bounds" ] || env_list="$env_list KAI_TRACE_RC=1"

counter() { grep -oE "$1=[0-9]+" "$b.rc" | head -1 | cut -d= -f2; }

check_bounds() {
  for bd in $bounds; do
    case $bd in
      free-eq-alloc|free-near-alloc)
        alloc=$(counter alloc_total); free=$(counter free_total)
        [ -n "$alloc" ] && [ -n "$free" ] || fail "$fixture: no alloc_total/free_total line" "$b.rc"
        if [ "$bd" = free-eq-alloc ]; then [ "$free" -eq "$alloc" ]; else [ "$free" -ge $((alloc - 10)) ]; fi \
          || fail "$fixture: $bd: alloc_total=$alloc free_total=$free"
        ;;
      *"<"*|*">"*)
        k=${bd%%[<>]*}; rest=${bd#"$k"}; op=${rest%"${rest#?}"}; lim=${rest#?}
        v=$(counter "$k")
        [ -n "$v" ] || fail "$fixture: no $k= line" "$b.rc"
        if [ "$op" = "<" ]; then [ "$v" -lt "$lim" ]; else [ "$v" -gt "$lim" ]; fi \
          || fail "$fixture: $k=$v violates $k$op$lim"
        ;;
    esac
  done
}

n=0
for fixture in $(echo "$fixtures" | tr , ' '); do
  n=$((n + 1))
  b=build/gate-$label-$n
  src=../$fixture
  gold=../${fixture%.kai}.out.expected
  case $fixture in */main.kai) gold=../${fixture%main.kai}main.out.expected ;; esac

  if [ "$backend" = native ]; then
    KAI_BACKEND=native ../bin/kai build "$src" -o "$b" 2> "$b.err" || fail "$fixture: native build errored" "$b.err"
  else
    $KAIC2 --path ../stdlib "$src" > "$b.c" || fail "$fixture: kaic2 errored"
    if [ $strict = 1 ]; then
      # shellcheck disable=SC2086
      $CC $CFLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -Wno-absolute-value \
        -I ../stage2 -I ../stage0 "$b.c" -o "$b" $LDLIBS_M || fail "$fixture: C compile failed"
    else
      nocp=""; [ $cellpool = 1 ] || nocp="-DKAI_NO_CELL_POOL"
      # shellcheck disable=SC2086
      $CC -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $nocp \
        -Wno-unused-function -Wno-unused-variable $LDFLAGS_STACK \
        -I ../stage2 -I ../stage0 "$b.c" -o "$b" || fail "$fixture: C compile failed"
    fi
  fi

  run_prefix=""
  [ -z "$tmo" ] || run_prefix="${TIMEOUT:-timeout} $tmo"
  rc=0
  if [ $merge = 1 ]; then
    # shellcheck disable=SC2086
    env $env_list $run_prefix "./$b" > "$b.out" 2>&1 || rc=$?
    cp "$b.out" "$b.rc"
  else
    # shellcheck disable=SC2086
    env $env_list $run_prefix "./$b" > "$b.out" 2> "$b.rc" || rc=$?
  fi
  if [ $rc -ne 0 ] && [ $tolerate = 0 ]; then
    fail "$fixture: run exited $rc" "$b.rc"
  fi

  if [ "$backend" = asan ] && grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$b.rc"; then
    fail "$fixture: sanitizer diagnostic" "$b.rc"
  fi

  diff -q "$gold" "$b.out" > /dev/null || { echo "$label DIFF ($fixture)"; diff "$gold" "$b.out"; exit 1; }
  check_bounds
done
echo "$label OK ($n fixtures)"
