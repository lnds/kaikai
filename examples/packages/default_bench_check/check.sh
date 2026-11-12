#!/bin/sh
# examples/packages/default_bench_check — a `bench`, a `check` and a `test` entry
# (the last on both backends) install a kaikai-bodied default the way `main` does.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

out="$(cd "$DIR" && "$KAI" bench tests/bench_test.kai 2>&1)" || {
  echo "default_bench_check: FAIL — $out" >&2
  exit 1
}
echo "$out" | grep -q "1 benches" || { echo "default_bench_check: FAIL — $out" >&2; exit 1; }

out="$(cd "$DIR" && "$KAI" check tests/check_test.kai 2>&1)" || {
  echo "default_bench_check: FAIL — $out" >&2
  exit 1
}
echo "$out" | grep -q "1/1 checks passed" || { echo "default_bench_check: FAIL — $out" >&2; exit 1; }

for backend in c native; do
  out="$(cd "$DIR" && "$KAI" test --backend=$backend tests/test_entry_test.kai 2>&1)" || {
    echo "default_bench_check: FAIL ($backend) — $out" >&2
    exit 1
  }
  echo "$out" | grep -q "1/1 tests passed" || { echo "default_bench_check: FAIL ($backend) — $out" >&2; exit 1; }
done
