#!/bin/sh
# tools/test-grammar-blocks.sh — compile every fenced ```kaikai block
# in docs/grammar.md to keep the surface reference in sync with the
# parser. Companion to tools/test-info-blocks.sh.
#
# Fences:
#   ```kaikai          full program; must compile clean.
#   ```kaikai-snippet  expression fragment; wrapped in a tiny harness.
#   ```kaikai-neg      must FAIL to compile (parser rejection).
#   ```text  ```sh  etc.   ignored.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DOC="$ROOT/docs/grammar.md"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

total=0
ok=0
fails=0
fail_log="$WORK/fails.log"
: > "$fail_log"

awk -v out_dir="$WORK" '
  BEGIN { in_code = 0; tag = ""; n = 0; out_body = ""; out_tag = "" }
  /^```/ {
    if (in_code) {
      printf "%s", body > out_body
      close(out_body)
      printf "%s\n", tag > out_tag
      close(out_tag)
      in_code = 0
      body = ""
      next
    }
    fence = substr($0, 4)
    sub(/^[[:space:]]+/, "", fence)
    sub(/[[:space:]]+$/, "", fence)
    if (fence == "kaikai" || fence == "kaikai-snippet" || fence == "kaikai-neg") {
      n++
      tag = fence
      out_body = sprintf("%s/block_%03d.body", out_dir, n)
      out_tag = sprintf("%s/block_%03d.tag", out_dir, n)
      in_code = 1
      body = ""
    }
    next
  }
  in_code { body = body $0 "\n" }
' "$DOC"

for body_path in "$WORK"/*.body; do
  [ -f "$body_path" ] || continue
  tag_path="${body_path%.body}.tag"
  tag="$(cat "$tag_path")"
  num="$(basename "$body_path" .body | sed 's/^block_//')"
  total=$((total + 1))
  src="$WORK/grammar_${num}.kai"
  out="$WORK/grammar_${num}"
  log="$WORK/grammar_${num}.log"

  case "$tag" in
    kaikai|kaikai-neg)
      cp "$body_path" "$src"
      # Auto-append a `fn main` so snippet fragments still compile.
      if ! grep -q "^fn main" "$src"; then
        printf '\nfn main() : Unit = ()\n' >> "$src"
      fi ;;
    kaikai-snippet)
      {
        printf 'fn __snippet_main() : Int = {\n'
        printf '  let _ = '
        cat "$body_path"
        printf '\n  0\n}\n'
        printf 'fn main() : Int = __snippet_main()\n'
      } > "$src" ;;
  esac

  if "$KAI" build "$src" -o "$out" >"$log" 2>&1; then
    case "$tag" in
      kaikai-neg)
        fails=$((fails + 1))
        {
          echo "FAIL [grammar block $num, tag=$tag]: expected compile failure"
          echo "---- source:"
          cat "$src"
          echo "----"
        } >> "$fail_log" ;;
      *)
        ok=$((ok + 1)) ;;
    esac
  else
    case "$tag" in
      kaikai-neg)
        ok=$((ok + 1)) ;;
      *)
        fails=$((fails + 1))
        {
          echo "FAIL [grammar block $num, tag=$tag]:"
          echo "---- source:"
          cat "$src"
          echo "---- compiler output:"
          cat "$log"
          echo "----"
        } >> "$fail_log" ;;
    esac
  fi
done

if [ "$fails" -gt 0 ]; then
  echo "test-grammar-blocks: $fails failure(s) out of $total block(s)" >&2
  echo "---" >&2
  cat "$fail_log" >&2
  exit 1
fi

echo "test-grammar-blocks: OK — $ok/$total compile blocks pass"
