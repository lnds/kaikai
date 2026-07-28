#!/bin/sh
# The per-module typed cut activated by the SHARED core-cache directory,
# with no `--user-cache`. `bin/kai` always passes
# `--core-cache-dir`/`--toolchain-id`, so a typecheck-only run rides that
# directory and the stdlib core is inferred once per toolchain instead of
# once per invocation.
#
# The cut's blobs are content-addressed by a key folding the module's
# decls, its inherited interface hash and the cross-module context, so
# sharing a directory across projects is safe. This fixture is the gate
# on that claim. Serving a stale typed interface is a correctness bug
# worse than having no cache, so each property below is hard.
#
# 1. The cut actually cuts — `--check` lands blobs in the shared
#    directory when only `--core-cache-dir` is passed, and a body edit
#    republishes exactly the edited module.
# 2. A build publishes NOTHING there. Only the front-end-only modes ride
#    the shared directory: a build would pay one blob per module, which
#    the whole-compiler selfhost repeats per run for a cache it never
#    reads back. Keeping builds out is what holds that gate inside its
#    time budget.
# 3. Acceptance is unchanged — cold and warm `--check` agree with a full
#    build, whose emitted C is unaffected by the cut.
# 4. Two projects that declare the same module name with DIFFERENT
#    bodies must not serve each other's blob out of the one directory.
# 5. Diagnostics survive: a type error is still reported warm, and the
#    nodes the cut hashes before inference do not panic the codec.

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

# $1: project dir, rest: extra flags — a typecheck-only run.
check() {
  proj="$1"; shift
  "$KAIC2" "$@" --check --path "$STDLIB" --path "$proj" "$proj/main.kai" \
    >/dev/null 2>"$WORK/checkerr" || {
      cat "$WORK/checkerr"
      fail "--check exited non-zero ($*)"
    }
  grep -q '^panic:' "$WORK/checkerr" 2>/dev/null \
    && fail "--check panicked ($*)" cat "$WORK/checkerr"
  return 0
}

# ---- 1: --check publishes, and a body edit republishes one module ----

# shellcheck disable=SC2046
check "$PROJ" $(shared)
cold_blobs="$(blob_count)"
if [ "$cold_blobs" -lt 2 ]; then
  fail "--check published $cold_blobs typed blobs into the shared dir (cut inactive?)"
fi

# Warm run: same key set, so nothing new lands.
# shellcheck disable=SC2046
check "$PROJ" $(shared)
if [ "$(blob_count)" -ne "$cold_blobs" ]; then
  fail "warm --check republished blobs ($cold_blobs -> $(blob_count)); keys are unstable"
fi

# A body-only edit re-infers exactly its own module: one new blob.
cat > "$PROJ/b.kai" <<'EOF'
import a
pub fn twice(s: String) : Unit / Stdout = { a.shout(s) a.shout(s) a.shout(s) }
EOF
# shellcheck disable=SC2046
check "$PROJ" $(shared)
edit_blobs="$(blob_count)"
if [ "$edit_blobs" -ne $((cold_blobs + 1)) ]; then
  fail "body edit published $((edit_blobs - cold_blobs)) blobs, expected 1"
fi

# ---- 2: a build publishes nothing into the shared directory ----------
#
# Builds deliberately stay off this cache. The whole-compiler selfhost
# compiles ~120 modules and would serialise a blob for each on every run,
# for a cache that gate never reads back.
#
# Must run against a VIRGIN directory: the checks above already published
# these modules under these keys, so a build republishing the same keys
# would leave the file count unchanged and slip past a counter.

CC_BUILD="$WORK/coredir_build"
mkdir -p "$CC_BUILD"
# shellcheck disable=SC2046
build "$WORK/shared_build.c" "$PROJ" \
  --core-cache-dir "$CC_BUILD" --toolchain-id "$TID"
build_blobs="$(ls "$CC_BUILD"/tm-*.kab 2>/dev/null | wc -l | tr -d ' ')"
if [ "$build_blobs" -ne 0 ]; then
  fail "a build published $build_blobs typed blobs into the shared dir"
fi
# The core layer DOES belong to a build — assert the directory was live,
# or a broken --core-cache-dir would make the check above vacuous.
if [ "$(ls "$CC_BUILD"/core-*.kab 2>/dev/null | wc -l | tr -d ' ')" -eq 0 ]; then
  fail "build wrote no core blobs either — the shared dir was never active"
fi

# ---- 3: the cut does not perturb the emitted C ------------------------

build "$WORK/plain.c" "$PROJ"
same "$WORK/plain.c" "$WORK/shared_build.c" \
     "build C differs when the shared core dir is passed"

# ---- 4: same module names, different bodies, one shared directory ----
#
# The dangerous shape the shared directory introduces: a second project
# whose module `a` has the same name but a different body must not be
# served the first project's blob. Exercised through `--check`, the mode
# that actually reads and writes this directory: a colliding key would
# publish nothing new for p2 and hand it p1's typed interface.

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

before_p2="$(blob_count)"
# shellcheck disable=SC2046
check "$PROJ2" $(shared)
if [ "$(blob_count)" -le "$before_p2" ]; then
  fail "second project published no new blobs — its modules collided with p1's keys"
fi

# The two projects really do differ, or the collision check proves nothing.
build "$WORK/p2_plain.c" "$PROJ2"
if cmp -s "$WORK/plain.c" "$WORK/p2_plain.c"; then
  fail "fixture is not discriminating — the two projects emit the same C"
fi

# ---- 5: --check agrees, warm, and still reports a type error ---------

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

# ---- 6: nodes the cut hashes BEFORE inference can normalise them -----
#
# Keying a module serialises its decls before inference runs, so any
# node the pipeline only removes *during* inference reaches the codec.
# Two such nodes, each of which turned a diagnostic into a codec panic:
#
#   - an out-of-range integer literal stays `EIntLit`, because failing
#     to mint it IS the diagnostic under test;
#   - the root file keeps its module doc so the typer can lift it into
#     `TypedProgram.module_doc` (only imported modules drop it).
#
# Both must survive keying and still produce their normal output.

cat > "$PROJ/overflow.kai" <<'EOF'
fn main() : Int / Stdout = {
  let x = 9223372036854775808
  Stdout.print(show(x))
  0
}
EOF
# shellcheck disable=SC2046
if "$KAIC2" $(shared) --check --path "$STDLIB" --path "$PROJ" \
   "$PROJ/overflow.kai" >/dev/null 2>"$WORK/ovferr"; then
  fail "--check accepted an out-of-range Int literal"
fi
grep -q "out of Int range" "$WORK/ovferr" \
  || fail "out-of-range literal lost its diagnostic under the cut" \
          cat "$WORK/ovferr"

cat > "$PROJ/moddoc.kai" <<'EOF'
#[doc("""
Module-position doc on the root file.
""")]

fn main() : Unit / Stdout = Stdout.print("ok")
EOF
# shellcheck disable=SC2046
if ! "$KAIC2" $(shared) --check --path "$STDLIB" --path "$PROJ" \
     "$PROJ/moddoc.kai" >/dev/null 2>"$WORK/docerr"; then
  cat "$WORK/docerr"
  fail "--check rejected a root file carrying a module doc"
fi

# A codec panic exits non-zero, but assert on the text too so a future
# panic cannot masquerade as an ordinary rejection.
for e in "$WORK/ovferr" "$WORK/docerr" "$WORK/baderr"; do
  grep -q '^panic:' "$e" && fail "codec panicked instead of diagnosing" cat "$e"
done

echo "typedc_cut_shared_core_dir: OK — --check rides the shared core dir (builds do not), emitted C unchanged, no cross-project collision, pre-inference nodes keyed without panic, diagnostics intact"
exit 0
