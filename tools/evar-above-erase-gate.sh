#!/bin/sh
# A reference minted between the resolver and the erasure carries its id.
#
# `resolve` turns a name that reaches a declaration into an `ESym`, and
# `sym_erase` (driver, just before unbox) turns it back into a spelling
# because below that line a name is a C symbol, not a reference. Between
# those two points a pass that mints a bare `EVar` re-introduces exactly
# the ambiguity the resolver settled: the next reader compares spellings
# and may answer with a different declaration.
#
# Below the erasure `EVar` is the correct and only form, so those passes
# are not the subject of this gate. Above the resolver there is no id to
# carry yet. The list below is therefore the set of passes that sit in
# the window and are still allowed to mint a bare reference, each for a
# reason that is written down.
#
# The failure this prevents is silent: a bare reference resolves to
# *something*, so the compiler builds, the self-host stays
# byte-identical and the corpus stays green. Only a count fails.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/stage2/compiler"

# Passes inside the window that may still mint a bare reference.
#
#   ast              — `mk_ref` itself, the one constructor.
#   parse, desugar,  — run before the resolver; there is no id yet.
#   surface_lower,
#   modules, type_scope, effect_scope, const_pattern, refinements,
#   refine_enforce, vec_surface, protos, json_derive, layout_derive,
#   fmt_expr, tailfuse, tailsubst, pipe_fusion, resolve
#   infer            — mints calls to runtime primitives (`array_get`,
#                      `vec_slice`) that no user declaration backs.
#   monomorph        — the callee of a specialisation it just created;
#                      the declaration did not exist to be resolved.
#   fwd_inline,      — mint a primitive or a binder they just bound.
#   closure_spec_ast,
#   closure_spec_emit
#   cache_ast        — deserialisation; ids are rebuilt on load.
#   sym_read         — the ESym→EVar reader itself.
#   emit_tcrec_live  — the matches are prose inside a `#[doc]` block.
#   cache_delta_test,— fixtures; they build the node they assert on.
#   resolve_sym_test
ALLOW='ast|parse|desugar|surface_lower|modules|type_scope|effect_scope'
ALLOW="$ALLOW|const_pattern|refinements|refine_enforce|vec_surface|protos"
ALLOW="$ALLOW|json_derive|layout_derive|fmt_expr|tailfuse|tailsubst"
ALLOW="$ALLOW|pipe_fusion|resolve|infer|monomorph|fwd_inline"
ALLOW="$ALLOW|closure_spec_ast|closure_spec_emit|cache_ast|sym_read"
ALLOW="$ALLOW|emit_tcrec_live|cache_delta_test|resolve_sym_test"

# Passes at or below the erasure. `EVar` is their correct form: the name
# is a C symbol there, and `sym_erase` is what put it back.
BELOW='sym_erase|perceus|perceus_plant_drop|perceus_tail_drop|perceus_op_arg'
BELOW="$BELOW|perceus_payer|perceus_let_own"
BELOW="$BELOW|emit_c|emit_shared|unbox|unbox_native_raw|kir_lower|kir_lower_walk"
BELOW="$BELOW|cell_promote|region|driver"

# Count constructions, not pattern matches. Two forms build one: the
# `mk_ref` helper every pass should call, and a raw `EVar(...)` that is
# not immediately followed by `->` (that arrow marks a match arm taking
# a node apart, not building one). Comment lines are dropped first so
# prose inside a doc block never counts.
scan() {
  for f in "$SRC"/*.kai; do
    body=$(sed 's/^[[:space:]]*#.*$//' "$f" | tr '\n' ' ')
    raw=$(printf '%s' "$body" \
        | grep -oE 'EVar\([^()]*\)[[:space:]]*(->)?' \
        | grep -vcE '\->[[:space:]]*$' || true)
    helper=$(printf '%s' "$body" | grep -oE 'mk_ref\(' | wc -l | tr -d ' ')
    n=$(( ${raw:-0} + ${helper:-0} ))
    [ "$n" -gt 0 ] && printf '%s %s\n' "$(basename "$f" .kai)" "$n"
  done
}

offenders=$(scan | grep -vE "^($ALLOW|$BELOW) " || true)

if [ -n "$offenders" ]; then
  echo "evar-above-erase FAIL — these passes mint a bare reference between resolve and the erasure:" >&2
  echo "$offenders" | sed 's/^/  /' >&2
  echo "" >&2
  echo "  A reference to a declaration the resolver settled is an ESym:" >&2
  echo "    ESym(sym, name)   not   mk_ref(name, line, col)" >&2
  echo "  A pass that mints a reference to something no declaration" >&2
  echo "  backs — a runtime primitive, a binder it just bound, a" >&2
  echo "  specialisation it just created — belongs in this script's" >&2
  echo "  ALLOW list, with the reason written down." >&2
  exit 1
fi

n=$(scan | grep -E "^($ALLOW) " | awk '{s+=$2} END {print s+0}')
b=$(scan | grep -E "^($BELOW) " | awk '{s+=$2} END {print s+0}')
echo "evar-above-erase OK — $n bare references minted in declared-exempt passes above the erasure, $b below it where the form is correct"
