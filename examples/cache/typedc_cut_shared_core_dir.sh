#!/bin/sh
# The per-module typed cut activated by the SHARED core-cache directory,
# with no `--user-cache`. This is the path every `kai typecheck` and
# every default build takes: `bin/kai` always passes
# `--core-cache-dir`/`--toolchain-id`, so the cut rides that directory
# and the stdlib core is inferred once per toolchain instead of once per
# invocation.
#
# The cut's blobs are content-addressed by a key folding the module's
# decls, its inherited interface hash and the cross-module context, so
# sharing a directory across projects is safe. This fixture is the gate
# on that claim. Serving a stale typed interface is a correctness bug
# worse than having no cache, so each property below is hard.
#
# 1. cold == warm == plain. A module restored from the shared directory
#    feeds monomorph exactly what a fresh infer would.
# 2. The cut actually cuts — blobs land in the shared directory when
#    only `--core-cache-dir` is passed.
# 3. Two projects that declare the same module name with DIFFERENT
#    bodies must not serve each other's blob out of the one directory.
# 4. `--check` agrees with a full build about acceptance, and a type
#    error is still reported when the cache is warm.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
STDLIB="$ROOT/stdlib"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

CC="$WORK/coredir"
mkdir -p "$CC"
# The real toolchain id bin/kai derives; any stable string works here.
TID="fixture-$(cksum < "$KAIC2" | cut -d' ' -f1)"

PROJ="$WORK/p1"
mkdir -p "$PROJ"

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

fail() {
  echo "typedc_cut_shared_core_dir: FAIL — $1"
  shift
  [ $# -gt 0 ] && "$@"
  exit 1
}

# $1: output file, $2: project dir, rest: extra flags
build() {
  out="$1"; proj="$2"; shift 2
  if ! "$KAIC2" "$@" --path "$STDLIB" --path "$proj" "$proj/main.kai" \
       > "$out" 2>"$WORK/err"; then
    cat "$WORK/err"
    fail "build exited non-zero ($*)"
  fi
  # Only stderr signals a compiler-runtime panic; the emitted C
  # legitimately contains kai_core_panic as a match default.
  if grep -q panic "$WORK/err" 2>/dev/null; then
    cat "$WORK/err"
    fail "build panicked ($*)"
  fi
}

shared() { echo "--core-cache-dir $CC --toolchain-id $TID"; }

blob_count() { ls "$CC"/tm-*.kab 2>/dev/null | wc -l | tr -d ' '; }

same() {
  cmp -s "$1" "$2" || fail "$3" diff "$1" "$2"
}

# ---- 1 + 2: cold == warm == plain, and the cut publishes -------------

build "$WORK/plain.c" "$PROJ"
# shellcheck disable=SC2046
build "$WORK/cold.c" "$PROJ" $(shared)
cold_blobs="$(blob_count)"
# shellcheck disable=SC2046
build "$WORK/warm.c" "$PROJ" $(shared)

same "$WORK/plain.c" "$WORK/cold.c" "cold shared-dir C differs from plain C"
same "$WORK/plain.c" "$WORK/warm.c" "warm shared-dir C differs from plain C"

if [ "$cold_blobs" -lt 2 ]; then
  fail "cut published $cold_blobs typed blobs into the shared dir (cut inactive?)"
fi

# A body-only edit re-infers exactly its own module: one new blob.
cat > "$PROJ/b.kai" <<'EOF'
import a
pub fn twice(s: String) : Unit / Stdout = { a.shout(s) a.shout(s) a.shout(s) }
EOF
# shellcheck disable=SC2046
build "$WORK/edit.c" "$PROJ" $(shared)
build "$WORK/edit_plain.c" "$PROJ"
same "$WORK/edit_plain.c" "$WORK/edit.c" "body-edit shared-dir C differs from plain C"

edit_blobs="$(blob_count)"
if [ "$edit_blobs" -ne $((cold_blobs + 1)) ]; then
  fail "body edit published $((edit_blobs - cold_blobs)) blobs, expected 1"
fi

# ---- 3: same module names, different bodies, one shared directory ----
#
# The dangerous shape the shared directory introduces: a second project
# whose module `a` has the same name but a different body must not be
# served the first project's blob.

PROJ2="$WORK/p2"
mkdir -p "$PROJ2"
cat > "$PROJ2/a.kai" <<'EOF'
pub fn shout(s: String) : Unit / Stdout = { print(s) print(s) }
EOF
cat > "$PROJ2/b.kai" <<'EOF'
import a
pub fn twice(s: String) : Unit / Stdout = a.shout(s)
EOF
cat > "$PROJ2/main.kai" <<'EOF'
import a
import b
fn main() : Unit / Console { b.twice("hi") }
EOF

build "$WORK/p2_plain.c" "$PROJ2"
# shellcheck disable=SC2046
build "$WORK/p2_shared.c" "$PROJ2" $(shared)
same "$WORK/p2_plain.c" "$WORK/p2_shared.c" \
     "second project served a colliding blob from the shared dir"

if cmp -s "$WORK/plain.c" "$WORK/p2_plain.c"; then
  fail "fixture is not discriminating — the two projects emit the same C"
fi

# ---- 4: --check agrees, warm, and still reports a type error ---------

# shellcheck disable=SC2046
if ! "$KAIC2" $(shared) --check --path "$STDLIB" --path "$PROJ" \
     "$PROJ/main.kai" >/dev/null 2>"$WORK/err"; then
  cat "$WORK/err"
  fail "--check rejected a program a full build accepts"
fi

cat > "$PROJ/bad.kai" <<'EOF'
fn main() : Unit = { let x: Int = "not an int" () }
EOF
# shellcheck disable=SC2046
if "$KAIC2" $(shared) --check --path "$STDLIB" --path "$PROJ" \
   "$PROJ/bad.kai" >/dev/null 2>"$WORK/baderr"; then
  fail "--check accepted a type-incorrect program with a warm cut"
fi
grep -q "type mismatch" "$WORK/baderr" \
  || fail "warm --check lost the type-mismatch diagnostic" cat "$WORK/baderr"

echo "typedc_cut_shared_core_dir: OK — cut rides the shared core dir, cold/warm/edit == plain, no cross-project collision, --check diagnostics intact"
exit 0
