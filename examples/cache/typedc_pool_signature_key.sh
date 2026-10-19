#!/bin/sh
# A core function the front end types only when the program asks for it
# has no entry among the interface a typed module inherits: its scheme is
# built from the signature when the module is typed. A typed blob of a
# module that calls it is therefore valid only while that signature is
# unchanged, and the key must say so. Editing one class of the signature
# of a function in a copy of the stdlib must make the program fail to
# compile again, even though a blob typed against the old signature is in
# the cache. Each class is checked on its own, and each edit is first shown
# to fail with no cache, so a pass cannot come from anything but the key.
# The key reads the scheme, not the spelling: renaming a type parameter is
# a hit. A bound is not part of the scheme the typer reads (the lowering
# strips it and the monomorphiser checks it against the declaration each
# compile), so a blob does not depend on it; the bound class below shows the
# check still bites with a blob cached.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

fail() {
  echo "typedc_pool_signature_key: FAIL — $1"
  [ -f "$WORK/err" ] && cat "$WORK/err"
  exit 1
}

PROJ="$WORK/p"
mkdir -p "$PROJ"
cat > "$PROJ/main.kai" <<'EOF'
type Opaque = Opaque(Int)

fn main() : Unit / Stdout = {
  let a: Int = probe_param(1)
  let b: Int = probe_ret(1)
  let c: Int = probe_row(1)
  let d: Int = probe_bound(Opaque(1))
  let e: Int = probe_gen(1)
  Stdout.print(show(a + b + c + d + e))
}
EOF

# $1: file, rest: extra flags. Prints nothing; the exit status is the compiler's.
compile() {
  f="$1"; shift
  KAIKAI_STDLIB_PATH="$WORK/stdlib" "$KAIC2" $EDITION_FLAG --path "$WORK/stdlib" --path "$PROJ" "$@" "$f" > "$WORK/out.c" 2> "$WORK/err"
}

write_probes() { # $1 param, $2 ret, $3 row, $4 bound, $5 generic type parameter, $6 its bound
  rm -rf "$WORK/stdlib"
  cp -R "$ROOT/stdlib" "$WORK/stdlib"
  cat >> "$WORK/stdlib/core/string.kai" <<EOF

pub fn probe_param(x: $1) : Int = panic("probe")
pub fn probe_ret(x: Int) : $2 = panic("probe")
pub fn probe_row(x: Int) : Int$3 = panic("probe")
pub fn probe_bound$4(x: T) : Int = panic("probe")
pub fn probe_gen[${5:-T}: ${6:-Show}](x: ${5:-T}) : Int = panic("probe")
EOF
}

BASE_PARAM="Int"; BASE_RET="Int"; BASE_ROW=""; BASE_BOUND="[T]"

CACHE="$WORK/cache"
cached() { compile "$PROJ/main.kai" --user-cache --user-cache-dir "$CACHE" --core-cache-dir "$CACHE"; }
fresh() { compile "$PROJ/main.kai"; }

write_probes "$BASE_PARAM" "$BASE_RET" "$BASE_ROW" "$BASE_BOUND"
fresh || fail "the baseline does not compile"
cached || fail "the baseline does not compile with the cache"
cached || fail "the baseline does not compile from a warm cache"

# $1: class, $2..$5: the four probe signatures after the edit
edit_class() {
  class="$1"; shift
  write_probes "$@"
  if fresh; then fail "$class: the edit does not break the program, so it proves nothing"; fi
  if cached; then fail "$class: a typed blob typed against the old signature was served"; fi
}

edit_class "parameter type" "String" "$BASE_RET" "$BASE_ROW" "$BASE_BOUND"
edit_class "return type" "$BASE_PARAM" "String" "$BASE_ROW" "$BASE_BOUND"
edit_class "effect row" "$BASE_PARAM" "$BASE_RET" " / File" "$BASE_BOUND"
edit_class "type parameter bound" "$BASE_PARAM" "$BASE_RET" "$BASE_ROW" "[T: Show]"

blobs() { ls "$CACHE"/tm-*.kab 2>/dev/null | wc -l | tr -d ' '; }

# A rename keeps the scheme: nothing is re-keyed, nothing is published.
write_probes "$BASE_PARAM" "$BASE_RET" "$BASE_ROW" "$BASE_BOUND"
cached || fail "the baseline does not compile with the cache"
before="$(blobs)"
write_probes "$BASE_PARAM" "$BASE_RET" "$BASE_ROW" "$BASE_BOUND" U Show
cached || fail "renaming a type parameter breaks the program"
[ "$(blobs)" -eq "$before" ] || fail "renaming a type parameter missed the cache ($before -> $(blobs) blobs)"

# A bound the argument still meets changes no scheme: the blobs are served.
write_probes "$BASE_PARAM" "$BASE_RET" "$BASE_ROW" "$BASE_BOUND" T Eq
cached || fail "changing a bound to one the argument meets breaks the program"
[ "$(blobs)" -eq "$before" ] || fail "changing a bound re-keyed the typed blobs ($before -> $(blobs))"

echo "typedc_pool_signature_key: OK — an edit of a parameter type, a return type, an effect row or a type parameter bound of a function typed on demand is never served from a typed blob of the old signature, and renaming a type parameter or changing a bound keeps the blobs"
exit 0
