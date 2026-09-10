#!/bin/bash
# The home-module partition must cost O(decls), not O(decls²).
#
# `partition_decls_by_home` buckets a flat decl stream by home module. An
# append that copies the bucket spine per decl is quadratic in
# decls-per-module — invisible on the compiler's own sources, where the
# largest module holds ~1.2k decls, and dominant on the single large file a
# code generator emits.
#
# The gate compiles one generated module at N and at 2N decls and reads the
# compiler's own RC ledger. Under a linear partition the per-decl cost is
# flat, so doubling the input doubles the allocations attributable to the
# decls; under a quadratic one it quadruples. Comparing the two runs' RATIO
# rather than an absolute count keeps the gate stable across machines and
# across unrelated compiler growth — only the growth EXPONENT is pinned.

set -u

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"
WORK="$ROOT/stage2/build/partition-linearity"
N="${KAI_PARTITION_N:-1600}"
# A quadratic partition doubles the per-decl cost from N to 2N; a linear one
# holds it flat. The window admits the sublinear drift a shared per-compile
# structure contributes without admitting the quadratic.
MAX_RATIO="${KAI_PARTITION_MAX_RATIO:-215}"

[ -x "$KAIC2" ] || { echo "partition-linearity: kaic2 not built" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"

gen() {
  local n="$1" out="$2" i=1
  : > "$out"
  while [ "$i" -le "$n" ]; do
    printf 'fn f%d(x: Int) : Int = {\n  let a = x + %d\n  let b = a * 2\n  if b > 10 { b - 1 } else { b + 1 }\n}\n' "$i" "$i" >> "$out"
    i=$((i + 1))
  done
  printf 'fn main() : Unit / Stdout = {\n  Stdout.print("r=#{f1(1)}")\n}\n' >> "$out"
}

# `alloc_total` for one --check run; the front-end is where the partition runs.
allocs() {
  local src="$1"
  KAI_TRACE_RC=1 "$KAIC2" --check --path "$ROOT/stdlib" "$src" 2>&1 >/dev/null \
    | sed -n 's/.*alloc_total=\([0-9]*\).*/\1/p' | head -1
}

gen 0        "$WORK/base.kai"
gen "$N"     "$WORK/n.kai"
gen $((N*2)) "$WORK/n2.kai"

a0="$(allocs "$WORK/base.kai")"
a1="$(allocs "$WORK/n.kai")"
a2="$(allocs "$WORK/n2.kai")"

for v in "$a0" "$a1" "$a2"; do
  case "$v" in ''|*[!0-9]*) echo "partition-linearity: no ledger from kaic2" >&2; exit 1 ;; esac
done

d1=$((a1 - a0))
d2=$((a2 - a0))
[ "$d1" -gt 0 ] || { echo "partition-linearity: no decl-attributable allocations at n=$N" >&2; exit 1; }

ratio=$(( d2 * 100 / d1 ))

if [ "$ratio" -gt "$MAX_RATIO" ]; then
  echo "partition-linearity: FAIL — allocations grow ${ratio}% from n=$N to n=$((N*2)) (max ${MAX_RATIO}%)"
  echo "  n=0     alloc_total=$a0"
  echo "  n=$N    alloc_total=$a1  (decl-attributable $d1)"
  echo "  n=$((N*2)) alloc_total=$a2  (decl-attributable $d2)"
  echo "  a per-decl append that copies its bucket spine is the shape this catches"
  exit 1
fi

echo "partition-linearity: OK (${ratio}% growth for 2x the decls, max ${MAX_RATIO}%)"
