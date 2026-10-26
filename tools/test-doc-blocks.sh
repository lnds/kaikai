#!/bin/sh
# tools/test-doc-blocks.sh — check every fenced kaikai block in the
# repo's tracked Markdown against the compiler, so no doc teaches a form
# that no longer runs.
#
# Fence tags:
#   ```kaikai           full program; must build clean.
#   ```kaikai-snippet   expression; wrapped in a tiny harness, must build.
#   ```kaikai-neg       must FAIL to build.
#   ```kaikai-fragment  declarations or statements that use names defined
#                       elsewhere; must parse (as a file, or as a fn body).
#   ```kaikai-minimal   the stage 1 dialect; not checked here.
#   ```text ```sh ...   ignored.
# A ```kai fence, or an untagged fence whose lines start like kaikai
# declarations, fails: every kaikai block must say which check it gets.
#
# Dated records (lane retros, audits, investigations, reports) are not
# current docs and are skipped; see EXCLUDE. KAI_DOC_PAGES="a.md b.md"
# (paths relative to the repo root) checks only those pages.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KAI="$ROOT/bin/kai"
KAIC2="$ROOT/stage2/kaic2"
EXCLUDE='/lane-|audit|followups|phase0|retro|nocturnal-report|investigation-|tier3-experiment|native-perf-measurement|^/docs/decisions/|^/CHANGELOG\.md$'

parses() {
  "$KAIC2" --path "$ROOT/stdlib" --ast "$1" > /dev/null 2>&1
}

check_one() {
  topic="$1"
  block_file="$2"
  work="$3"
  tag="$(cat "${block_file%.body}.tag")"
  where="$(cat "${block_file%.body}.where")"
  num="$(basename "$block_file" .body | sed 's/^block_//')"
  src="$work/${topic}_${num}.kai"
  out="$work/${topic}_${num}"
  log="$work/${topic}_${num}.log"

  case "$tag" in
    kaikai|kaikai-neg|kaikai-fragment)
      cp "$block_file" "$src" ;;
    kaikai-snippet)
      {
        printf 'fn __snippet_main() : Int = {\n'
        printf '  let _ = '
        cat "$block_file"
        printf '\n  0\n}\n'
        printf 'fn main() : Int = __snippet_main()\n'
      } > "$src" ;;
    untagged)
      echo "FAIL [$where]: kaikai code in a fence with no tag; tag it kaikai, kaikai-snippet, kaikai-neg, kaikai-fragment or text"
      return ;;
    kai)
      echo "FAIL [$where]: \`\`\`kai fence; tag it kaikai, kaikai-snippet, kaikai-neg, kaikai-fragment or text"
      return ;;
  esac

  if [ "$tag" = kaikai-fragment ]; then
    if parses "$src"; then echo OK; return; fi
    { printf 'fn __fragment() = {\n'; cat "$block_file"; printf '\n}\n'; } > "$src.body.kai"
    if parses "$src.body.kai"; then echo OK; return; fi
    echo "FAIL [$where, tag=$tag]: does not parse as declarations or as a fn body"
    echo "---- source:"; cat "$block_file"; echo "----"
    return
  fi

  if "$KAI" build "$src" -o "$out" > "$log" 2>&1; then
    if [ "$tag" = kaikai-neg ]; then
      echo "FAIL [$where, tag=$tag]: expected a compile failure, but it built"
      echo "---- source:"; cat "$src"; echo "----"
    else
      echo OK
    fi
  else
    if [ "$tag" = kaikai-neg ]; then
      echo OK
    else
      echo "FAIL [$where, tag=$tag]:"
      echo "---- source:"; cat "$src"
      echo "---- compiler output:"; cat "$log"; echo "----"
    fi
  fi
}

# Extract the checked fences of one page into $work/$topic/block_N.{body,tag,where}.
extract_blocks() {
  page="$1"
  topic="$2"
  rel="$3"
  out_dir="$4/$topic"
  mkdir -p "$out_dir"
  awk -v out_dir="$out_dir" -v rel="$rel" '
    function flush() {
      if (keep || (tag == "" && looks_kaikai)) {
        n++
        base = sprintf("%s/block_%03d", out_dir, n)
        printf "%s", body > (base ".body"); close(base ".body")
        printf "%s\n", (tag == "" ? "untagged" : tag) > (base ".tag"); close(base ".tag")
        printf "%s:%d\n", rel, start > (base ".where"); close(base ".where")
      }
    }
    BEGIN { in_code = 0; n = 0 }
    /^[[:space:]]*```/ {
      line = $0
      sub(/^[[:space:]]*```/, "", line)
      sub(/[[:space:]]+$/, "", line)
      if (in_code && line == "") { flush(); in_code = 0; next }
      if (!in_code) {
        in_code = 1; tag = line; body = ""; start = NR + 1; looks_kaikai = 0
        keep = (tag == "kaikai" || tag == "kaikai-snippet" || tag == "kaikai-neg" || tag == "kaikai-fragment" || tag == "kai")
        next
      }
    }
    in_code {
      body = body $0 "\n"
      if ($0 ~ /^[[:space:]]*(pub fn|fn|import|type|effect|protocol|impl) [A-Za-z_]/) looks_kaikai = 1
    }
  ' "$page"
}

if [ "${1:-}" = "__check" ]; then
  check_one "$3" "$4" "$2"
  exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
JOBS="${KAI_TEST_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
self="$SCRIPT_DIR/test-doc-blocks.sh"

worklist="$WORK/worklist"
: > "$worklist"
for rel in $(git -C "$ROOT" ls-files '*.md'); do
  page="$ROOT/$rel"
  [ -f "$page" ] || continue
  if printf '%s\n' "/$rel" | grep -Eq "$EXCLUDE"; then continue; fi
  if [ -n "${KAI_DOC_PAGES:-}" ]; then
    case " $KAI_DOC_PAGES " in *" $rel "*) ;; *) continue ;; esac
  fi
  topic="$(printf '%s' "$rel" | tr '/.' '__')"
  extract_blocks "$page" "$topic" "$rel" "$WORK"
  for block in "$WORK/$topic"/*.body; do
    [ -f "$block" ] || continue
    printf '%s\t%s\n' "$topic" "$block" >> "$worklist"
  done
done

total=$(wc -l < "$worklist" | tr -d ' ')
results="$WORK/results"
: > "$results"
if [ "$total" -gt 0 ]; then
  while IFS="$(printf '\t')" read -r topic block; do
    printf '%s\0%s\0' "$topic" "$block"
  done < "$worklist" \
    | xargs -0 -P "$JOBS" -n2 "$self" __check "$WORK" > "$results"
fi

ok=$(grep -c '^OK$' "$results" 2>/dev/null || true)
fails=$(grep -c '^FAIL ' "$results" 2>/dev/null || true)
if [ "$fails" -gt 0 ]; then
  echo "test-doc-blocks: $fails failure(s) out of $total block(s)" >&2
  echo "---" >&2
  grep -v '^OK$' "$results" >&2
  exit 1
fi
echo "test-doc-blocks: OK — $ok/$total blocks"
