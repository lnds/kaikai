#!/bin/sh
# Identity must survive the pipeline.
#
# The resolver stamps a `SymId` on `EHandle`; a walker that rebuilds the
# node with `sym_none()` throws that identity away, and every consumer
# downstream falls back to comparing the effect's bare name. That is how
# two passes come to disagree about which declaration a name means.
#
# The failure this prevents is silent: the compiler still builds, the
# self-host stays byte-identical, and the corpus stays green, because a
# bare name resolves to *something*. Only a count like this one fails.
#
# A site that legitimately mints a node with no prior declaration (the
# parser, a synthesised handler) is listed below by file, not by count,
# so adding one is a deliberate edit to this list.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/stage2/compiler"

# Files allowed to construct an EHandle carrying no identity.
#   parse          — runs before the resolver; nothing to carry yet.
#   desugar        — the synthesised `State` handler wraps `var x = init`;
#                    it has no source declaration behind it.
#   qual_head      — mints a fresh node under a rewritten effect name.
#   scope_walk     — rewrites the effect name itself, so a carried id
#                    would point at the declaration the rename left.
#   loop_lower     — threads the node through a helper.
ALLOW='parse|desugar|qual_head|scope_walk|loop_lower'

# An EHandle construction spans several lines, so the id sits on the
# same line as the constructor only some of the time. Join each file
# into one line before matching, and count constructions whose third
# argument is sym_none() — `EHandle(` then two commas, then the id.
scan() {
  for f in "$SRC"/*.kai; do
    n=$(tr '\n' ' ' < "$f" \
        | grep -oE "EHandle\([^()]*(\([^()]*\))?[^()]*, *sym_none\(\)" \
        | wc -l | tr -d ' ')
    [ "$n" -gt 0 ] && printf '%s %s\n' "$(basename "$f" .kai)" "$n"
  done
}

offenders=$(scan | grep -vE "^($ALLOW) " || true)

if [ -n "$offenders" ]; then
  echo "symid-survives FAIL — these passes rebuild EHandle without carrying its identity:" >&2
  echo "$offenders" | sed 's/^/  /' >&2
  echo "" >&2
  echo "  Bind the SymId slot in the pattern and pass it through:" >&2
  echo "    EHandle(body, eff, hsym, ...) -> EHandle(f(body), eff, hsym, ...)" >&2
  echo "  A pass that legitimately mints an identity-free node belongs in" >&2
  echo "  this script's ALLOW list, with the reason written down." >&2
  exit 1
fi

n=$(scan | awk '{s+=$2} END {print s+0}')
echo "symid-survives OK — $n identity-free EHandle sites, all in declared-exempt passes"
