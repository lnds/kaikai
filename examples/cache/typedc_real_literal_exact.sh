#!/bin/sh
# A Real literal is the same number on a package's first build and on every
# build after it, in the entry and in a module it imports, on each backend.
#
# The first build types the modules from source; a later one restores them
# from the typed blobs the first left. A blob that writes a literal's value
# shorter than it is hands the second build another number, so the gate runs
# the package, then tests it, then runs it again, and tests a fresh copy
# alone. It then edits one literal past its sixth digit: the build after the
# edit must see the new value, not an object cached for the old one.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

fail() { echo "typedc_real_literal_exact FAIL — $1"; exit 1; }

native_capable=0
if [ -x "$KAIC2" ]; then
  printf 'fn main() : Unit = ()\n' > "$PROJ/probe.kai"
  "$KAIC2" $EDITION_FLAG --emit=native --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/probe.kai" \
    >/dev/null 2>"$PROJ/native-probe.err" || true
  grep -q "not built into this compiler" "$PROJ/native-probe.err" 2>/dev/null || native_capable=1
  rm -f "$PROJ/probe.o"
fi

# The package of one module, its entry and its test file, with `odd`'s literal as $2.
package() {
  mkdir -p "$1/src/m" "$1/src/tests"
  printf '[package]\nname = "lit"\nversion = "0.0.0"\nentry = "src/main.kai"\n' > "$1/kai.toml"
  cat > "$1/src/m/other.kai" <<SRC
pub fn big() : Real = 1.7976931348623157e308

pub fn odd() : Real = $2
SRC
  cat > "$1/src/main.kai" <<'SRC'
import src.m.other

fn main() : Unit / Stdout = {
  let here = 1.7976931348623157e308
  Stdout.print("#{big() == here} #{big() == 1.79769e308} #{here == 1.79769e308} #{real_to_int(odd() - 9007199254740000.0)}")
}
SRC
  cat > "$1/src/tests/other_test.kai" <<'SRC'
import src.m.other

test "the literal of another module" {
  assert big() == 1.7976931348623157e308
}

test "not its six digits" {
  assert big() != 1.79769e308
}

test "past 2^53" {
  assert odd() - 9007199254740000.0 == 992.0
}

test "local" {
  let a = 1.7976931348623157e308
  assert a != 1.79769e308
}
SRC
}

# Every build compiles: the binary memo would hand back the first build's.
run() {
  out="$(cd "$1" && env $ENVS KAI_BIN_MEMO=0 "$KAI" run src/main.kai 2>"$PROJ/err")" \
    || { cat "$PROJ/err"; fail "$NAME: kai run errored ($2)"; }
  [ "$out" = "$3" ] || fail "$NAME: $2 printed '$out', want '$3'"
}

tests() {
  (cd "$1" && env $ENVS KAI_BIN_MEMO=0 "$KAI" test src/tests/other_test.kai > "$PROJ/test.out" 2>&1) \
    || { cat "$PROJ/test.out"; fail "$NAME: kai test failed ($2)"; }
}

for spec in "c:KAI_BACKEND=c KAI_MODULAR=0" "c-modular:KAI_BACKEND=c KAI_MODULAR=1" \
            "native:KAI_BACKEND=native KAI_NATIVE_MODULAR=0" \
            "native-modular:KAI_BACKEND=native KAI_NATIVE_MODULAR=1"; do
  NAME="${spec%%:*}"; ENVS="${spec#*:}"
  case "$NAME" in native*)
    if [ "$native_capable" != "1" ]; then
      echo "typedc_real_literal_exact SKIP ($NAME: kaic2 has no libLLVM native backend)"
      continue
    fi ;;
  esac
  export KAI_NATIVE_MODULAR_CACHE_DIR="$PROJ/$NAME-objs" KAI_MODULAR_CACHE_DIR="$PROJ/$NAME-objs" \
         KAI_CORE_CACHE_DIR="$PROJ/$NAME-core"

  package "$PROJ/$NAME-a" 9007199254740993.0
  run "$PROJ/$NAME-a" "the first run" "true false false 992"
  tests "$PROJ/$NAME-a" "after a run"
  run "$PROJ/$NAME-a" "the run after the tests" "true false false 992"

  package "$PROJ/$NAME-b" 9007199254740993.0
  tests "$PROJ/$NAME-b" "on a fresh package"
  tests "$PROJ/$NAME-b" "a second time"
  run "$PROJ/$NAME-b" "the run after two test runs" "true false false 992"

  package "$PROJ/$NAME-a" 9007199254741993.0
  run "$PROJ/$NAME-a" "the run after the literal's edit" "true false false 1992"
done

echo "typedc_real_literal_exact: OK"
