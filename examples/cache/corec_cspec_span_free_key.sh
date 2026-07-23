#!/bin/sh
# Core emit-cache differential fixture — a HOF-with-lambda edit HITS the
# warm core emit cache.
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
#   - B: `list.sum(list.map([1,2,3], (x)=>x*2))`   -> warm HIT (same name-level
#        surface; the spec decl B adds no longer re-keys the core surface).
#
# Guardrail (never weaken invalidation): a real name-level surface
# change — a new top-level fn — must still MISS.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
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

# C: a genuine name-level surface change — a new top-level fn.
cat > "$PROJ/c.kai" <<'EOF'
fn helper(n: Int) : Int = n + 1
fn main() : Unit / Console {
  print("#{int_to_string(list.sum(list.map([1, 2, 3], (x) => x * 2)))}")
}
EOF

CACHE="$PROJ/core-cache"
mkdir -p "$CACHE"

# A: cold cache -> miss, entry written.
"$KAIC2" --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/a.kai" \
  > /dev/null 2> "$PROJ/a.err"
grep -q "core-emit-cache: miss" "$PROJ/a.err" || {
  echo "corec_cspec_span_free_key FAIL — A did not report a cold miss"
  cat "$PROJ/a.err"; exit 1; }

# B: adding a HOF-with-lambda must HIT — the cspec decl is span-free-keyed
# out of the surface. This is the line that FAILS on the pre-fix compiler.
"$KAIC2" --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/b.kai" \
  > /dev/null 2> "$PROJ/b.err"
grep -q "core-emit-cache: hit" "$PROJ/b.err" || {
  echo "corec_cspec_span_free_key FAIL — adding a HOF-with-lambda call missed the warm core emit cache"
  cat "$PROJ/b.err"; exit 1; }

# C: a new top-level fn is a real surface change and must still MISS —
# the fix drops cspec noise, it does not weaken invalidation.
"$KAIC2" --emit=c-modular --core-cache-dir "$CACHE" --toolchain-id tid1 \
  --core-cache-stats --path "$STDLIB" --path "$PROJ" "$PROJ/c.kai" \
  > /dev/null 2> "$PROJ/c.err"
grep -q "core-emit-cache: miss" "$PROJ/c.err" || {
  echo "corec_cspec_span_free_key FAIL — a new top-level fn did not change the surface key"
  cat "$PROJ/c.err"; exit 1; }

echo "corec_cspec_span_free_key OK — HOF-with-lambda edit hits, real surface change still misses"
exit 0
