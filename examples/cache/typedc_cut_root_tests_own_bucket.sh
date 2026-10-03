#!/bin/sh
# A root's own decls are typed in the root's bucket, never in the bucket
# of the module it imports last.
#
# A test file that declares only `test` blocks has no root fn to start
# its bucket. If its tests are filed by position, they join the bucket
# of the last module loaded, whose typed blob then differs per test file:
# every test file re-infers that module and publishes another copy of
# it. Filed by origin, the module's blob is shared and the second test
# file publishes only its own tests.
#
# The gate: the blobs the second test file publishes carry none of the
# module's declarations.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/shared_lib.kai" <<'EOF'
#[doc("The value the tests read.")]
pub fn shared_lib_value() : Int = shared_lib_unread_marker() - 1

fn shared_lib_unread_marker() : Int = 42
EOF
for t in first second; do
  cat > "$PROJ/${t}_test.kai" <<EOF
import shared_lib

test "$t" {
  assert shared_lib.shared_lib_value() == 41
}
EOF
done

mkdir -p "$PROJ/.kai-cache"

check() {
  if ! "$KAIC2" $EDITION_FLAG --user-cache --path "$PROJ" --test --check "$PROJ/$1.kai" > /dev/null 2>"$PROJ/err"; then
    echo "typedc_cut_root_tests_own_bucket: FAIL — $1 did not check"
    cat "$PROJ/err"
    exit 1
  fi
}

blobs() { ls "$PROJ/.kai-cache"/tm-*.kab 2>/dev/null | sort; }

check first_test
blobs > "$PROJ/after_first"
if [ ! -s "$PROJ/after_first" ]; then
  echo "typedc_cut_root_tests_own_bucket: FAIL — no typed blob published (cut inactive?)"
  exit 1
fi
check second_test
blobs > "$PROJ/after_second"

for b in $(comm -13 "$PROJ/after_first" "$PROJ/after_second"); do
  if grep -aq shared_lib_unread_marker "$b"; then
    echo "typedc_cut_root_tests_own_bucket: FAIL — the second test file republished the imported module's decls ($(basename "$b"))"
    exit 1
  fi
done

echo "typedc_cut_root_tests_own_bucket: OK — a tests-only root publishes only its own bucket"
exit 0
