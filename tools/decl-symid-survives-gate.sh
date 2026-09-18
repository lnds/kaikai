#!/bin/sh
# A declaration's identity must survive the passes that rewrite its body.
#
# The resolver stamps a `SymId` on each declaration form. A pass that
# rewrites a body and rebuilds the decl with `sym_none()` in that slot
# throws the identity away, and every consumer downstream falls back to
# comparing the declaration's bare name.
#
# The failure this prevents is silent: the compiler still builds, the
# self-host stays byte-identical and the corpus stays green, because a
# bare name resolves to *something*. Only a count like this one fails.
#
# The companion gate (symid-survives-gate.sh) holds the same property for
# `EHandle`, where the identity already has consumers. This one holds it
# for the declaration forms, where a body-rewriting pass is the likely
# place to drop it.
#
# A site that legitimately mints a declaration with no prior identity
# (the parser, a synthesised impl, a derive) is listed below by file, not
# by count, so adding one is a deliberate edit to this list.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/stage2/compiler"

# Files allowed to construct a declaration carrying no identity.
#   parse            — runs before the resolver; nothing to carry yet.
#   desugar          — synthesised wrappers with no source declaration.
#   cache_ast        — serialisation; ids are rebuilt on load, not stored.
#   protos           — lowers impls into fresh fns the resolver never saw.
#   json_derive      — same, for derived codecs.
#   layout_derive    — same, for derived layouts.
#   *_test           — fixtures constructing decls by hand.
ALLOW='parse|desugar|cache_ast|protos|json_derive|layout_derive|.*_test'

# A declaration construction spans several lines, so the id sits on the
# same line as the constructor only some of the time. Join each file into
# one line before matching, and count constructions whose identity slot —
# the last argument — is sym_none().
scan() {
  for f in "$SRC"/*.kai; do
    n=$(tr '\n' ' ' < "$f" \
        | grep -oE "D(Fn|Const)\([^()]*(\([^()]*\))?[^()]*, *sym_none\(\)" \
        | wc -l | tr -d ' ')
    [ "$n" -gt 0 ] && printf '%s %s\n' "$(basename "$f" .kai)" "$n"
  done
}

offenders=$(scan | grep -vE "^($ALLOW) " || true)

if [ -n "$offenders" ]; then
  echo "decl-symid-survives FAIL — these passes rebuild a declaration without carrying its identity:" >&2
  echo "$offenders" | sed 's/^/  /' >&2
  echo "" >&2
  echo "  Bind the SymId slot in the pattern and pass it through:" >&2
  echo "    DFn(p, n, tp, ps, rt, row, body, ln, cl, mo, fsym)" >&2
  echo "      -> DFn(p, n, tp, ps, rt, row, f(body), ln, cl, mo, fsym)" >&2
  echo "  A pass that legitimately mints an identity-free declaration" >&2
  echo "  belongs in this script's ALLOW list, with the reason written down." >&2
  exit 1
fi

n=$(scan | awk '{s+=$2} END {print s+0}')
echo "decl-symid-survives OK — $n identity-free declaration sites, all in declared-exempt passes"
