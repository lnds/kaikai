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

# Package fixtures (examples/packages/**) import modules the driver
# resolves through kai.toml; stdlib alone cannot parse them, which left
# them unmeasured. pkg_paths() below reconstructs the search paths a
# manifest implies, reading kai.toml.template ahead of kai.toml: the
# rendered manifests are machine-local (gitignored, absolute bare-repo
# paths), while the templates are in-repo, so a clean checkout works
# without running render-fixtures.sh. The two git-source placeholders
# map to the plain source dirs the bare repos are built from.
GIT_FIX="$ROOT/tests/fixtures/git-fixtures"

# ---------------------------------------------------------------
# Known-failing files, by property, each tagged with its issue.
# Remove an entry when its issue closes.
# ---------------------------------------------------------------

# (b) re-parse — writer emits syntax the parser does not accept.
KNOWN_REPARSE="
"

# (c) AST equality — output parses but denotes something else.
KNOWN_AST="
"

# (d) idempotency — no fixed point in one pass.
KNOWN_IDEMPOTENT="
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

# Nearest directory at or above $1 carrying a manifest (template
# counts), stopping below $ROOT. Empty when none.
manifest_dir_of() {
  d="$1"
  while [ "$d" != "$ROOT" ] && [ "$d" != "/" ]; do
    if [ -f "$d/kai.toml" ] || [ -f "$d/kai.toml.template" ]; then
      printf '%s' "$d"
      return 0
    fi
    d="$(dirname "$d")"
  done
}

# Dependency package dirs declared by the manifest in $1, one per line.
# `source`/`path`/`git` all name a dep's location; the git-fixture
# placeholders resolve to the in-repo source dirs the bare repos are
# built from, so no rendering step is required.
manifest_dep_dirs() {
  m="$1/kai.toml.template"
  [ -f "$m" ] || m="$1/kai.toml"
  [ -f "$m" ] || return 0
  { sed -n 's/.*source[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$m"
    sed -n 's/.*path[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$m"
    sed -n 's/.*git[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$m"; } |
  while read -r p; do
    case "$p" in
      GREET_BARE|GREET_PATH_PLACEHOLDER) echo "$GIT_FIX/greet" ;;
      UTIL_BARE)                         echo "$GIT_FIX/util" ;;
      /*) ;;
      *) (cd "$1" && cd "$p" 2>/dev/null && pwd) || true ;;
    esac
  done
}

# Extra `--path` flags a file needs beyond stdlib, mirroring what the
# driver's manifest_path_flags passes to kaic2: the manifest dir, its
# parent (for `import <pkgname>.<module>` from sibling dirs), and the
# transitive closure of declared deps (a consumer's dep may itself
# import its own dep, and the parse loads that module too).
#
# Two non-package families need one known search root each; the values
# mirror their own harnesses (stage2/Makefile test-modules-path,
# tools/independence-oracle.sh).
pkg_paths() {
  case "$1" in
    "$ROOT"/examples/packages/*)
      start="$(manifest_dir_of "$(dirname "$1")")"
      [ -n "$start" ] || return 0
      printf -- '--path %s --path %s ' "$start" "$(dirname "$start")"
      queue="$start"; seen=""
      while [ -n "$queue" ]; do
        d="${queue%% *}"
        rest="${queue#*"$d"}"; queue="${rest# }"
        case " $seen " in *" $d "*) continue ;; esac
        seen="$seen $d"
        for dep in $(manifest_dep_dirs "$d"); do
          case " $seen $queue " in *" $dep "*) ;; *)
            printf -- '--path %s ' "$dep"
            queue="${queue:+$queue }$dep" ;;
          esac
        done
      done
      ;;
    "$ROOT"/examples/modules-path/*)
      printf -- '--path %s ' "$ROOT/examples/modules-path-lib" ;;
    "$ROOT"/examples/oracle/*)
      d="$(dirname "$1")"
      if [ -d "$d/lib" ]; then printf -- '--path %s ' "$d/lib"; fi ;;
  esac
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

results="$tmp/results"      # one char per file: P|F|E|N|X|D|S|R
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
  extra="$(pkg_paths "$abs")"

  # A file the parser rejects is not a formatter subject, but WHY it
  # was rejected must stay auditable — a deliberate negative is fine,
  # a valid file the harness cannot resolve is unmeasured surface:
  #
  #   N  negative by design — an `.err.expected` golden (own name or a
  #      sibling's: modules-family negatives diagnose at the importing
  #      root, so their support modules fail as roots too and the
  #      golden carries the root's name), the `.err.kai` spelling, or
  #      the examples/negative/ tree.
  #   X  non-subject by design — fmt golden-suite inputs (measured by
  #      tests/fmt_fixtures.sh, deliberately not whole programs) and
  #      aspirational code that does not compile yet.
  #   D  coverage debt — everything else. FAILS the run: either the
  #      harness learns to resolve the file or it gets classified.
  if ! "$KAIC2" $KPATH $extra --ast "$abs" > "$b.ast0" 2> "$b.err"; then
    case "$rel" in
      *.err.kai)                printf 'N' >> "$results"; return 0 ;;
      examples/negative/*)      printf 'N' >> "$results"; return 0 ;;
      examples/fmt/*)           printf 'X' >> "$results"; return 0 ;;
      examples/aspirational/*)  printf 'X' >> "$results"; return 0 ;;
    esac
    for g in "$dir"/*.err.expected; do
      if [ -f "$g" ]; then
        printf 'N' >> "$results"
        return 0
      fi
    done
    { echo "  UNCOVERED $rel — valid-looking file the harness cannot evaluate:"
      sed 's/^/      /' "$b.err"; } >> "$failures"
    printf 'D' >> "$results"
    return 0
  fi

  # (a) fmt must not refuse.
  if ! "$KAIC2" $KPATH $extra --fmt "$abs" > "$p1" 2> "$b.err"; then
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
  if "$KAIC2" $KPATH $extra --ast "$p1" > "$b.ast1" 2> "$b.err"; then
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
  if ! "$KAIC2" $KPATH $extra --fmt "$p1" > "$p2" 2> "$b.err"; then
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
negative=$(tr -cd 'N' < "$results" | wc -c | tr -d ' ')
nonsubject=$(tr -cd 'X' < "$results" | wc -c | tr -d ' ')
uncovered=$(tr -cd 'D' < "$results" | wc -c | tr -d ' ')
refused=$(tr -cd 'R' < "$results" | wc -c | tr -d ' ')

cat "$failures"
echo "fmt_property: $pass passed, $excused excused (open issues), $refused refused (out of scope), $negative negative by design, $nonsubject non-subjects by design, $uncovered uncovered, $stale stale, $fail failed"

if [ "$stale" -gt 0 ]; then
  echo "fmt_property: an exception is listed for a file that now passes — the list must shrink to zero, not rot" >&2
  exit 1
fi
if [ "$uncovered" -gt 0 ]; then
  echo "fmt_property: a corpus file is neither measured nor excluded by design — teach pkg_paths to resolve it (or give it its negative golden) instead of letting coverage shrink silently" >&2
  exit 1
fi
if [ "$fail" -gt 0 ]; then
  exit 1
fi
