#!/bin/sh
# tests/migrate_fixtures.sh — exercise `kai migrate` against
# examples/migrate/.
#
# Per fixture:
#   1. migrate(input.kai)    == expected.kai   (the rewrite)
#   2. migrate(expected.kai) == expected.kai   (idempotency)
#   3. the output re-parses                    (never emits non-parsing)
# A fixture with a <name>.report.expected file also asserts the
# stderr `manual:` report matches, proving un-migratable changes are
# reported, not silently dropped.
#
# The rewrite runs as `--edition hanga-roa` (the origin edition of the
# migration); the idempotency step re-runs it the way a user's second
# `kai migrate` reaches the compiler — over a package that the first
# run moved to orongo. That gate is what keeps the positional Result
# flip from undoing itself.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

if [ ! -x "$KAIC2" ]; then
  echo "migrate_fixtures: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

fail=0
pass=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

for input in "$ROOT"/examples/migrate/*.input.kai; do
  name=$(basename "$input" .input.kai)
  expected="${input%.input.kai}.expected.kai"
  report="${input%.input.kai}.report.expected"
  if [ ! -f "$expected" ]; then
    echo "  SKIP $name — missing $expected"
    continue
  fi

  # 1. rewrite matches the golden
  if ! "$KAIC2" --migrate --edition hanga-roa "$input" > "$tmp/out.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — migrate rejected input:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1)); continue
  fi
  if ! diff -u "$expected" "$tmp/out.kai" > "$tmp/diff"; then
    echo "  FAIL $name — output != expected:"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1)); continue
  fi

  # 2. idempotency: the second run sees a package the first run moved
  #    to orongo, and must be a byte-for-byte no-op.
  if ! "$KAIC2" --migrate --edition orongo "$expected" > "$tmp/out2.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — migrate rejected expected (idempotency):"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1)); continue
  fi
  if ! diff -u "$expected" "$tmp/out2.kai" > "$tmp/diff"; then
    echo "  FAIL $name — migrate(expected) != expected (idempotency):"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1)); continue
  fi

  # 3. the migrated output re-parses (never emits non-parsing source)
  if ! "$KAIC2" --tokens "$tmp/out.kai" > /dev/null 2> "$tmp/err"; then
    echo "  FAIL $name — migrated output failed to re-parse:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1)); continue
  fi

  # 4. optional: the `manual:` report matches the golden
  if [ -f "$report" ]; then
    "$KAIC2" --migrate "$input" > /dev/null 2> "$tmp/rep"
    if ! diff -u "$report" "$tmp/rep" > "$tmp/diff"; then
      echo "  FAIL $name — manual report != expected:"
      sed 's/^/      /' "$tmp/diff"
      fail=$((fail + 1)); continue
    fi
  fi

  echo "  OK   $name"
  pass=$((pass + 1))
done

# The edition gate itself: the Result flip is a hanga-roa -> orongo
# rewrite, so un-migrated Err-first source must pass through untouched
# when the declared edition is already orongo. Without this the flip
# would run on any input and could never converge.
# Compared against `--fmt` of the same source, not the raw file: both
# paths run the fmt-writer, so this isolates the rewrite from layout.
err_first="$ROOT/examples/migrate/result_ok_first.input.kai"
"$KAIC2" --fmt "$err_first" > "$tmp/orongo.want" 2> /dev/null
if "$KAIC2" --migrate --edition orongo "$err_first" > "$tmp/orongo.kai" 2> "$tmp/err"; then
  if diff -u "$tmp/orongo.want" "$tmp/orongo.kai" > "$tmp/diff"; then
    echo "  OK   edition-gate (no Result flip under orongo)"
    pass=$((pass + 1))
  else
    echo "  FAIL edition-gate — Result flip ran under orongo:"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1))
  fi
else
  echo "  FAIL edition-gate — migrate rejected --edition orongo:"
  sed 's/^/      /' "$tmp/err"
  fail=$((fail + 1))
fi

# Driver idempotency: the defect lived entirely in `bin/kai migrate`'s
# --from default-resolution, which the raw-kaic2 fixtures above never
# exercise. Run the real driver twice over a package directory whose
# kai.toml declares the origin edition, and assert the second --write is
# byte-stable. Run 1 flips to Ok-first and stamps orongo; run 2 must
# resolve --from off the stamped manifest and apply no rule.
KAI="$ROOT/bin/kai"
pkg="$tmp/driver-pkg"
mkdir -p "$pkg"
printf 'name = "repro"\nversion = "0.1.0"\nedition = "hanga-roa"\n' > "$pkg/kai.toml"
cat > "$pkg/main.kai" <<'EOF'
fn parse(s: String) : Result[String, Int] = Ok(1)

fn main() : Unit / Stdout = match parse("x") {
  Ok(n) -> println("ok")
  Err(e) -> println(e)
}
EOF
if "$KAI" migrate --write "$pkg/main.kai" > /dev/null 2>&1; then
  cp "$pkg/main.kai" "$tmp/driver-after1.kai"
  if "$KAI" migrate --write "$pkg/main.kai" > /dev/null 2>&1; then
    if diff -u "$tmp/driver-after1.kai" "$pkg/main.kai" > "$tmp/diff"; then
      echo "  OK   driver-idempotency (second --write is a no-op)"
      pass=$((pass + 1))
    else
      echo "  FAIL driver-idempotency — second --write flipped the file back:"
      sed 's/^/      /' "$tmp/diff"
      fail=$((fail + 1))
    fi
  else
    echo "  FAIL driver-idempotency — second migrate --write errored"
    fail=$((fail + 1))
  fi
else
  echo "  FAIL driver-idempotency — first migrate --write errored"
  fail=$((fail + 1))
fi

if [ "$fail" -gt 0 ]; then
  echo "migrate_fixtures: $pass passed, $fail failed"
  exit 1
fi
echo "migrate_fixtures: $pass passed, 0 failed"
