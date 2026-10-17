#!/bin/sh
# A c-modular compile that prunes the core against a cached core TU set must
# agree with an unpruned compile that uses no cache: every core TU and its
# header slices byte for byte, and the root TU and its slices as a subset
# (pruning drops the instances and lambdas nothing reaches, never adds one).
#
#   tools/cmodular-prune-diff.sh <kaic2> <stdlib-dir> <prime.kai> <program.kai>...
#
# <prime.kai> compiles whole and fills the cache; every <program.kai> then
# compiles with the core pruned and is compared file by file with a no-cache
# compile of the same program.
set -u
KAIC2=$1; STDLIB=$2; PRIME=$3; shift 3
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cmodprune.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
CACHE="$WORK/cache"; mkdir -p "$CACHE"
TC=cmodprune-test

# split_stream <stream> <dir>: one file per //KAIFILE: marker, nonce stripped.
split_stream() {
  mkdir -p "$2"
  awk -v dir="$2" '
    NR == 1 { next }
    index($0, "//KAIFILE:") == 1 {
      name = $0; sub(/^\/\/KAIFILE:[^:]*::/, "", name)
      out = dir "/" name; printf "" > out; next
    }
    out != "" { print > out }
  ' "$1"
}

# norm_root <file>: its non-blank lines, sorted, without the impl-table
# registration count, with the table's closing brace folded into its last
# entry (the only lines a shorter table rewrites) and with the numbers
# minted into effect-instance names, which follow the declaration count.
norm_root() {
  grep -v '^[[:space:]]*$' "$1" | grep -v 'kai_register_impls(' | sed -e 's/},};$/},/' -e 's/= {};$/= {/' -e 's/\[#[0-9][0-9]*\]/[#N]/g' | sort
}

# subset <got> <ref>: every non-blank line of <got> occurs in <ref> at least as often.
subset() {
  norm_root "$1" > "$WORK/s.got"
  norm_root "$2" > "$WORK/s.ref"
  [ -z "$(comm -23 "$WORK/s.got" "$WORK/s.ref")" ]
}

# trim <file>: the file without the blank lines that end it (the stream
# separates files by one, and the last file's depends on the TU order).
trim() {
  awk '{ l[NR] = $0 } END { n = NR; while (n > 0 && l[n] ~ /^[ \t]*$/) n--; for (i = 1; i <= n; i++) print l[i] }' "$1"
}

same() {
  trim "$1" > "$WORK/e.got"; trim "$2" > "$WORK/e.ref"
  cmp -s "$WORK/e.got" "$WORK/e.ref"
}

"$KAIC2" --emit=c-modular --path "$STDLIB" --core-cache-dir "$CACHE" --toolchain-id $TC "$PRIME" \
  > "$WORK/prime.out" 2> "$WORK/prime.err" \
  || { echo "cmodular-prune FAIL (prime compile)"; cat "$WORK/prime.err"; exit 1; }

# The core TUs are the ones an empty program emits besides the root; every
# other TU (imported library modules, the program's own) may lose what the
# pruning drops, like the root.
printf 'fn main() {\n}\n' > "$WORK/empty.kai"
"$KAIC2" --emit=c-modular --path "$STDLIB" "$WORK/empty.kai" > "$WORK/empty.out" 2> /dev/null
split_stream "$WORK/empty.out" "$WORK/empty"
CORE=" $(ls "$WORK/empty" | grep -v '__root__' | tr '\n' ' ') "

fail=0; n=0; pruned=0
for p in "$@"; do
  n=$((n + 1))
  "$KAIC2" --emit=c-modular --path "$STDLIB" "$p" > "$WORK/ref.out" 2> /dev/null \
    || { echo "cmodular-prune SKIP $p (reference compile fails)"; continue; }
  "$KAIC2" --emit=c-modular --path "$STDLIB" --core-cache-dir "$CACHE" --toolchain-id $TC --core-cache-stats "$p" \
    > "$WORK/got.out" 2> "$WORK/got.err"
  grep -q 'core-prune: dropped' "$WORK/got.err" && pruned=$((pruned + 1))
  rm -rf "$WORK/ref" "$WORK/got"
  split_stream "$WORK/ref.out" "$WORK/ref"
  split_stream "$WORK/got.out" "$WORK/got"
  bad=""
  for f in $(ls "$WORK/ref"); do
    case "$CORE" in
      *" $f "*) [ -f "$WORK/got/$f" ] || { bad="$bad missing:$f"; continue; } ;;
      *) [ -f "$WORK/got/$f" ] || continue ;;
    esac
    case "$CORE" in
      *" $f "*) same "$WORK/got/$f" "$WORK/ref/$f" || bad="$bad differs:$f" ;;
      *) case "$f" in
           *.h) ;;
           *) subset "$WORK/got/$f" "$WORK/ref/$f" || bad="$bad grew:$f" ;;
         esac ;;
    esac
  done
  for f in $(ls "$WORK/got"); do [ -f "$WORK/ref/$f" ] || bad="$bad extra:$f"; done
  if [ -n "$bad" ]; then echo "cmodular-prune DIFF $p:$bad"; fail=1; fi
done
variants=$(cat "$CACHE"/core-emit-pre-* 2>/dev/null | grep -c .)
if cat "$CACHE"/core-emit-pre-* 2>/dev/null | grep -q ' conflict$'; then
  echo "cmodular-prune FAIL: a core TU variant conflicted, so pruning was switched off for it"; fail=1
fi
[ "$pruned" -gt 0 ] || { echo "cmodular-prune FAIL: no program was compiled with a pruned core"; exit 1; }
[ "$fail" -eq 0 ] && echo "cmodular-prune OK ($n programs, $pruned pruned, $variants variants)"
exit $fail
