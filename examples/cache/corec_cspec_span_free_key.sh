#!/bin/sh
# Core translation units are independent of the user program's edits.
#
# `closure_spec_pass` appends per-call-site spec decls named
# `<fn>__cspec__L<line>_<col>` with `mo=None` (root-homed). Their names carry
# the lambda body's span, so a plain body edit that adds, removes, or moves a
# specialisable HOF call must not change any unit but the root one: the
# per-unit object cache is keyed by each unit's text, and a core unit that
# changed would recompile on every such edit.
#
#   - A: `list.sum([1,2,3])`                       -> the baseline unit set.
#   - B: `list.sum(list.map([1,2,3], (x)=>x*2))`   -> a HOF-with-lambda.
#   - C: B plus a new top-level fn.
#   - D: A plus a row that may cancel (`/ Spawn`).
#   - E: B with another lambda body.
#
# Every unit but the root one is byte-identical across all five.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the repo declares another.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
STDLIB="$ROOT/stdlib"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

fail() { echo "corec_cspec_span_free_key FAIL — $1"; exit 1; }

cat > "$PROJ/a.kai" <<'EOF'
fn main() : Unit / Console {
  print("#{int_to_string(list.sum([1, 2, 3]))}")
}
EOF
cat > "$PROJ/b.kai" <<'EOF'
fn main() : Unit / Console {
  print("#{int_to_string(list.sum(list.map([1, 2, 3], (x) => x * 2)))}")
}
EOF
cat > "$PROJ/c.kai" <<'EOF'
fn helper(n: Int) : Int = n + 1
fn main() : Unit / Console {
  print("#{int_to_string(list.sum(list.map([1, 2, 3], (x) => x * 2)))}")
}
EOF
cat > "$PROJ/d.kai" <<'EOF'
fn helper() : Unit / Spawn = ()
fn main() : Unit / Console {
  print("#{int_to_string(list.sum([1, 2, 3]))}")
}
EOF
cat > "$PROJ/e.kai" <<'EOF'
fn main() : Unit / Console {
  print("#{int_to_string(list.sum(list.map([1, 2, 3], (x) => x * 3)))}")
}
EOF

# One file per unit of the stream, named by the unit.
split() {
  mkdir -p "$PROJ/$1.tu"
  "$KAIC2" $EDITION_FLAG --emit=c-modular --path "$STDLIB" --path "$PROJ" "$PROJ/$1.kai" \
    > "$PROJ/$1.stream" 2> "$PROJ/$1.err" || { cat "$PROJ/$1.err"; fail "$1.kai does not compile"; }
  awk -v dir="$PROJ/$1.tu/" '
    /^\/\/KAIFILE:/ { f = $0; sub(/.*::/, "", f); out = dir f; next }
    /^\/\/KAIDELIM/ { next }
    out != "" { print > out }' "$PROJ/$1.stream"
}

for p in a b c d e; do split "$p"; done

n="$(ls "$PROJ"/a.tu | wc -l | tr -d ' ')"
[ "$n" -gt 1 ] || fail "the stream split into $n unit(s)"

for p in b c d e; do
  for f in "$PROJ"/a.tu/*; do
    u="$(basename "$f")"
    case "$u" in *__root__*) continue ;; esac
    [ -f "$PROJ/$p.tu/$u" ] || fail "unit $u of A is missing from $p.kai"
    cmp -s "$f" "$PROJ/$p.tu/$u" || fail "unit $u changed between a.kai and $p.kai (a core unit must not depend on the program's edits)"
  done
done
cmp -s "$PROJ/a.tu/__root__.c" "$PROJ/b.tu/__root__.c" && fail "the HOF-with-lambda edit left the root unit unchanged"

echo "corec_cspec_span_free_key OK — a HOF-with-lambda edit, a new fn and a new row change only the root unit"
exit 0
