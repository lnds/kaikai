#!/bin/bash
# Block-reuse gate — freeing a variant structure larger than any pool cap
# and building it again must reuse the freed blocks, not grow the heap.
#
# Variant blocks live in slabs that never go back to libc, so a freed block
# that the free list does not keep is stranded for the rest of the run. The
# program builds and frees a 1.5M-block list once per round; peak RSS after
# six rounds must match peak RSS after one.

set -eu
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"
WORK="$ROOT/stage2/build/block-reuse"
CC="${CC:-cc}"

[ -x "$KAIC2" ] || { echo "block-reuse: SKIP — no stage2/kaic2"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "block-reuse: SKIP — no python3"; exit 0; }

rm -rf "$WORK"; mkdir -p "$WORK"
cat > "$WORK/main.kai" <<'KAI'
type Lst = LNil | LCons(Int, Lst)

fn mk(n : Int, acc : Lst) : Lst =
  if n == 0 { acc } else { mk(n - 1, LCons(n, acc)) }

fn len(l : Lst, a : Int) : Int =
  match l {
    LNil -> a
    LCons(_, t) -> len(t, a + 1)
  }

fn rounds(k : Int, acc : Int) : Int =
  if k == 0 { acc } else { rounds(k - 1, acc + len(mk(1500000, LNil), 0)) }

fn main() : Unit / Env + Process {
  exit(rounds(list_length(Env.args()), 0) % 256)
}
KAI
(cd "$WORK" && "$KAIC2" --path "$ROOT/stdlib" main.kai > main.c 2> emit.err) \
  || { cat "$WORK/emit.err"; echo "block-reuse: FAIL — emit"; exit 1; }
"$CC" -std=c99 -O2 -I "$ROOT/stage2" -I "$ROOT/stage0" "$WORK/main.c" -o "$WORK/main" -lm -lpthread \
  || { echo "block-reuse: FAIL — cc"; exit 1; }

python3 - "$WORK/main" <<'PY'
import os, resource, subprocess, sys
prog = sys.argv[1]
env = dict(os.environ, KAI_THREADS="1")
def peak_mb(rounds):
    # ru_maxrss of the waited-for children is the max over all of them, so
    # each measurement runs in its own python child.
    code = ("import os,resource,subprocess,sys;"
            "subprocess.run(sys.argv[1:],env=dict(os.environ,KAI_THREADS='1'));"
            "r=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss;"
            "print(r/1048576 if sys.platform=='darwin' else r/1024)")
    out = subprocess.run([sys.executable, "-c", code, prog] + ["r"] * rounds,
                         capture_output=True, text=True, check=True).stdout
    return float(out.strip())
one, six = peak_mb(1), peak_mb(6)
limit = one * 1.10 + 4
print(f"block-reuse: peak RSS 1 round {one:.1f} MB, 6 rounds {six:.1f} MB (limit {limit:.1f} MB)")
if six > limit:
    print("block-reuse: FAIL — freed variant blocks were not reused across rounds")
    sys.exit(1)
print("block-reuse OK")
PY
