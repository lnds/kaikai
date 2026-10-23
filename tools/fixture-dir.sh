#!/bin/sh
# tools/fixture-dir.sh — run every fixture of a directory that carries a
# FIXTURES manifest, so a .kai dropped there runs in CI with nothing to wire.
#
#   fixture-dir.sh <slug>                 run the directory (fixtures in parallel)
#   fixture-dir.sh --list                 print every directory slug that runs on C
#   fixture-dir.sh --list-native          print every directory slug that runs on native
#   fixture-dir.sh --count <slug>         print the fixtures a directory runs
#   fixture-dir.sh --one <slug> <name>    run one fixture
#
# <slug> is the directory under examples/ with `/` written `-`. The stdlib
# path is always passed. A fixture is negative when it has `.err.expected`
# (compile must fail and print every line of it), otherwise it compiles,
# runs, and its stdout is diffed against `.out.expected` when that exists.
# Manifest keys (both required):
#
#   backends: c native        the backends the directory runs on (c, native, or both)
#   diagnostics: fixed|regex  how `.err.expected` lines match stderr
#
# Marks, one `# fixture: <mark>` line anywhere in the fixture:
#
#   # fixture: test        compile with --test; stdout+stderr is the output
#   # fixture: no-stdlib   do not pass the stdlib path (the fixture tests its absence)
#   # fixture: target=<name>  compile for that target (`--target=<name>`)
#   # fixture: threads=<n>    run with KAI_THREADS=<n> (output that depends on one scheduler thread)
#
# FIXTURE_BACKEND picks the backend (c, the default, or native). Native builds
# through the kai wrapper named by FIXTURE_KAI, which owns the stdlib path and
# the link, and supports no marks.
#
# Environment (set by stage2/Makefile): KAIC2 CC CFLAGS CPPFLAGS_CORE
# LDLIBS_M LDFLAGS_STACK TEST_CORE_CACHE FIXTURE_JOBS FIXTURE_BACKEND.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EX="$ROOT/examples"

BACKEND="${FIXTURE_BACKEND:-c}"

slug_dirs() {
  for m in "$EX"/*/FIXTURES "$EX"/*/*/FIXTURES; do
    [ -f "$m" ] || continue
    d="${m%/FIXTURES}"
    sed -n 's/^backends: //p' "$m" | tr ' ' '\n' | grep -qx "$BACKEND" || continue
    rel="${d#"$EX"/}"
    printf '%s %s\n' "$(printf '%s' "$rel" | tr / -)" "$d"
  done
}

dir_of() {
  slug_dirs | awk -v s="$1" '$1 == s { print $2; found = 1 } END { exit !found }'
}

fixtures_of() {
  for f in "$1"/*.kai; do
    [ -f "$f" ] || continue
    basename "$f" .kai
  done
}

manifest() {
  v=$(sed -n "s/^$2: //p" "$1/FIXTURES")
  [ -n "$v" ] || { echo "fixture-dir: $1/FIXTURES lacks '$2:'" >&2; exit 2; }
  printf '%s' "$v"
}

mark() {
  grep -q "^# fixture: $2\$" "$1"
}

run_one() {
  slug=$1
  name=$2
  dir=$(dir_of "$slug") || { echo "fixture-dir: no FIXTURES manifest for '$slug'" >&2; exit 2; }
  f="$dir/$name.kai"
  exp_out="$dir/$name.out.expected"
  exp_err="$dir/$name.err.expected"
  b="build/fx-$slug-$name"
  [ "$BACKEND" = c ] || b="$b-$BACKEND"
  if mark "$f" no-stdlib; then path=; else path="--path ../stdlib"; fi
  tflag=$(sed -n 's/^# fixture: target=\(.*\)$/--target=\1/p' "$f")
  threads=$(sed -n 's/^# fixture: threads=\(.*\)$/\1/p' "$f")
  [ -z "$threads" ] || export KAI_THREADS="$threads"
  manifest "$dir" backends | tr ' ' '\n' | grep -qx "$BACKEND" \
    || { echo "fixture-dir: $dir/FIXTURES: does not declare 'backends: $BACKEND'" >&2; exit 2; }
  case $(manifest "$dir" diagnostics) in
    fixed) grepflag=-qF ;;
    regex) grepflag=-q ;;
    *) echo "fixture-dir: $dir/FIXTURES: diagnostics must be fixed or regex" >&2; exit 2 ;;
  esac

  if [ "$BACKEND" = native ]; then
    if mark "$f" test || mark "$f" no-stdlib; then
      echo "fixture-dir: $f: marks are not supported on native" >&2; exit 2
    fi
    compile() { KAI_BACKEND=native "${FIXTURE_KAI:?native needs FIXTURE_KAI}" build "$f" -o "$b" > /dev/null 2> "$b.err"; }
  else
    compile() { $KAIC2 $TEST_CORE_CACHE $path $tflag "$f" > /dev/null 2> "$b.err"; }
  fi

  if [ -f "$exp_err" ]; then
    rc=0
    compile || rc=$?
    if [ "$rc" -eq 0 ]; then
      echo "$slug FAIL $name (expected error, got success)"; exit 1
    fi
    while IFS= read -r line || [ -n "$line" ]; do
      grep $grepflag -- "$line" "$b.err" \
        || { echo "$slug FAIL $name (missing diagnostic: $line)"; cat "$b.err"; exit 1; }
    done < "$exp_err"
    echo "$slug OK $name (rejected as expected)"
    return 0
  fi

  if [ "$BACKEND" = native ]; then
    compile || { echo "$slug FAIL $name (native build failed)"; cat "$b.err"; exit 1; }
    "./$b" > "$b.out" \
      || { echo "$slug FAIL $name (run errored)"; cat "$b.out"; exit 1; }
  elif mark "$f" test; then
    $KAIC2 $TEST_CORE_CACHE $path --test "$f" > "$b.c" \
      || { echo "$slug FAIL $name (kaic2 errored)"; exit 1; }
    eval "$CC $CFLAGS $CPPFLAGS_CORE -I ../stage0 $b.c -o $b $LDFLAGS_STACK $LDLIBS_M" \
      || { echo "$slug FAIL $name (C compile failed)"; exit 1; }
    "./$b" > "$b.out" 2>&1 \
      || { echo "$slug FAIL $name (test run failed)"; cat "$b.out"; exit 1; }
  else
    $KAIC2 $TEST_CORE_CACHE $path $tflag "$f" > "$b.c" \
      || { echo "$slug FAIL $name (kaic2 errored)"; exit 1; }
    eval "$CC $CFLAGS $CPPFLAGS_CORE -I ../stage0 $b.c -o $b $LDFLAGS_STACK $LDLIBS_M" \
      || { echo "$slug FAIL $name (C compile failed)"; exit 1; }
    "./$b" > "$b.out" \
      || { echo "$slug FAIL $name (run errored)"; cat "$b.out"; exit 1; }
  fi
  if [ -f "$exp_out" ]; then
    diff -q "$exp_out" "$b.out" > /dev/null \
      && echo "$slug OK $name" \
      || { echo "$slug DIFF $name"; diff "$exp_out" "$b.out"; exit 1; }
  else
    echo "$slug OK $name (no golden, ran clean)"
  fi
}

case "${1:-}" in
  --list) slug_dirs | awk '{ print $1 }' ;;
  --list-native) BACKEND=native; slug_dirs | awk '{ print $1 }' ;;
  --count)
    dir=$(dir_of "$2") || exit 2
    fixtures_of "$dir" | wc -l | tr -d ' ' ;;
  --one) run_one "$2" "$3" ;;
  '' ) echo "usage: $0 <slug> | --list | --count <slug> | --one <slug> <name>" >&2; exit 2 ;;
  *)
    slug=$1
    dir=$(dir_of "$slug") || { echo "fixture-dir: no FIXTURES manifest for '$slug'" >&2; exit 2; }
    n=$(fixtures_of "$dir" | wc -l | tr -d ' ')
    [ "$n" -gt 0 ] || { echo "fixture-dir: $slug has no .kai fixtures" >&2; exit 1; }
    fixtures_of "$dir" \
      | xargs -P "${FIXTURE_JOBS:-4}" -I{} sh "$0" --one "$slug" {}
    echo "fixture-dir $slug: $n fixtures" ;;
esac
