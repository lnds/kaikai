#!/bin/bash
# A derived BinSerialize encode must cost O(bytes), not O(bytes²).
#
# Two shapes: a record holding a list of N records, and a recursive value
# N levels deep through a sum, a record, an option and a list. An encoder
# that re-copies an accumulator per element, or a subtree per level, keeps
# the output bytes correct, so no golden notices; only the cost curve does.
#
# Each shape is encoded at N and 2N and the runtime RC ledger is read.
# Every byte copied into an output buffer is read back with an owning
# `array_get`, so `incref_total` counts the copy traffic. Under a linear
# encoder doubling N doubles the N-attributable traffic; under a quadratic
# one it quadruples. The RATIO, not an absolute count, is pinned, so the
# gate is stable across machines and unrelated runtime changes. The
# backend is pinned because the two emit different counts for the same
# shape.

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

gen_list() {
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

gen_rec() {
  cat > "$2" <<EOF
#[derive(BinSerialize)]
type Expr = Lit(Int) | Call(String, [Expr]) | Let(Stmt, Expr)

#[derive(BinSerialize)]
type Stmt = { name: String, value: Option[Expr] }

fn nest(i: Int, acc: Expr) : Expr =
  if i <= 0 { acc }
  else { nest(i - 1, Let(Stmt { name: "x", value: Some(Lit(i)) }, Call("f", [acc]))) }

fn main() : Unit / Stdout = {
  let bs = to_bytes(nest($1, Lit(0)))
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

# check <shape>: the growth of <shape>'s encode traffic from N to 2N.
check() {
  local shape="$1"
  "gen_$shape" 0        "$WORK/${shape}0.kai"
  "gen_$shape" "$N"     "$WORK/${shape}1.kai"
  "gen_$shape" $((N*2)) "$WORK/${shape}2.kai"

  local i0 i1 i2
  i0="$(increfs "${shape}0")"
  i1="$(increfs "${shape}1")"
  i2="$(increfs "${shape}2")"

  for v in "$i0" "$i1" "$i2"; do
    case "$v" in ''|*[!0-9]*) echo "binserialize-linearity: $shape: no RC ledger from the encoder" >&2; return 1 ;; esac
  done

  local d1=$((i1 - i0)) d2=$((i2 - i0))
  [ "$d1" -gt 0 ] || { echo "binserialize-linearity: $shape: no N-attributable traffic at n=$N" >&2; return 1; }

  local ratio=$(( d2 * 100 / d1 ))
  if [ "$ratio" -gt "$MAX_RATIO" ]; then
    echo "binserialize-linearity: FAIL — $shape encode traffic grows ${ratio}% from n=$N to n=$((N*2)) (max ${MAX_RATIO}%)"
    echo "  n=0     incref_total=$i0"
    echo "  n=$N    incref_total=$i1  (N-attributable $d1)"
    echo "  n=$((N*2)) incref_total=$i2  (N-attributable $d2)"
    echo "  an encoder that re-copies an accumulator or a subtree is the shape this catches"
    return 1
  fi
  echo "binserialize-linearity: $shape OK (${ratio}% growth for 2x N, max ${MAX_RATIO}%)"
}

status=0
check list || status=1
check rec  || status=1
exit $status
