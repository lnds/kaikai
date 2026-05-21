#!/bin/sh
# tools/test-info-blocks.sh — compile every fenced kaikai block in
# docs/info/*.md to guarantee the documented surface matches the
# compiler.
#
# Fence tags:
#   ```kaikai          full program; must compile clean.
#   ```kaikai-snippet  expression fragment; wrapped in a tiny harness
#                      before compiling.
#   ```kaikai-neg      must FAIL to compile (used in NOT IN KAIKAI
#                      sections). The script inverts the assert.
#   ```text  ```sh  ```toml ```diff  etc.   ignored.
#
# Wired into `make test-info` alongside the page-presence smoke.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
INFO_DIR="$ROOT/docs/info"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

total=0
ok=0
fails=0
fail_log="$WORK/fails.log"
: > "$fail_log"

# Extract every fenced kaikai* block from a markdown file into
# numbered files inside $WORK/$topic/. For each block we write two
# siblings:
#   block_N.tag    one line with the tag (kaikai|kaikai-snippet|kaikai-neg)
#   block_N.body   the raw block body
extract_blocks() {
  page="$1"
  topic="$2"
  out_dir="$WORK/$topic"
  mkdir -p "$out_dir"
  awk -v out_dir="$out_dir" '
    BEGIN { in_code = 0; tag = ""; n = 0; out_body = ""; out_tag = "" }
    /^```/ {
      if (in_code) {
        # Close current block.
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
  ' "$page"
}

compile_one() {
  topic="$1"
  block_file="$2"   # path to block_NNN.body

  body_path="$block_file"
  tag_path="${block_file%.body}.tag"
  tag="$(cat "$tag_path")"
  num="$(basename "$block_file" .body | sed 's/^block_//')"

  total=$((total + 1))
  src="$WORK/${topic}_${num}.kai"
  out="$WORK/${topic}_${num}"
  log="$WORK/${topic}_${num}.log"

  case "$tag" in
    kaikai|kaikai-neg)
      cp "$body_path" "$src" ;;
    kaikai-snippet)
      {
        printf 'fn __snippet_main() : Int = {\n'
        printf '  let _ = '
        cat "$body_path"
        printf '\n  0\n}\n'
        printf 'fn main() : Int = __snippet_main()\n'
      } > "$src" ;;
    *)
      echo "internal: unknown tag '$tag'" >&2
      exit 2 ;;
  esac

  if "$KAI" build "$src" -o "$out" >"$log" 2>&1; then
    case "$tag" in
      kaikai-neg)
        fails=$((fails + 1))
        {
          echo "FAIL [$topic block $num, tag=$tag]: expected compile failure, but it succeeded"
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
          echo "FAIL [$topic block $num, tag=$tag]:"
          echo "---- source:"
          cat "$src"
          echo "---- compiler output:"
          cat "$log"
          echo "----"
        } >> "$fail_log" ;;
    esac
  fi
}

for page in "$INFO_DIR"/*.md; do
  [ -f "$page" ] || continue
  topic="$(basename "$page" .md)"
  extract_blocks "$page" "$topic"
  for block in "$WORK/$topic"/*.body; do
    [ -f "$block" ] || continue
    compile_one "$topic" "$block"
  done
done

if [ "$fails" -gt 0 ]; then
  echo "test-info-blocks: $fails failure(s) out of $total block(s)" >&2
  echo "---" >&2
  cat "$fail_log" >&2
  exit 1
fi

echo "test-info-blocks: OK — $ok/$total compile blocks pass"
