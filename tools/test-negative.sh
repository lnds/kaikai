#!/bin/sh
# Negative-space test runner (issue #511).
#
# For every negative fixture under examples/negative/**, run kaic2
# (the stage 2 compiler) and assert two things:
#   1. exit code is non-zero (compile-time rejection)
#   2. stderr contains the substring stored in the sibling
#      .err.expected golden (first line, like the existing
#      test-modules-qualified-neg pattern in stage2/Makefile).
#
# Layout conventions:
#
#   Single-file fixture:
#     examples/negative/<category>/<name>.kai
#     examples/negative/<category>/<name>.err.expected
#
#   Single-file fixture loaded as prelude:
#     examples/negative/<category>/<name>.kai
#     examples/negative/<category>/<name>.err.expected
#     examples/negative/<category>/<name>.prelude.kai   (optional sibling)
#
#   Multi-file fixture:
#     examples/negative/<category>/<name>/main.kai
#     examples/negative/<category>/<name>/lib.kai        (or any siblings)
#     examples/negative/<category>/<name>/main.err.expected
#
# Single-file fixtures whose `.err.expected` ends in `.kaic1.err.expected`
# are routed to stage1/kaic1 instead — used to assert clean stage 1
# rejection of stage-2-only features (e.g. protocol impls).
#
# Each fixture run also produces one line of audit output:
#   PASS <fixture>  — language rejects with the expected diagnostic.
#   FAIL <fixture>  — language accepts (silent contract; needs follow-up).
#   MISS <fixture>  — language rejects but with a different diagnostic.
# The script exits non-zero if any fixture FAILs or MISSes.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

KAIC2="$ROOT/stage2/kaic2"
KAIC1="$ROOT/stage1/kaic1"

if [ ! -x "$KAIC2" ]; then
  echo "test-negative FAIL — stage2/kaic2 not built (run 'make kaic2' first)"
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

pass=0
fail=0
miss=0
total=0

run_one() {
  src="$1"
  exp="$2"
  compiler="$3"        # kaic2 or kaic1
  prelude="$4"          # path or empty
  extra_flags="$5"      # raw flag string or empty
  multi_line="$6"       # "1" → match every non-empty line; otherwise first line

  total=$((total + 1))
  rel="${src#$ROOT/}"

  errfile="$tmp/$(echo "$rel" | tr '/' '_').err"
  rc=0
  # shellcheck disable=SC2086 — extra_flags is intentionally word-split.
  # Hanga Roa: core is loaded automatically by the compiler. The
  # legacy `--prelude` flag was retired; callers wanting an
  # additional prelude (which the runner used to support per-fixture)
  # now have to add an `import` to the fixture source instead.
  "$compiler" $extra_flags "$src" > /dev/null 2> "$errfile" || rc=$?

  if [ "$rc" -eq 0 ]; then
    echo "FAIL $rel — exit 0 (silent contract; expected non-zero)"
    fail=$((fail + 1))
    return
  fi

  if [ "$multi_line" = "1" ]; then
    # Every non-empty, non-comment line in the golden must appear
    # somewhere in stderr (substring match, fixed-string). Order
    # is not enforced — kaikai diagnostics can re-order notes
    # when multiple errors fire.
    missing=""
    while IFS= read -r line; do
      case "$line" in
        ""|"#"*) continue ;;
      esac
      if ! grep -qF -- "$line" "$errfile"; then
        missing="$missing\n  want: $line"
      fi
    done < "$exp"
    if [ -z "$missing" ]; then
      echo "PASS $rel"
      pass=$((pass + 1))
    else
      printf 'MISS %s — diagnostic body mismatch%b\n' "$rel" "$missing"
      miss=$((miss + 1))
    fi
    return
  fi

  needle=$(head -1 "$exp")
  if [ -z "$needle" ]; then
    echo "MISS $rel — .err.expected first line is empty"
    miss=$((miss + 1))
    return
  fi

  if grep -qF "$needle" "$errfile"; then
    echo "PASS $rel"
    pass=$((pass + 1))
  else
    echo "MISS $rel — diagnostic mismatch"
    echo "  want: $needle"
    echo "  got : $(head -1 "$errfile")"
    miss=$((miss + 1))
  fi
}

# Find all .err.expected / .diag.expected goldens and pair them with
# their .kai sources.
# Multi-file: <dir>/main.err.expected pairs with <dir>/main.kai.
# Single-file: <name>.err.expected pairs with <name>.kai.
# Stage 1 routing: <name>.kaic1.err.expected pairs with <name>.kai using kaic1.
# Multi-line shape: <name>.diag.expected — every non-empty,
# non-`#`-prefixed line in the golden must appear (as a fixed
# substring) somewhere in stderr. Used to assert diagnostic body
# quality (anchor, caret, notes, help) per #520 category 7.

# Skip the `silent_contract/` subtree — those fixtures DOCUMENT a
# gap the language has not yet closed, and live alongside a follow-up
# issue link in a sibling `.silent_contract.md` note. Once the gap
# closes, the fixture moves out of `silent_contract/` and the
# `.err.expected` golden lands in this run.
find "$ROOT/examples/negative" \( -name '*.err.expected' -o -name '*.diag.expected' \) -not -name '*.run.err.expected' -not -path '*/silent_contract/*' 2>/dev/null | sort | while read -r exp; do
  base=$(basename "$exp")
  dir=$(dirname "$exp")
  multi_line=0

  case "$base" in
    main.err.expected)
      src="$dir/main.kai"
      stem="main"
      compiler="$KAIC2"
      ;;
    *.kaic1.err.expected)
      stem="${base%.kaic1.err.expected}"
      src="$dir/$stem.kai"
      compiler="$KAIC1"
      ;;
    *.diag.expected)
      stem="${base%.diag.expected}"
      src="$dir/$stem.kai"
      compiler="$KAIC2"
      multi_line=1
      ;;
    *.err.expected)
      stem="${base%.err.expected}"
      src="$dir/$stem.kai"
      compiler="$KAIC2"
      ;;
  esac

  if [ ! -f "$src" ]; then
    echo "MISS $exp — source file not found ($src)"
    continue
  fi

  # Optional sibling overrides.
  prelude=""
  if [ -f "$dir/$stem.prelude.kai" ]; then
    prelude="$dir/$stem.prelude.kai"
  fi

  extra_flags=""
  if [ -f "$dir/$stem.flags" ]; then
    extra_flags=$(cat "$dir/$stem.flags")
  fi

  run_one "$src" "$exp" "$compiler" "$prelude" "$extra_flags" "$multi_line"
done > "$tmp/results"
cat "$tmp/results"

# Runtime-time negative tests: fixtures that the typer accepts by
# design (e.g. resume-twice is a runtime panic, not a static error
# per docs/effects-impl.md). Pair each `<name>.kai` with
# `<name>.run.err.expected` — we compile via kaic2 → C → cc, run
# the resulting binary, expect non-zero exit, and grep stderr for
# the golden substring.

find "$ROOT/examples/negative" -name '*.run.err.expected' -not -path '*/silent_contract/*' 2>/dev/null | sort | while read -r exp; do
  base=$(basename "$exp")
  dir=$(dirname "$exp")
  stem="${base%.run.err.expected}"
  src="$dir/$stem.kai"

  if [ ! -f "$src" ]; then
    echo "MISS $exp — source file not found ($src)"
    continue
  fi

  rel="${src#$ROOT/}"
  cfile="$tmp/$(echo "$rel" | tr '/' '_').c"
  bin="$tmp/$(echo "$rel" | tr '/' '_').bin"
  errfile="$tmp/$(echo "$rel" | tr '/' '_').runerr"

  if ! "$KAIC2" "$src" > "$cfile" 2> "$errfile.compile"; then
    echo "FAIL $rel — kaic2 rejected at compile (this is a runtime-negative fixture)"
    continue
  fi
  if ! cc -std=c99 -Wall -Wno-incompatible-function-pointer-types -I "$ROOT/stage0" "$cfile" -o "$bin" -lm 2> "$errfile.cc"; then
    echo "FAIL $rel — cc rejected the generated C (likely silent type-level contract)"
    cat "$errfile.cc" | head -3
    continue
  fi

  rc=0
  "$bin" > /dev/null 2> "$errfile" || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "FAIL $rel — binary exit 0 (expected runtime panic)"
    continue
  fi

  needle=$(head -1 "$exp")
  if grep -qF "$needle" "$errfile"; then
    echo "PASS $rel (runtime)"
  else
    echo "MISS $rel (runtime) — diagnostic mismatch"
    echo "  want: $needle"
    echo "  got : $(head -1 "$errfile")"
  fi
done >> "$tmp/results.runtime"

if [ -f "$tmp/results.runtime" ]; then
  cat "$tmp/results.runtime"
  cat "$tmp/results.runtime" >> "$tmp/results"
fi

# subshell breaks our counters; recompute from results file.
pass=$(grep -c '^PASS ' "$tmp/results" 2>/dev/null || true)
fail=$(grep -c '^FAIL ' "$tmp/results" 2>/dev/null || true)
miss=$(grep -c '^MISS ' "$tmp/results" 2>/dev/null || true)
total=$((pass + fail + miss))

echo
echo "test-negative summary: $pass PASS, $fail FAIL, $miss MISS (total $total)"

if [ "$fail" -ne 0 ] || [ "$miss" -ne 0 ]; then
  exit 1
fi
