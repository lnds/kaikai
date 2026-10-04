#!/bin/sh
# tools/test-kaic-link.sh — test of tools/kaic-link.sh's object cache.
#
# Links a three-TU c-modular stream with the real cc and runtime owner. The
# contract under test: an object is reused only while everything cc reads for
# it is unchanged — the TU, a header it includes, the flags — and a binary
# linked after an edit runs the edited code.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LINK_SH="$ROOT/tools/kaic-link.sh"

fail() { echo "test-kaic-link: FAIL: $1" >&2; exit 1; }
ok()   { echo "test-kaic-link: ok — $1"; }

work="$(mktemp -d "${TMPDIR:-/tmp}/kaic-link-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

# stream <a's result> <the result main expects>
stream() {
  cat > "$work/prog.c" <<EOF
//KAIDELIM:7
//KAIFILE:7::kai_decls_a.h
int a(void);
#define WANT $2
//KAIFILE:7::a.c
#include "kai_decls_a.h"
int a(void) { return $1; }
//KAIFILE:7::__root__.c
#include "kai_decls_a.h"
int main(void) { return a() == WANT ? 0 : 1; }
EOF
}

# link [flags...] — sets `compiled` to the number of objects cc built.
link() {
  CC="${CC:-cc}" LDLIBS=-lm "$LINK_SH" "$work/prog.c" "$work/prog" \
    -std=c99 -I "$ROOT/stage2" -I "$ROOT/stage0" "$@" 2> "$work/log" \
    || { cat "$work/log" >&2; fail "link failed"; }
  compiled="$(sed -n 's/.*compiled=\([0-9]*\)$/\1/p' "$work/log")"
}
built() { [ "$compiled" = "$1" ] || fail "$2: compiled $compiled objects, expected $1"; }
cached() { find "$work/obj-cache" -name '*.o' | wc -l | tr -d ' '; }

stream 1 1
link; built 3 "first link"
"$work/prog" || fail "the linked program does not run"
link; built 0 "unchanged relink"
ok "an unchanged stream relinks from the cache"

stream 2 1
link; built 1 "one TU edited"
if "$work/prog"; then fail "an edited TU was linked from its old object"; fi
stream 2 2
link; built 2 "shared header edited"
"$work/prog" || fail "a header edit was linked from old objects"
ok "an edited TU rebuilds alone; an edited header rebuilds every TU that includes it"

link -DKAI_TEST_FLAG=1; built 3 "flag added"
[ "$(cached)" = 3 ] || fail "the cache holds $(cached) objects after three links of three TUs"
ok "a flag change rebuilds everything; objects no link used are pruned"

for obj in "$work/obj-cache/prog"/*.o; do : > "$obj.part"; mv "$obj.part" "$obj"; done
stream 2 2
if CC="${CC:-cc}" LDLIBS=-lm "$LINK_SH" "$work/prog.c" "$work/prog" -std=c99 -I "$ROOT/stage2" -I "$ROOT/stage0" \
    -DKAI_TEST_FLAG=1 2>/dev/null; then
  fail "the test's own probe is blind: emptied cache objects linked"
fi
ok "the cache is what the link reads: emptied objects break it"

echo "test-kaic-link: all cases passed"
