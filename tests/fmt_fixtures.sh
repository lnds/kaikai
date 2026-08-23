#!/bin/sh
# tests/fmt_fixtures.sh — exercise `kai fmt` against examples/fmt/.
#
# Two checks per fixture:
#   1. fmt(input.kai)    == expected.kai   (canonical output)
#   2. fmt(expected.kai) == expected.kai   (idempotency)
# Plus a roundtrip sanity check on all formattable examples — the
# formatted output must re-parse without errors.
#
# examples/fmt/width/ rides the same two checks: the width corpus's
# goldens pin the layout the width model produces, and a change in
# layout policy shows up here as a reviewable diff; the numbers behind
# them are gated by tests/fmt_width.sh.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; fmt reproduces the surface
# of the edition the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"

if [ ! -x "$KAIC2" ]; then
  echo "fmt_fixtures: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

fail=0
pass=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

for input in "$ROOT"/examples/fmt/*.input.kai "$ROOT"/examples/fmt/width/*.input.kai; do
  name=$(basename "$input" .input.kai)
  expected="${input%.input.kai}.expected.kai"
  if [ ! -f "$expected" ]; then
    echo "  SKIP $name — missing $expected"
    continue
  fi
  if ! "$KAIC2" $EDITION_FLAG --fmt "$input" > "$tmp/out.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — fmt rejected input:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  if ! diff -u "$expected" "$tmp/out.kai" > "$tmp/diff"; then
    echo "  FAIL $name — output != expected:"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1))
    continue
  fi
  if ! "$KAIC2" $EDITION_FLAG --fmt "$expected" > "$tmp/out2.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — fmt rejected expected (idempotency):"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  if ! diff -u "$expected" "$tmp/out2.kai" > "$tmp/diff"; then
    echo "  FAIL $name — fmt(expected) != expected (idempotency):"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1))
    continue
  fi
  echo "  OK   $name"
  pass=$((pass + 1))
done

# Roundtrip on the examples/minimal/ corpus: every file the formatter
# accepts must produce output that parses back without error. Catches
# silent grammar-side regressions like the (1..100) round-trip break.
for f in "$ROOT"/examples/minimal/*.kai \
         "$ROOT"/examples/quickstart/01_hello.kai \
         "$ROOT"/examples/quickstart/02_fizzbuzz.kai \
         "$ROOT"/examples/quickstart/03_calculator.kai \
         "$ROOT"/examples/phase4/*.kai; do
  [ -f "$f" ] || continue
  name=$(basename "$f")
  if ! "$KAIC2" $EDITION_FLAG --fmt "$f" > "$tmp/rt.kai" 2> "$tmp/err"; then
    # Unsupported constructs are reported via stderr + exit 1; that is
    # an explicit refusal, not a failure of the formatter.
    if grep -q "kai fmt:" "$tmp/err"; then
      echo "  SKIP $name — unsupported subset (expected)"
      continue
    fi
    echo "  FAIL $name — fmt errored unexpectedly:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  # Re-parse: if the formatter's output cannot be parsed, that is
  # a fatal silent breakage of the round-trip invariant.
  if ! "$KAIC2" $EDITION_FLAG --tokens "$tmp/rt.kai" > /dev/null 2> "$tmp/err"; then
    echo "  FAIL $name — formatted output failed to re-parse:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  if ! "$KAIC2" $EDITION_FLAG --fmt "$tmp/rt.kai" > "$tmp/rt2.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — fmt(fmt) errored:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  if ! diff -u "$tmp/rt.kai" "$tmp/rt2.kai" > "$tmp/diff"; then
    echo "  FAIL $name — fmt is not idempotent:"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1))
    continue
  fi
  echo "  OK   $name (roundtrip)"
  pass=$((pass + 1))
done

# Argument-routing regression: `kai fmt ./main.kai` must be treated as a
# FILE, not a package spec — a `./`-prefixed file path routed through
# resolve_package_spec died with "is not a directory", breaking the
# obvious `find . -exec kai fmt {}` sweep. These checks drive the shell
# driver (bin/kai), where the routing lives, not kaic2 directly.
KAI="$ROOT/bin/kai"
dsp="$tmp/dotslash-pkg"
mkdir -p "$dsp"
printf 'name = "repro"\nversion = "0.1.0"\nedition = "hanga-roa"\n' > "$dsp/kai.toml"
cat > "$dsp/unformatted.kai" <<'EOF'
fn   main( ) : Unit / Stdout   =   println( "hi" )
EOF
# The canonical formatting the driver should apply, independent of path.
"$KAIC2" $EDITION_FLAG --fmt "$dsp/unformatted.kai" > "$tmp/canonical.kai"

# In-place fmt over `./main.kai` must apply the same rewrite as the bare
# filename does over `main.kai` — no "is not a directory" death.
cp "$dsp/unformatted.kai" "$tmp/bare.kai"
cp "$dsp/unformatted.kai" "$tmp/dotslash.kai"
( cd "$tmp"; "$KAI" fmt bare.kai 2>"$tmp/bare.err"; "$KAI" fmt ./dotslash.kai 2>"$tmp/dotslash.err" )
if diff -u "$tmp/bare.kai" "$tmp/dotslash.kai" > "$tmp/diff" \
   && diff -u "$tmp/canonical.kai" "$tmp/dotslash.kai" >> "$tmp/diff"; then
  echo "  OK   dotslash-file (fmt ./main.kai == fmt main.kai)"
  pass=$((pass + 1))
else
  echo "  FAIL dotslash-file — ./main.kai != main.kai (or != canonical):"
  sed 's/^/      /' "$tmp/diff"
  sed 's/^/      /' "$tmp/dotslash.err"
  fail=$((fail + 1))
fi

# The package-directory case formats the whole package: `kai fmt .`
# and `kai fmt ./<pkg>` rewrite every .kai file the package owns, not
# just the resolved entry point (a sibling left mangled while fmt
# exits 0 is the #1849 regression).
cp "$dsp/unformatted.kai" "$dsp/main.kai"
if ( cd "$dsp"; "$KAI" fmt . ) 2>"$tmp/dot.err" \
   && diff -u "$tmp/canonical.kai" "$dsp/main.kai" > "$tmp/diff" \
   && diff -u "$tmp/canonical.kai" "$dsp/unformatted.kai" >> "$tmp/diff"; then
  echo "  OK   dot-package (fmt . formats every package file)"
  pass=$((pass + 1))
else
  echo "  FAIL dot-package — fmt . left a package file unformatted:"
  sed 's/^/      /' "$tmp/dot.err"
  sed 's/^/      /' "$tmp/diff"
  fail=$((fail + 1))
fi
cp "$dsp/unformatted.kai" "$dsp/main.kai"
cp "$dsp/unformatted.kai" "$dsp/sibling.kai"
if ( cd "$tmp"; "$KAI" fmt ./dotslash-pkg ) 2>"$tmp/subpkg.err" \
   && diff -u "$tmp/canonical.kai" "$dsp/main.kai" > "$tmp/diff" \
   && diff -u "$tmp/canonical.kai" "$dsp/sibling.kai" >> "$tmp/diff"; then
  echo "  OK   subdir-package (fmt ./<pkg> formats every package file)"
  pass=$((pass + 1))
else
  echo "  FAIL subdir-package — fmt ./<pkg> left a package file unformatted:"
  sed 's/^/      /' "$tmp/subpkg.err"
  sed 's/^/      /' "$tmp/diff"
  fail=$((fail + 1))
fi

# Package-mode --check: exit 1 listing the files that would change;
# exit 0 (silent) when the whole package is canonical. Drives the
# driver-level comparison too — a canonical file must check clean, the
# trailing-newline comparison trap fixed alongside #1849.
printf 'fn  main( ) =  ()\n' > "$dsp/sibling.kai"
check_rc=0
( cd "$dsp"; "$KAI" fmt --check . >"$tmp/check.out" 2>&1 ) || check_rc=$?
if [ "$check_rc" -eq 1 ] && [ "$(cat "$tmp/check.out")" = "sibling.kai" ]; then
  echo "  OK   package-check-dirty (lists only the unformatted file, exit 1)"
  pass=$((pass + 1))
else
  echo "  FAIL package-check-dirty — rc=$check_rc out=$(cat "$tmp/check.out")"
  fail=$((fail + 1))
fi
"$KAI" fmt "$dsp/sibling.kai" 2>/dev/null
check_rc=0
( cd "$dsp"; "$KAI" fmt --check . >"$tmp/check.out" 2>&1 ) || check_rc=$?
if [ "$check_rc" -eq 0 ] && [ ! -s "$tmp/check.out" ]; then
  echo "  OK   package-check-clean (exit 0, silent, canonical package)"
  pass=$((pass + 1))
else
  echo "  FAIL package-check-clean — rc=$check_rc out=$(cat "$tmp/check.out")"
  fail=$((fail + 1))
fi

# Single-file --check round-trip: a file just rewritten by `kai fmt`
# checks clean (rc 0); a non-canonical one checks dirty (rc 1) and the
# canonical text goes to stdout.
printf 'fn  main( ) =  ()\n' > "$tmp/single.kai"
"$KAI" fmt "$tmp/single.kai" 2>/dev/null
check_rc=0
"$KAI" fmt --check "$tmp/single.kai" >"$tmp/check.out" 2>&1 || check_rc=$?
if [ "$check_rc" -eq 0 ]; then
  echo "  OK   file-check-clean (fmt then --check exits 0)"
  pass=$((pass + 1))
else
  echo "  FAIL file-check-clean — rc=$check_rc out=$(cat "$tmp/check.out")"
  fail=$((fail + 1))
fi

if [ "$fail" -gt 0 ]; then
  echo "fmt_fixtures: $pass passed, $fail failed"
  exit 1
fi
echo "fmt_fixtures: $pass passed, 0 failed"
