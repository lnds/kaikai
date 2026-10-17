#!/bin/sh
# Core emit-cache differential fixture — a HOF-with-lambda edit or a new
# top-level fn HITS the warm core emit cache; a new unwind variant MISSES.
#
# `closure_spec_pass` appends per-call-site spec decls named
# `<fn>__cspec__L<line>_<col>` with `mo=None` (root-homed). Those names
# carry the lambda body's span, so admitting them to the surface key let
# a plain body edit that adds, removes, or moves a specialisable HOF call
# re-key the core emit cache and force a cold re-emit — the exact hot
# rebuild loop the cache exists for. A cspec decl is derived and cannot
# perturb a core TU, so it stays out of the surface key.
#
#   - A: `list.sum([1,2,3])`                       -> cold miss, entry written.
#   - B: `list.sum(list.map([1,2,3], (x)=>x*2))`   -> warm HIT.
#
# The core TU set is found by a pre-key and the variant of the program (the
# core effects its handlers make unwind), never by the program's name-level
# surface, so the core TUs cannot depend on it:
#   - C: a new top-level fn           -> still HITS, and the core TUs it is
#        compiled against are byte-identical to a no-cache compile.
#   - D: a row that may cancel (`/ Spawn`) flips the variant -> MISS, the
#        whole core is compiled and recorded.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the cache keys carry the
# edition, so this fixture must exercise the one the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
STDLIB="$ROOT/stdlib"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

# A: no specialisable HOF call.
cat > "$PROJ/a.kai" <<'EOF'
fn main() : Unit / Console {
  print("#{int_to_string(list.sum([1, 2, 3]))}")
}
EOF

# B: same name-level surface, body adds a HOF-with-lambda (a cspec decl).
cat > "$PROJ/b.kai" <<'EOF'
fn main() : Unit / Console {
  print("#{int_to_string(list.sum(list.map([1, 2, 3], (x) => x * 2)))}")
}
EOF

# C: a new top-level fn.
cat > "$PROJ/c.kai" <<'EOF'
fn helper(n: Int) : Int = n + 1
fn main() : Unit / Console {
  print("#{int_to_string(list.sum(list.map([1, 2, 3], (x) => x * 2)))}")
}
EOF

# D: a row that may cancel.
cat > "$PROJ/d.kai" <<'EOF'
fn helper() : Unit / Spawn = ()
fn main() : Unit / Console {
  print("#{int_to_string(list.sum([1, 2, 3]))}")
}
EOF

CACHE="$PROJ/core-cache"
mkdir -p "$CACHE"

# A: cold cache -> miss, entry written.
"$KAIC2" $EDITION_FLAG --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/a.kai" \
  > /dev/null 2> "$PROJ/a.err"
grep -q "core-emit-cache: miss" "$PROJ/a.err" || {
  echo "corec_cspec_span_free_key FAIL — A did not report a cold miss"
  cat "$PROJ/a.err"; exit 1; }

# B: adding a HOF-with-lambda must HIT — the cspec decl is span-free-keyed
# out of the surface. This is the line that FAILS on the pre-fix compiler.
"$KAIC2" $EDITION_FLAG --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/b.kai" \
  > /dev/null 2> "$PROJ/b.err"
grep -q "core-emit-cache: hit" "$PROJ/b.err" || {
  echo "corec_cspec_span_free_key FAIL — adding a HOF-with-lambda call missed the warm core emit cache"
  cat "$PROJ/b.err"; exit 1; }

# C: a new top-level fn does not touch the variant: it hits, and pruning
# against the cached set emits the same core TUs as an unpruned compile.
"$KAIC2" $EDITION_FLAG --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/c.kai" \
  > /dev/null 2> "$PROJ/c.err"
grep -q "core-emit-cache: hit" "$PROJ/c.err" || {
  echo "corec_cspec_span_free_key FAIL — a new top-level fn missed the core TU set"
  cat "$PROJ/c.err"; exit 1; }
"$ROOT/tools/cmodular-prune-diff.sh" "$KAIC2" "$STDLIB" "$PROJ/a.kai" "$PROJ/b.kai" "$PROJ/c.kai" \
  > "$PROJ/diff.out" 2>&1 || {
  echo "corec_cspec_span_free_key FAIL — pruned core TUs differ from the unpruned ones"
  cat "$PROJ/diff.out"; exit 1; }

# D: a row that may cancel changes what the core's functions unwind
# through, so it is another variant and must MISS.
"$KAIC2" $EDITION_FLAG --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/d.kai" \
  > /dev/null 2> "$PROJ/d.err"
grep -q "core-emit-cache: miss" "$PROJ/d.err" || {
  echo "corec_cspec_span_free_key FAIL — a program of another unwind variant hit the core TU set"
  cat "$PROJ/d.err"; exit 1; }

echo "corec_cspec_span_free_key OK — HOF-with-lambda edit and a new fn hit, a new unwind variant misses"
exit 0
