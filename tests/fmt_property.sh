#!/bin/sh
# tests/fmt_property.sh — `kai fmt` meaning-preservation property harness.
#
# The fixture suite (tests/fmt_fixtures.sh) only covers shapes somebody
# thought to write down; the selfhost ratchet (tests/fmt_selfhost.sh)
# only covers shapes that happen to occur in stdlib/ + stage2/compiler/.
# Neither checks the central property of a formatter: FMT PRESERVES
# MEANING. A rewrite that changes which variant a comment documents, or
# that emits an internal binder name, passes both nets and still
# corrupts the user's file — silently, in place.
#
# For every source in the corpus this gate asserts four properties:
#
#   (a) exit 0        — fmt does not refuse the file
#   (b) re-parse      — fmt's output is still valid kaikai
#   (c) AST equality  — parse(fmt(src)) == parse(src), modulo positions
#   (d) idempotency   — fmt(fmt(x)) == fmt(x)
#
# (b) is what catches a writer emitting internal syntax; (c) is what
# catches a writer that produces parseable but DIFFERENT code; (d) is
# what catches a writer with no fixed point.
#
# ---------------------------------------------------------------
# What (c) actually compares — and what it does not
# ---------------------------------------------------------------
# The comparison uses `kaic2 --ast`, the parser's own dumper, with
# source positions stripped (`@line:col` suffixes). Positions MUST be
# normalised: reformatting moves every token, so an unnormalised dump
# differs for every file and the check would be vacuous.
#
# The dumper is structural and exhaustive over Expr/Pattern/TypeExpr/
# Decl — no catch-all arm — so it discriminates far more than a token
# diff. But it is a PROXY, not a structural equality on the AST type,
# and three slots are dropped by the dumper itself:
#
#   * `TyRefine(base, _)`  — the where-predicate is not printed, so a
#     rewrite confined to a refinement predicate is invisible to (c).
#     Property (d) is the net that covers that region (it is how
#     #1603 shows up).
#   * `DDoc(_, _, inner, ...)` — the `#[doc]` text is not printed, so
#     corruption of a doc string is invisible to (c).
#   * `ELambda` prints only each param's NAME, not its annotation.
#
# Comments are not in the AST at all, so (c) cannot see a comment being
# re-attached to the wrong element (#1597) directly — it sees it only
# when the move also perturbs structure. That is a real limit of any
# AST-level check and is why (c) does not retire the golden fixtures:
# goldens remain the only net over comment placement and layout.
#
# Widening the dumper to close the three slots above would strengthen
# this gate; it is deliberately NOT done here, because changing --ast
# output is a change to a debugging surface other tests read.
#
# ---------------------------------------------------------------
# Exception list
# ---------------------------------------------------------------
# KNOWN_* below list files that fail a property because of an OPEN,
# NUMBERED bug. Each entry carries its issue. The lists must reach
# zero as the fixing lanes close, exactly like the skip-list in
# tests/fmt_selfhost.sh. An exception waives ONE property for ONE file:
# a file excused from (c) is still required to satisfy (a), (b), (d),
# so an exception cannot silently mask a total refusal.
#
# A file that STOPS failing while still listed is reported as a stale
# exception and fails the run — that is what forces the list to empty
# instead of rotting.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# Most of the corpus imports stdlib (`import spawn`, `import actor`,
# `collections.map`, …). Without this the parse fails with "cannot open
# module" and the file is dropped as unparseable — which silently shrank
# coverage by roughly a quarter of the corpus before it was caught.
KPATH="--path $ROOT/stdlib"

# ---------------------------------------------------------------
# Known-failing files, by property, each tagged with its issue.
# Remove an entry when its issue closes.
# ---------------------------------------------------------------

# (b) re-parse — writer emits syntax the parser does not accept.
KNOWN_REPARSE="
examples/perceus/pipe_fusion_ring_1143.kai
examples/perceus/range_lazy_1180.kai
examples/perceus/range_lazy_sum_1180.kai
examples/perceus/range_step_zero_1180.kai
examples/perceus/vec_collect_1150.kai
examples/sugars/shape_kind_dispatch.kai
examples/sugars/shape_kind_fusion.kai
examples/sugars/shape_kind_laws.kai
examples/sugars/shape_kind_laws_axiom.kai
examples/sugars/shape_kind_laws_violation.kai
examples/sugars/shape_kind_arity_err.kai
examples/sugars/shape_kind_composition_err.kai
examples/sugars/shape_kind_scalar_err.kai
examples/perceus/shape_fusion_1200.kai
examples/sugars/if_cond_record_lit_delimited.kai
examples/protocols/free_fn_interp_mono_collision.kai
examples/stdlib/hashmap_collision.kai
"

# (c) AST equality — output parses but denotes something else.
# #1606 — 'requires' erased from the signature into a body 'assert',
# which silently stops the compiler checking the contract at call sites.
KNOWN_AST="
examples/intervals/call_site_literal.kai
examples/intervals/call_site_refined.kai
examples/intervals/call_site_too_loose.kai
examples/intervals/call_site_unproven.kai
examples/refinements/contracts_passing.kai
examples/refinements/ensures_violation_diagnostic.kai
examples/refinements/requires_complex_predicate.kai
examples/refinements/requires_help_narrow.kai
examples/refinements/requires_help_narrow_int.kai
examples/refinements/requires_named_binding.kai
examples/refinements/requires_value_boxed_real.kai
examples/refinements/requires_value_reused_binding.kai
examples/refinements/requires_violation_char.kai
examples/refinements/requires_violation_diagnostic.kai
examples/refinements/requires_violation_real.kai
examples/refinements/requires_violation_string.kai
examples/sugars/m12_6_const_basic.kai
examples/sugars/m12_6_contract_basic.kai
examples/sugars/m12_6_contract_const_fold.kai
examples/sugars/m12_6_contract_result.kai
examples/sugars/m12_6_param_entail.kai
examples/sugars/m12_6_pred_pure_ok.kai
examples/sugars/m12_6_pred_pure_user_fn.kai
examples/sugars/m12_6_pure_attr_inline.kai
examples/sugars/m12_6_pure_attr_no_collision.kai
examples/sugars/m12_6_pure_attr_ok.kai
# #1607 — hex/binary literals re-rendered as decimal; the emitted '-1'
# re-parses as a unop over a literal, not the single literal it was.
examples/numeric/hex_literal.kai
# #1608 — imaginary literal emitted as its 'complex.mk' desugar, with an
# Int where the call needs a Real, so the output no longer type-checks.
examples/sugars/complex_literal_basic.kai
# #1610 — regex literal '~r/.../' in a where-refinement replaced by a
# malformed desugar that names a module not yet in scope.
examples/stdlib/regex_anchors_repetition.kai
examples/stdlib/regex_predicate_basic.kai
examples/stdlib/regex_subsume_alpha.kai
examples/stdlib/regex_subsume_basic.kai
examples/stdlib/regex_subsume_unsupported.kai
# #1603 — where-clause body collapses to one line then re-expands to a
# block, which also shows up as a structural change.
examples/sugars/m12_6_ensure_primary.kai
"

# (d) idempotency — no fixed point in one pass.
KNOWN_IDEMPOTENT="
examples/stdlib/regex_anchors_repetition.kai
examples/stdlib/regex_predicate_basic.kai
examples/stdlib/regex_subsume_alpha.kai
examples/stdlib/regex_subsume_basic.kai
examples/stdlib/regex_subsume_unsupported.kai
examples/sugars/m12_6_ensure_primary.kai
"

if [ ! -x "$KAIC2" ]; then
  echo "fmt_property: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

# The parent creates the scratch dir and exports it; a worker re-entered
# via xargs inherits it instead of minting its own (and must not sweep it
# on exit — only the parent cleans up).
if [ -n "${FMT_PROPERTY_TMP:-}" ]; then
  tmp="$FMT_PROPERTY_TMP"
else
  tmp="$(mktemp -d)"
fi

# Scratch artefacts that must be PARSED live next to their source file,
# not in $tmp: a multi-module example resolves `import foo` relative to
# the importing file's directory, so a copy parked elsewhere dies with
# "cannot open module". They are named with a fixed prefix and swept on
# exit, including on interrupt.
SCRATCH_PREFIX=".fmtprop-tmp"
cleanup() {
  rm -rf "$tmp"
  find "$ROOT/examples" -name "$SCRATCH_PREFIX*" -type f -delete 2>/dev/null || true
}
# Only the parent sweeps: a worker exiting would otherwise delete the
# shared scratch dir out from under its siblings.
if [ -z "${FMT_PROPERTY_TMP:-}" ]; then
  trap cleanup EXIT INT TERM
fi

# $1 = needle, $2 = newline-separated haystack.
#
# `case` against a newline-delimited blob rather than a loop: this runs
# once per property per file over the whole corpus, and a per-entry loop
# spawning `sed` dominated the harness's runtime.
#
# The lists carry `# refs #N` annotation lines. Anchoring each side with
# a newline means a comment can never match — a bare path is only found
# when it occupies a whole line — so an annotation that happened to
# contain a path cannot excuse that file silently.
in_list() {
  case "
$2
" in
    *"
$1
"*) return 0 ;;
  esac
  return 1
}

# Normalise source positions out of the dump so it compares structure
# only. Reformatting moves every token; without this the check is
# vacuous.
#
# Two forms carry position:
#   * the dumper's own `@line:col` node suffix;
#   * `declared at line N, col M` INSIDE a string literal — the
#     refinement lowering synthesises its runtime diagnostic message at
#     parse time and embeds the predicate's source position in it;
#   * `__<tag>_LINE_COL_N__` — several desugars mint binder names from
#     the construct's position (`__rf_…` for record literals,
#     `__spread_src_…` for record spread).
#
# The last two legitimately change when a file is reformatted: the
# construct moved, so a position-derived name moves with it. Both are
# normalised rather than reported as meaning changes. Note this makes
# the check blind to a genuine RENUMBERING of such a binder, which is
# accepted: the alternative is ~20 files of permanent false positives.
strip_pos() {
  sed -e 's/ @[0-9][0-9]*:[0-9][0-9]*//g' \
      -e 's/declared at line [0-9][0-9]*, col [0-9][0-9]*/declared at line L, col C/g' \
      -e 's/__\([a-z_]*\)_[0-9][0-9]*_[0-9][0-9]*__/__\1_L_C__/g' \
      -e 's/__\([a-z_]*\)_[0-9][0-9]*_[0-9][0-9]*_\([0-9][0-9]*\)__/__\1_L_C_\2__/g' "$1"
}

# Jobs: parallel workers. Four kaic2 invocations per file over ~1800
# files is ~18 minutes serial, which does not fit a Tier 1 shard; the
# work is per-file independent, so it fans out. Defaults to the host's
# logical CPU count, same convention as tools/test-backend-parity.sh.
if [ -n "${FMT_PROPERTY_JOBS:-}" ]; then
  JOBS="$FMT_PROPERTY_JOBS"
elif command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
  JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
else
  JOBS=4
fi

results="$tmp/results"      # one char per file: P|F|E|U|S|R
failures="$tmp/failures"    # multi-line report block per failure

# Only the parent truncates. A worker re-entered via xargs runs this
# same prologue, so an unconditional `: >` here has every worker wipe
# the tally its siblings already wrote.
if [ -z "${FMT_PROPERTY_TMP:-}" ]; then
  : > "$results"
  : > "$failures"
fi

# Worker — one fixture, invoked via `xargs -P $JOBS -n 1`.
#
# Appends exactly one status char to $results and, on failure or a stale
# exception, one report block to $failures. Scratch files are keyed by
# the worker's PID so concurrent workers cannot collide.
check_one() {
  abs="$1"
  rel="${abs#"$ROOT"/}"
  b="$tmp/w$$"
  dir="$(dirname "$abs")"
  p1="$dir/$SCRATCH_PREFIX.$$.p1.kai"
  p2="$dir/$SCRATCH_PREFIX.$$.p2.kai"

  # A file the PARSER already rejects is not a formatter subject —
  # negative fixtures deliberately contain syntax errors. Count it so
  # the corpus reach stays auditable.
  if ! "$KAIC2" $KPATH --ast "$abs" > "$b.ast0" 2> "$b.err"; then
    printf 'U' >> "$results"
    return 0
  fi

  # (a) fmt must not refuse.
  if ! "$KAIC2" $KPATH --fmt "$abs" > "$p1" 2> "$b.err"; then
    # An explicit `kai fmt:` refusal is a documented out-of-scope
    # construct, not a failure.
    if grep -q "kai fmt:" "$b.err"; then
      printf 'R' >> "$results"
      rm -f "$p1"
      return 0
    fi
    { echo "  FAIL $rel — (a) fmt errored:"; sed 's/^/      /' "$b.err"; } >> "$failures"
    printf 'F' >> "$results"
    rm -f "$p1"
    return 0
  fi

  # (b) the output must re-parse.
  if "$KAIC2" $KPATH --ast "$p1" > "$b.ast1" 2> "$b.err"; then
    reparsed=1
  else
    reparsed=0
  fi

  if [ "$reparsed" -eq 0 ]; then
    if in_list "$rel" "$KNOWN_REPARSE"; then
      printf 'E' >> "$results"
    else
      { echo "  FAIL $rel — (b) formatted output does not re-parse:"
        sed 's/^/      /' "$b.err"; } >> "$failures"
      printf 'F' >> "$results"
    fi
    rm -f "$p1"
    return 0
  fi
  if in_list "$rel" "$KNOWN_REPARSE"; then
    echo "  STALE $rel — listed in KNOWN_REPARSE but (b) now passes; remove the entry" >> "$failures"
    printf 'S' >> "$results"
    rm -f "$p1"
    return 0
  fi

  # (c) the ASTs must agree, modulo position.
  strip_pos "$b.ast0" > "$b.n0"
  strip_pos "$b.ast1" > "$b.n1"
  if ! diff -u "$b.n0" "$b.n1" > "$b.astdiff"; then
    if in_list "$rel" "$KNOWN_AST"; then
      printf 'E' >> "$results"
    else
      { echo "  FAIL $rel — (c) fmt changed the AST:"
        head -40 "$b.astdiff" | sed 's/^/      /'; } >> "$failures"
      printf 'F' >> "$results"
    fi
    rm -f "$p1"
    return 0
  fi
  if in_list "$rel" "$KNOWN_AST"; then
    echo "  STALE $rel — listed in KNOWN_AST but (c) now passes; remove the entry" >> "$failures"
    printf 'S' >> "$results"
    rm -f "$p1"
    return 0
  fi

  # (d) idempotency.
  if ! "$KAIC2" $KPATH --fmt "$p1" > "$p2" 2> "$b.err"; then
    { echo "  FAIL $rel — (d) fmt refused its own output:"
      sed 's/^/      /' "$b.err"; } >> "$failures"
    printf 'F' >> "$results"
    rm -f "$p1" "$p2"
    return 0
  fi
  if ! diff -u "$p1" "$p2" > "$b.idiff"; then
    if in_list "$rel" "$KNOWN_IDEMPOTENT"; then
      printf 'E' >> "$results"
    else
      { echo "  FAIL $rel — (d) fmt is not idempotent:"
        head -40 "$b.idiff" | sed 's/^/      /'; } >> "$failures"
      printf 'F' >> "$results"
    fi
    rm -f "$p1" "$p2"
    return 0
  fi
  if in_list "$rel" "$KNOWN_IDEMPOTENT"; then
    echo "  STALE $rel — listed in KNOWN_IDEMPOTENT but (d) now passes; remove the entry" >> "$failures"
    printf 'S' >> "$results"
    rm -f "$p1" "$p2"
    return 0
  fi

  printf 'P' >> "$results"
  rm -f "$p1" "$p2"
}

# xargs re-enters this script with --worker so the function is available
# in the child shell; $0 is the script itself.
if [ "${1:-}" = "--worker" ]; then
  shift
  check_one "$1"
  exit 0
fi

# Corpus: every example. This is the point of the harness — examples/
# exercises surface (rest-patterns, kind-annotated type params,
# delimited record literals in condition position) that stdlib/ and
# stage2/compiler/ never use, which is exactly why the selfhost
# ratchet could not see these bugs.
find "$ROOT/examples" -name '*.kai' -type f | sort > "$tmp/corpus"

FMT_PROPERTY_TMP="$tmp"
export FMT_PROPERTY_TMP
xargs -P "$JOBS" -n 1 "$0" --worker < "$tmp/corpus"

pass=$(tr -cd 'P' < "$results" | wc -c | tr -d ' ')
fail=$(tr -cd 'F' < "$results" | wc -c | tr -d ' ')
excused=$(tr -cd 'E' < "$results" | wc -c | tr -d ' ')
stale=$(tr -cd 'S' < "$results" | wc -c | tr -d ' ')
unparseable=$(tr -cd 'U' < "$results" | wc -c | tr -d ' ')
refused=$(tr -cd 'R' < "$results" | wc -c | tr -d ' ')

cat "$failures"
echo "fmt_property: $pass passed, $excused excused (open issues), $refused refused (out of scope), $unparseable not parseable, $stale stale, $fail failed"

if [ "$stale" -gt 0 ]; then
  echo "fmt_property: an exception is listed for a file that now passes — the list must shrink to zero, not rot" >&2
  exit 1
fi
if [ "$fail" -gt 0 ]; then
  exit 1
fi
