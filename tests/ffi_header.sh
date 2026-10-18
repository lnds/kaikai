#!/bin/sh
# A C source the build compiles by hand — a `$CFLAGS` shim or a `[native]`
# source — finds the generated `kai_ffi.h` on its include path, on every
# backend and through `kai build` and `kai run`. The header is rebuilt with
# the program: a struct that changes width rebuilds the shim against the new
# layout, never reuses an object or a binary built against the old one.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

export XDG_CACHE_HOME="$work/cache"
fail=0

check() {
  if [ "$2" = "$3" ]; then echo "ffi-header OK ($1)"; else echo "ffi-header FAIL ($1): want '$3', got '$2'"; fail=1; fi
}

printf 'fn main() {\n  println("native")\n}\n' > "$work/probe.kai"
backends="c"
if "$KAI" build --backend=native "$work/probe.kai" -o "$work/probe" > /dev/null 2>&1; then
  backends="c native"
else
  echo "ffi-header SKIP (native): this kaic2 has no libLLVM"
fi

fixture="$work/fixture"
mkdir -p "$fixture"
cp "$ROOT/examples/ffi/v2_struct_by_value.kai" "$ROOT/examples/ffi/v2_struct_shim.c" "$fixture/"
want="$(cat "$ROOT/examples/ffi/v2_struct_by_value.out.expected")"

for b in $backends; do
  out="$fixture/sbv-$b"
  (cd "$fixture" && CFLAGS="v2_struct_shim.c" "$KAI" build --backend="$b" v2_struct_by_value.kai -o "$out") \
    || { echo "ffi-header FAIL (build $b)"; fail=1; continue; }
  check "build $b" "$("$out")" "$want"
  check "run $b" "$(cd "$fixture" && CFLAGS="v2_struct_shim.c" "$KAI" run --backend="$b" v2_struct_by_value.kai)" "$want"
done

out="$fixture/sbv-single"
(cd "$fixture" && KAI_MODULAR=0 CFLAGS="v2_struct_shim.c" "$KAI" build --backend=c v2_struct_by_value.kai -o "$out") \
  || { echo "ffi-header FAIL (build c single)"; fail=1; }
check "build c single" "$("$out" 2>/dev/null || true)" "$want"

# A struct that changes width: the shim and the binary must follow it.
pt="$work/pt"
mkdir -p "$pt"
program() {
  cat > "$pt/pt.kai" <<EOF
extern "C" type Pt = { x: $1, y: $1 }

extern "C"("pt_sum")
fn pt_sum(p: Pt) : Int / Ffi

fn main() : Unit / Console + Ffi = print(int_to_string(pt_sum(Pt { x: 40, y: 2 })))
EOF
}
cat > "$pt/pt_shim.c" <<'EOF'
#include "kai_ffi.h"

int64_t pt_sum(Pt p) { return (int64_t) p.x + (int64_t) p.y; }
EOF
for b in $backends; do
  for w in I32 I64 I32; do
    program "$w"
    (cd "$pt" && CFLAGS="pt_shim.c" "$KAI" build --backend="$b" pt.kai -o "pt-$b") \
      || { echo "ffi-header FAIL (rebuild $b $w)"; fail=1; continue; }
    check "rebuild $b $w" "$("$pt/pt-$b")" 42
  done
done

# The shim's own text is an input of the build.
program I32
for edit in 42 7; do
  printf '#include "kai_ffi.h"\n\nint64_t pt_sum(Pt p) { (void) p; return %s; }\n' "$edit" > "$pt/pt_edit.c"
  (cd "$pt" && CFLAGS="pt_edit.c" "$KAI" build --backend=c pt.kai -o pt-edit) \
    || { echo "ffi-header FAIL (shim edit $edit)"; fail=1; continue; }
  check "shim edit $edit" "$("$pt/pt-edit")" "$edit"
done

# The shim reaches the header only through another header of its own.
program I32
printf '#include "kai_ffi.h"\n' > "$pt/my.h"
printf '#include "my.h"\n\nint64_t pt_sum(Pt p) { return (int64_t) p.x + (int64_t) p.y; }\n' > "$pt/pt_indirect.c"
for b in $backends; do
  (cd "$pt" && CFLAGS="pt_indirect.c" "$KAI" build --backend="$b" pt.kai -o "pt-indirect-$b") \
    || { echo "ffi-header FAIL (indirect $b)"; fail=1; continue; }
  check "indirect $b" "$("$pt/pt-indirect-$b")" 42
done

# A program without an `extern "C" type` builds with a shim that never includes the header.
printf 'fn main() {\n  println("plain")\n}\n' > "$pt/plain.kai"
printf 'int unused_by_plain(void) { return 0; }\n' > "$pt/plain_shim.c"
(cd "$pt" && CFLAGS="plain_shim.c" "$KAI" build --backend=c plain.kai -o plain) \
  || { echo "ffi-header FAIL (no struct)"; fail=1; }
check "no struct" "$("$pt/plain")" plain

# A `[native]` source includes the header too.
pkg="$work/pkg"
mkdir -p "$pkg/c"
cat > "$pkg/kai.toml" <<'EOF'
name = "ffihdr"
entry = "main.kai"

[native]
sources = ["c/shim.c"]
include = ["c"]
EOF
printf '#include "kai_ffi.h"\n' > "$pkg/c/my.h"
printf '#include "my.h"\n\nint64_t pt_sum(Pt p) { return (int64_t) p.x + (int64_t) p.y; }\n' > "$pkg/c/shim.c"
for b in $backends; do
  for w in I32 I64 I32; do
    cat > "$pkg/main.kai" <<EOF
extern "C" type Pt = { x: $w, y: $w }

extern "C"("pt_sum")
fn pt_sum(p: Pt) : Int / Ffi

fn main() : Unit / Console + Ffi = print(int_to_string(pt_sum(Pt { x: 40, y: 2 })))
EOF
    (cd "$pkg" && "$KAI" build --backend="$b" -o "pkg-$b") \
      || { echo "ffi-header FAIL (package $b $w)"; fail=1; continue; }
    check "package $b $w" "$("$pkg/pkg-$b")" 42
  done
done

[ "$fail" = 0 ] || exit 1
