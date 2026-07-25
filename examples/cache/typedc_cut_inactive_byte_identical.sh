#!/bin/sh
# The per-module typed cut is wired through `tcut_infer_program` but held
# INACTIVE (empty cache dir → uncut fold). This fixture is the guard that
# it stays inactive AND lossless: a `--user-cache` build of a multi-module
# effectful package must emit C byte-identical to a plain build, and must
# not panic.
#
# Why it matters: restoring a module's typed blob is faithful as bytes
# (the codec round-trips exactly — see typedc_iface_hash_gates.sh), but a
# restored `[Decl]`'s heap layout is mis-read by the stage1 bootstrap
# compiler when a downstream pass walks it, tripping a `non-exhaustive
# match`. Until that stage1 fault is fixed, the cut must route through the
# uncut path. This fixture fails loudly if the cut is ever re-activated
# without the fix (a diverging or panicking `--user-cache` build).

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/a.kai" <<'EOF'
pub fn shout(s: String) : Unit / Stdout = print(s)
EOF
cat > "$PROJ/b.kai" <<'EOF'
import a
pub fn twice(s: String) : Unit / Stdout = { a.shout(s) a.shout(s) }
EOF
cat > "$PROJ/main.kai" <<'EOF'
import a
import b
fn main() : Unit / Console { b.twice("hi") }
EOF

mkdir -p "$PROJ/.kai-cache"

# Plain build (oracle).
"$KAIC2" --path "$PROJ" "$PROJ/main.kai" > "$PROJ/plain.c" 2>/dev/null

# --user-cache build: the cut is wired but inactive, so this must match
# the oracle byte-for-byte and never panic.
"$KAIC2" --user-cache --path "$PROJ" "$PROJ/main.kai" > "$PROJ/uc.c" 2>"$PROJ/uc.err" || {
  echo "typedc_cut_inactive_byte_identical: FAIL — --user-cache build exited non-zero"
  cat "$PROJ/uc.err"
  exit 1
}

# Only stderr signals a compiler-runtime panic; the emitted C legitimately
# contains `kai_core_panic("non-exhaustive match")` as a match default.
if grep -q panic "$PROJ/uc.err" 2>/dev/null; then
  echo "typedc_cut_inactive_byte_identical: FAIL — --user-cache build panicked (cut re-activated?)"
  cat "$PROJ/uc.err"
  exit 1
fi

if ! cmp -s "$PROJ/uc.c" "$PROJ/plain.c"; then
  echo "typedc_cut_inactive_byte_identical: FAIL — --user-cache C differs from plain C"
  diff "$PROJ/plain.c" "$PROJ/uc.c" | head -20
  exit 1
fi

echo "typedc_cut_inactive_byte_identical: OK — cut inactive, --user-cache == plain, no panic"
exit 0
