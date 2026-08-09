#!/bin/bash
# Native-emission namespace gate — the native mirror of `test-kai-namespace`.
#
# The C gate reads the emitted C as TEXT. Native emits an object file, so
# the check runs over the real symbol table (`nm`): every `kai`-prefixed
# symbol the object defines must be a name the runtime itself defines.
# Anything else is a user-derived identifier minted into the runtime's
# namespace, which is a link-time collision waiting for the one program
# whose csym matches a runtime helper.
#
# The probe exercises every family that mints from a user symbol: a named
# fn used as a value (thunk), a raw protocol impl (boxed adapter), and a
# handler with an op clause and a `finally` (handler/clause/cleanup syms).
# `nm` is read at CGLEVEL=0 — the O2 pipeline internalises and DCEs most of
# these away, which would hide a leak rather than catch it.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

LLVM_CONFIG="${LLVM_CONFIG:-llvm-config}"
if ! command -v "$LLVM_CONFIG" >/dev/null 2>&1; then
  echo "test-native-namespace: SKIP (llvm-config not in PATH; native backend needs libLLVM)"
  exit 0
fi

KAIC2="$ROOT/stage2/kaic2"
[ -x "$KAIC2" ] || { echo "test-native-namespace FAIL — no kaic2 at $KAIC2"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat > "$work/probe.kai" <<'PROBE'
effect Greeter {
  greet(s: String) : String
}

protocol Shown {
  show_it(x: Self) : String
}

type Wrap = W(Int)

impl Shown for Wrap {
  fn show_it(w: Wrap) : String = match w { W(n) -> int_to_string(n) }
}

fn twice(f: (Int) -> Int, n: Int) : Int = f(f(n))

fn bump(n: Int) : Int = n + 1

fn main() : Int / Stdout = {
  let r = handle {
    Greeter.greet("kai")
  } with Greeter {
    greet(s, resume) -> resume(string_concat("hi ", s))
    finally { () }
  }
  Stdout.print(r)
  Stdout.print(int_to_string(twice(bump, 1)))
  Stdout.print(show_it(W(7)))
  0
}
PROBE

KAI_NATIVE_CGLEVEL=0 "$KAIC2" --emit=native --path "$ROOT/stdlib" "$work/probe.kai" >/dev/null 2>"$work/emit.err" \
  || { echo "test-native-namespace FAIL — native emit failed:"; cat "$work/emit.err"; exit 1; }

obj="$work/probe.o"
[ -f "$obj" ] || { echo "test-native-namespace FAIL — no object emitted at $obj"; exit 1; }

# `--defined-only` keeps just the symbols this object DEFINES — the only ones
# that can collide at link. `nm -a` on ELF also lists debug/file entries,
# among them the LLVM module name (`kai_native`, a compiler constant, not a
# user identifier), which Mach-O never reports: a plain `-a` reads as a leak
# on Linux and green on macOS.
#
# `$tlv$init` / `.buf` suffixes are linker-minted decorations of a runtime
# symbol, not separate names — trim them to their base.
#
# Mach-O prefixes every C symbol with an underscore, ELF does not, so the
# same source name reads `__kai_x` on macOS and `_kai_x` on Linux. Strip that
# ONE platform underscore only when what remains still starts with `_kai` /
# `kai` — a blind `s/^_//` would eat the source underscore of `_kai_x` on
# ELF, turning it into `kai_x` and reporting a leak that is not there.
# The user namespaces (`_kaiu_`/`_kaiv_`) are the CORRECT destination, so they
# drop out here; what remains is the runtime namespace proper, where nothing
# user-derived may appear.
# Both sides are then compared with any single leading underscore dropped, so
# the Mach-O `_kai_main` and the source's `kai_main` are one name.
nm --defined-only "$obj" | awk '{print $NF}' \
  | sed 's/^__kai/_kai/; s/^_kaiu_/kaiu_/; s/^_kaiv_/kaiv_/' \
  | sed 's/\$tlv\$init$//; s/\.[A-Za-z0-9_]*$//' \
  | grep -E '^_?kai' | grep -vE '^kai[uv]_' | sed 's/^_//' | sort -u > "$work/emitted.txt"

{ tr -c 'A-Za-z0-9_' '\n' < "$ROOT/stage2/runtime.h"
  tr -c 'A-Za-z0-9_' '\n' < "$ROOT/stage0/runtime_llvm.c"
} | grep -E '^_?kai' | grep -vE '^_?kai[uv]_' | sed 's/^_//' | sort -u > "$work/runtime.txt"

# `_kai_default_ev_<eff>` / `_kai_default_node_<eff>` carry an effect NAME,
# so they are user-derived, but both backends mint the identical spelling and
# the runtime defines neither — moving them alone would break C/native byte
# identity without closing a collision. Waived by shape, never by symbol: a
# NEW family cannot hide behind the waiver.
grep -vE '^kai_default_(ev|node)_' "$work/emitted.txt" > "$work/checked.txt"

leaks="$(comm -23 "$work/checked.txt" "$work/runtime.txt")"
if [ -n "$leaks" ]; then
  echo "test-native-namespace FAIL: user-derived symbols emitted into the runtime kai namespace:"
  echo "$leaks"
  exit 1
fi

echo "test-native-namespace OK — every emitted kai* symbol is a runtime name; user symbols stay in kaiu_/kaiv_"
