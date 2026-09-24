#!/bin/bash
# A derived BinSerialize encode must cost O(bytes), not O(bytes²).
#
# The list combinator used to re-copy its whole accumulator for every
# element. The output bytes stay correct, so no golden notices; only the
# cost curve does.
#
# The gate encodes a derived record holding a list of N and of 2N records
# and reads the runtime RC ledger. Every byte copied into an output buffer
# is read back with an owning `array_get`, so `incref_total` counts the copy
# traffic. Under a linear encoder doubling the list doubles the
# list-attributable traffic; under a quadratic one it quadruples. The RATIO,
# not an absolute count, is pinned, so the gate is stable across machines
# and unrelated runtime changes. The backend is pinned because the two emit
# different counts for the same shape.

set -u

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
BACKEND="${KAI_BINSER_BACKEND:-c}"
WORK="${TMPDIR:-/tmp}/binserialize-linearity.$$"
N="${KAI_BINSER_N:-1000}"
MAX_RATIO="${KAI_BINSER_MAX_RATIO:-215}"

[ -x "$KAI" ] || { echo "binserialize-linearity: bin/kai not built" >&2; exit 1; }

trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK"

gen() {
  cat > "$2" <<EOF
#[derive(BinSerialize)]
type Item = { id: Int, name: String, tags: Option[[Int]] }

#[derive(BinSerialize)]
type Doc = { title: String, items: [Item] }

fn main() : Unit / Stdout = {
  let items = [1..$1] | (i) => Item { id: i, name: "item", tags: Some([i, -i]) }
  let bs = to_bytes(Doc { title: "doc", items: items })
  print("#{array_length(bs)}")
}
EOF
}

increfs() {
  local name="$1"
  KAI_BACKEND="$BACKEND" "$KAI" build "$WORK/$name.kai" -o "$WORK/$name" >/dev/null 2>&1 \
    || { echo "binserialize-linearity: $name failed to build" >&2; return 1; }
  KAI_TRACE_RC=1 KAI_THREADS=1 "$WORK/$name" 2>&1 >/dev/null \
    | sed -n 's/.*incref_total=\([0-9]*\).*/\1/p' | head -1
}

gen 0        "$WORK/base.kai"
gen "$N"     "$WORK/n.kai"
gen $((N*2)) "$WORK/n2.kai"

i0="$(increfs base)"
i1="$(increfs n)"
i2="$(increfs n2)"

for v in "$i0" "$i1" "$i2"; do
  case "$v" in ''|*[!0-9]*) echo "binserialize-linearity: no RC ledger from the encoder" >&2; exit 1 ;; esac
done

d1=$((i1 - i0))
d2=$((i2 - i0))
[ "$d1" -gt 0 ] || { echo "binserialize-linearity: no list-attributable traffic at n=$N" >&2; exit 1; }

ratio=$(( d2 * 100 / d1 ))

if [ "$ratio" -gt "$MAX_RATIO" ]; then
  echo "binserialize-linearity: FAIL — encode traffic grows ${ratio}% from n=$N to n=$((N*2)) (max ${MAX_RATIO}%)"
  echo "  n=0     incref_total=$i0"
  echo "  n=$N    incref_total=$i1  (list-attributable $d1)"
  echo "  n=$((N*2)) incref_total=$i2  (list-attributable $d2)"
  echo "  an encoder that re-copies its accumulator per element is the shape this catches"
  exit 1
fi

echo "binserialize-linearity: OK (${ratio}% growth for 2x the elements, max ${MAX_RATIO}%)"
