#!/bin/sh
# tests/fmt_fixtures.sh — exercise `kai fmt` against examples/fmt/.
#
# Two checks per fixture:
#   1. fmt(input.kai)    == expected.kai   (canonical output)
#   2. fmt(expected.kai) == expected.kai   (idempotency)
# Plus a roundtrip sanity check on all formattable examples — the
# formatted output must re-parse without errors.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

if [ ! -x "$KAIC2" ]; then
  echo "fmt_fixtures: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

fail=0
pass=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

for input in "$ROOT"/examples/fmt/*.input.kai; do
  name=$(basename "$input" .input.kai)
  expected="${input%.input.kai}.expected.kai"
  if [ ! -f "$expected" ]; then
    echo "  SKIP $name — missing $expected"
    continue
  fi
  if ! "$KAIC2" --fmt "$input" > "$tmp/out.kai" 2> "$tmp/err"; then
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
  if ! "$KAIC2" --fmt "$expected" > "$tmp/out2.kai" 2> "$tmp/err"; then
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
  if ! "$KAIC2" --fmt "$f" > "$tmp/rt.kai" 2> "$tmp/err"; then
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
  if ! "$KAIC2" --tokens "$tmp/rt.kai" > /dev/null 2> "$tmp/err"; then
    echo "  FAIL $name — formatted output failed to re-parse:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  if ! "$KAIC2" --fmt "$tmp/rt.kai" > "$tmp/rt2.kai" 2> "$tmp/err"; then
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
"$KAIC2" --fmt "$dsp/unformatted.kai" > "$tmp/canonical.kai"

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

# The package-directory case must still resolve: `kai fmt .` and
# `kai fmt ./<pkg>` route through resolve_package_spec to the manifest
# entry point and format it in place (exit 0, entry rewritten).
cp "$dsp/unformatted.kai" "$dsp/main.kai"
if ( cd "$dsp"; "$KAI" fmt . ) 2>"$tmp/dot.err" && diff -u "$tmp/canonical.kai" "$dsp/main.kai" > "$tmp/diff"; then
  echo "  OK   dot-package (fmt . resolves + formats the manifest entry)"
  pass=$((pass + 1))
else
  echo "  FAIL dot-package — fmt . did not resolve/format the entry:"
  sed 's/^/      /' "$tmp/dot.err"
  sed 's/^/      /' "$tmp/diff"
  fail=$((fail + 1))
fi
cp "$dsp/unformatted.kai" "$dsp/main.kai"
if ( cd "$tmp"; "$KAI" fmt ./dotslash-pkg ) 2>"$tmp/subpkg.err" && diff -u "$tmp/canonical.kai" "$dsp/main.kai" > "$tmp/diff"; then
  echo "  OK   subdir-package (fmt ./<pkg> resolves + formats the manifest entry)"
  pass=$((pass + 1))
else
  echo "  FAIL subdir-package — fmt ./<pkg> did not resolve/format the entry:"
  sed 's/^/      /' "$tmp/subpkg.err"
  sed 's/^/      /' "$tmp/diff"
  fail=$((fail + 1))
fi

if [ "$fail" -gt 0 ]; then
  echo "fmt_fixtures: $pass passed, $fail failed"
  exit 1
fi
echo "fmt_fixtures: $pass passed, 0 failed"
