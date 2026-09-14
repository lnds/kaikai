#!/bin/sh
# examples/packages/library_report_modes — `kai typecheck` and `kai lint`
# over a package with no entry point. The failure shape this guards: the
# entry was resolved before the package's modules were, so a library died
# on the missing entry and could only be checked one file at a time.
# Fixtures are generated in a tmpdir so no committed package carries a
# deliberately ill-typed module.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "library_report_modes: FAIL — $1" >&2; exit 1; }

saw() { echo "$2" | grep -q "$1" || fail "$3"; }
missing() { echo "$2" | grep -q "$1" && fail "$3"; return 0; }

# Run `kai $@` in package dir $1; stdout+stderr on stdout, must succeed.
ok_run() {
  d="$1"; shift
  (cd "$TMP/$d" && "$KAI" "$@" 2>&1) || fail "'kai $*' failed in $d"
}

# Same, but the run must fail.
bad_run() {
  d="$1"; shift
  st=0
  out="$(cd "$TMP/$d" && "$KAI" "$@" 2>&1)" || st=$?
  [ "$st" -ne 0 ] || fail "'kai $*' unexpectedly succeeded in $d"
  printf '%s' "$out"
}

# A library: a manifest, two modules, and no entry file.
mkdir -p "$TMP/lib"
printf 'name = "lib"\nedition = "hanga-roa"\n' > "$TMP/lib/kai.toml"
cat > "$TMP/lib/ops.kai" <<'EOF'
#[doc("Doubles an Int.")]
pub fn doble(n: Int) : Int = n * 2
EOF
cat > "$TMP/lib/ops_test.kai" <<'EOF'
import ops

test "doubling" {
  assert ops.doble(7) == 14
}
EOF

# 1 — every spelling of the package form reaches every module instead of
# dying on the missing entry.
for spec in "" "."; do
  # shellcheck disable=SC2086
  out="$(ok_run lib typecheck $spec)"
  missing "does not exist" "$out" "'kai typecheck ${spec:-<bare>}' still reports the missing entry"
  saw "ops.kai" "$out" "'kai typecheck ${spec:-<bare>}' did not check ops.kai"
  saw "ops_test.kai" "$out" "'kai typecheck ${spec:-<bare>}' did not check ops_test.kai"
  # shellcheck disable=SC2086
  out="$(ok_run lib lint $spec)"
  missing "does not exist" "$out" "'kai lint ${spec:-<bare>}' still reports the missing entry"
  saw "ops.kai" "$out" "'kai lint ${spec:-<bare>}' did not lint ops.kai"
done

# 2 — the sub-package spelling resolves the same way.
out="$(ok_run . typecheck ./lib)"
saw "ops.kai" "$out" "'kai typecheck ./lib' did not check the sub-package's modules"
out="$(ok_run . lint ./lib)"
saw "ops.kai" "$out" "'kai lint ./lib' did not lint the sub-package's modules"

# 3 — a walk that looks at nothing is worse than an error: an ill-typed
# module the entry never reached must still fail typecheck.
cat > "$TMP/lib/broken.kai" <<'EOF'
pub fn oops(n: Int) : String = n
EOF
out="$(bad_run lib typecheck .)"
saw "type mismatch" "$out" "'kai typecheck .' passed over an ill-typed module"

# 4 — a lint never blocks, not even over a package that does not compile.
out="$(ok_run lib lint .)"
saw "type mismatch" "$out" "'kai lint .' hid the front-end error"
rm -f "$TMP/lib/broken.kai"

# 5 — a lint finding in a module no entry reaches is reported.
cat > "$TMP/lib/smelly.kai" <<'EOF'
fn unused_helper(n: Int) : Int = n * 3

pub fn run() : Int = 0
EOF
out="$(ok_run lib lint .)"
saw "dead_code_unused_priv" "$out" "'kai lint .' did not report a finding in an unreachable module"
rm -f "$TMP/lib/smelly.kai"

# 6 — a package WITH an entry keeps resolving through the entry alone.
mkdir -p "$TMP/app"
printf 'name = "app"\nedition = "hanga-roa"\n' > "$TMP/app/kai.toml"
printf 'fn main() : Int = 0\n' > "$TMP/app/main.kai"
out="$(ok_run app typecheck .)"
saw "entry point: main.kai" "$out" "'kai typecheck .' stopped resolving the entry of a binary package"
out="$(ok_run app lint .)"
saw "entry point: main.kai" "$out" "'kai lint .' stopped resolving the entry of a binary package"

# 7 — a directory with no manifest and no entry still errors: the fix
# widens what a package reaches, it does not make every directory a
# target.
mkdir -p "$TMP/bare"
out="$(bad_run bare typecheck .)"
saw "missing input file" "$out" "'kai typecheck .' lost its usage error outside a package"
bad_run bare lint . >/dev/null

# 8 — file mode is untouched.
out="$(ok_run lib typecheck ops.kai)"
missing "does not exist" "$out" "'kai typecheck <file>' regressed"

echo "library_report_modes: ok"
