#!/bin/sh
# The c-modular stream is a function of the program alone — cold == warm == oracle.
#
# The core cache flags only choose where parsed core blobs are read from; they
# must never change what is printed. The stream must be byte-identical whether
# the build was
#
#   - no-cache   (cache flags absent, the oracle)
#   - cold-cache (dir + toolchain id set, first run)
#   - warm-cache (second run)
#   - another toolchain id (the id keys the cache, not the output)
#
# The user program deliberately exercises generics, a protocol impl on a user
# type, a user effect with a handler, and units of measure — the surfaces whose
# lowering most plausibly could leak user state into a core unit. A body-only
# edit and an added impl change only the root units: every core unit is
# byte-identical to the original program's.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the repo declares another.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
STDLIB="$ROOT/stdlib"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

fail() { echo "emitc_cold_warm_identical FAIL — $1"; exit 1; }

cat > "$PROJ/main.kai" <<'EOF'
unit m
unit s

type Shade = Light | Dark

impl Show for Shade {
  fn show(x: Shade) : String = match x {
    Light -> "light"
    Dark  -> "dark"
  }
}

effect Tick {
  tick() : Int
}

fn speeds[u: Measure](xs: [Real<u>], t: Real<s>) : [Real<u/s>] =
  xs | (x) => x / t

fn main() : Unit / Stdout = {
  let xs = [1, 2, 3, 4]
  let ys = xs | (x) => x * x
  let total = foldr(ys, 0, (a, b) => a + b)
  let vs = speeds([10.0<m>, 20.0<m>], 2.0<s>)
  let n = handle {
    Tick.tick() + Tick.tick()
  } with Tick {
    tick(resume) -> resume(7)
  }
  Stdout.print("#{total} #{show(Dark)} #{list_length(vs)} #{n}")
}
EOF

CACHE="$PROJ/core-cache"
mkdir -p "$CACHE"

stream() { # out program [flags...]
  out="$1"; prog="$2"; shift 2
  "$KAIC2" $EDITION_FLAG --emit=c-modular "$@" --path "$STDLIB" --path "$PROJ" "$PROJ/$prog" \
    > "$PROJ/$out" 2> "$PROJ/$out.err" || { cat "$PROJ/$out.err"; fail "$prog does not compile ($out)"; }
}

stream oracle.stream main.kai
stream cold.stream   main.kai --core-cache-dir "$CACHE" --toolchain-id tid1
stream warm.stream   main.kai --core-cache-dir "$CACHE" --toolchain-id tid1
stream tid2.stream   main.kai --core-cache-dir "$CACHE" --toolchain-id tid2

cmp -s "$PROJ/cold.stream" "$PROJ/oracle.stream" || fail "the cold-cache stream differs from the no-cache oracle"
cmp -s "$PROJ/warm.stream" "$PROJ/oracle.stream" || fail "the warm-cache stream differs from the no-cache oracle"
cmp -s "$PROJ/tid2.stream" "$PROJ/oracle.stream" || fail "another toolchain id changed the stream"

# One file per unit of a stream, named by the unit.
split() {
  mkdir -p "$PROJ/$1.tu"
  awk -v dir="$PROJ/$1.tu/" '
    /^\/\/KAIFILE:/ { f = $0; sub(/.*::/, "", f); out = dir f; next }
    /^\/\/KAIDELIM/ { next }
    out != "" { print > out }' "$PROJ/$1.stream"
}

sed 's/"light"/"lighter"/' "$PROJ/main.kai" > "$PROJ/edited.kai"
cat "$PROJ/main.kai" > "$PROJ/impl_added.kai"
cat >> "$PROJ/impl_added.kai" <<'EOF'

impl Eq for Shade {
  fn eq(a: Shade, b: Shade) : Bool = match (a, b) {
    (Light, Light) -> true
    (Dark, Dark)   -> true
    _              -> false
  }
}
EOF
stream edited.stream     edited.kai
stream impl_added.stream impl_added.kai
split oracle; split edited; split impl_added

for p in edited impl_added; do
  for f in "$PROJ"/oracle.tu/*; do
    u="$(basename "$f")"
    case "$u" in *__root__*|*main*|*edited*|*impl_added*) continue ;; esac
    [ -f "$PROJ/$p.tu/$u" ] || fail "unit $u is missing from $p.kai"
    cmp -s "$f" "$PROJ/$p.tu/$u" || fail "unit $u changed in $p.kai (a core unit must not depend on the program's edits)"
  done
done

# A test-shaped build compiles under the cache flags too.
"$KAIC2" $EDITION_FLAG --emit=c-modular --test --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" > /dev/null 2> "$PROJ/test.err" || true
if grep -q "internal error" "$PROJ/test.err"; then cat "$PROJ/test.err"; fail "a test-shaped build hit an internal error"; fi

echo "emitc_cold_warm_identical OK"
