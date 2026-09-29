#!/bin/bash
# Hand-rolled expression walkers in stage2/compiler/.
#
# A function that matches eight or more `ExprKind` constructors and closes
# the match with a `_ ->` catch-all is enumerating sub-expressions by hand.
# The catch-all silently drops whatever variant its author did not think
# of, so the walk goes blind to it. `expr_positions.kai` (`xp_positions`)
# is the one exhaustive enumeration; a walker descends through it and keeps
# explicit arms only for the variants it treats specially.
#
# Pattern recognizers (does this expression have shape X?) legitimately
# match many constructors and answer "no" for the rest, and no syntactic
# rule tells them apart from walkers. So this is a ratchet: every existing
# site is pinned in the baseline, one `<file> <fn>` per line. A site not
# listed fails; a listed site that no longer matches fails too (shrink the
# baseline in that commit).
#
# `--self-test` injects each violation into a scratch copy and asserts the
# audit rejects it.
set -u
export LC_ALL=C
cd "$(dirname "$0")/.."

MIN_ARMS=8

# ExprKind constructor names, read from the type declaration itself.
expr_ctors() {
  awk '/^pub type ExprKind/{on=1; next}
       on && /^pub /{exit}
       on && match($0, /^[ \t]*[=|][ \t]*[A-Z][A-Za-z0-9]*/) {
         s=substr($0, 1, RLENGTH); sub(/^[ \t]*[=|][ \t]*/, "", s); print s
       }' "$1/stage2/compiler/ast.kai"
}

# observed <root>: `<file> <fn>` for every function holding a match with at
# least MIN_ARMS ExprKind arms and a catch-all at the same indentation.
observed() {
  local root="$1" ctors
  ctors=$(expr_ctors "$root" | tr '\n' ' ')
  for f in "$root"/stage2/compiler/*.kai; do
    awk -v file="$(basename "$f")" -v ctors="$ctors" -v min="$MIN_ARMS" '
      BEGIN { n = split(ctors, cs, " "); for (i = 1; i <= n; i++) isctor[cs[i]] = 1 }
      function flush(   k) {
        for (k in cnt) if (cnt[k] >= min && (k in catchall)) { print file " " fn; break }
        delete cnt; delete catchall; delete seen
      }
      /^(pub )?fn / {
        flush(); fn = $0; sub(/^(pub )?fn /, "", fn); sub(/[^a-z_0-9].*/, "", fn); next
      }
      index($0, "->") == 0 { next }
      match($0, /^[ \t]*/) {
        ind = RLENGTH; rest = substr($0, ind + 1)
        if (rest ~ /^_[ \t]*->/) { if (ind in cnt) catchall[ind] = 1; next }
        sub(/^\[/, "", rest)
        if (match(rest, /^[A-Z][A-Za-z0-9]*/)) {
          c = substr(rest, 1, RLENGTH)
          if ((c in isctor) && !((ind, c) in seen)) { seen[ind, c] = 1; cnt[ind]++ }
        }
      }
      END { flush() }' "$f"
  done | sort -u
}

audit() {
  local root="$1" baseline="$1/tools/walker-catchall-baseline.txt" tmp new gone
  tmp=$(mktemp -d)
  observed "$root" > "$tmp/obs" || { rm -rf "$tmp"; echo "walker-catchall-audit FAIL — scan errored"; return 1; }
  grep -v '^#' "$baseline" | sed 's/[[:space:]]*#.*$//' | grep -v '^$' | sort -u > "$tmp/pinned"
  new=$(comm -13 "$tmp/pinned" "$tmp/obs") && gone=$(comm -23 "$tmp/pinned" "$tmp/obs") \
    || { rm -rf "$tmp"; echo "walker-catchall-audit FAIL — comm errored"; return 1; }
  rm -rf "$tmp"
  local status=0
  if [ -n "$new" ]; then
    echo "walker-catchall-audit FAIL — new match(es) over >= $MIN_ARMS ExprKind constructors closed by a catch-all:"
    echo "$new" | sed 's/^/  /'
    echo "A walker descends through xp_positions (expr_positions.kai) and keeps explicit"
    echo "arms only for the variants it treats specially. If this is a pattern recognizer"
    echo "that answers \"no\" for every other shape, add it to tools/walker-catchall-baseline.txt"
    echo "with a one-line reason: \`<file> <fn>  # <reason>\`."
    status=1
  fi
  if [ -n "$gone" ]; then
    echo "walker-catchall-audit FAIL — stale baseline entry(ies), shrink tools/walker-catchall-baseline.txt:"
    echo "$gone" | sed 's/^/  /'
    status=1
  fi
  return $status
}

self_test() {
  local tmp rc1 rc2
  tmp=$(mktemp -d)
  mkdir -p "$tmp/tools" "$tmp/stage2/compiler"
  cp tools/walker-catchall-baseline.txt "$tmp/tools/"
  cp stage2/compiler/*.kai "$tmp/stage2/compiler/"
  cat >> "$tmp/stage2/compiler/lint_spread.kai" <<'EOF'

fn wca_probe(k: ExprKind) : [Expr] = match k {
  ECall(f, xs)    -> [f, ...xs]
  EField(b, _)    -> [b]
  EIndex(a, b)    -> [a, b]
  EBinop(_, a, b) -> [a, b]
  EUnop(_, x)     -> [x]
  EIf(c, t, _)    -> [c, t]
  ELambda(_, b)   -> [b]
  EBang(x)        -> [x]
  _               -> []
}
EOF
  audit "$tmp" > /dev/null; rc1=$?
  cp stage2/compiler/lint_spread.kai "$tmp/stage2/compiler/"
  echo "lint_spread.kai wca_gone  # pre-existing, unaudited" >> "$tmp/tools/walker-catchall-baseline.txt"
  audit "$tmp" > /dev/null; rc2=$?
  rm -rf "$tmp"
  if [ $rc1 -eq 0 ] || [ $rc2 -eq 0 ]; then
    echo "walker-catchall-audit self-test FAIL — an injected violation passed"; return 1
  fi
  echo "walker-catchall-audit self-test OK"
}

if [ "${1:-}" = --self-test ]; then self_test || exit 1; fi
if [ "${1:-}" = --list ]; then observed "$(pwd)"; exit 0; fi
audit "$(pwd)" || exit 1
echo "walker-catchall-audit OK ($(grep -v '^#' tools/walker-catchall-baseline.txt | grep -vc '^$') pinned sites)"
