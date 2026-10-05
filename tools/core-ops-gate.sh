#!/bin/bash
# Core protocol-operation gate. Pruning reaches a protocol through the
# names of its operations; one missed operation drops the impls a call
# needs. For every operation of every protocol the core declares,
# generated from those declarations, a program calls it directly on a
# primitive that has an impl, and must build and run with pruning on
# whenever it builds with the whole core.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"
WORK="$ROOT/stage2/build/core-ops"
EDITION="$(cat "$ROOT/EDITION")"
[ -x "$KAIC2" ] || { echo "core-ops: SKIP — no stage2/kaic2"; exit 0; }
rm -rf "$WORK"; mkdir -p "$WORK"

printf 'fn main() : Int = 0\n' > "$WORK/probe.kai"
core="$(cd "$WORK" && "$KAIC2" --guard-core-inputs probe.kai 2>/dev/null | grep '\.kai$')"

# protocol|op|params|return, one line per declared operation.
awk '/^pub protocol /{p=$3; sub(/\[.*/, "", p); inb=1; next}
     inb && /^}/{inb=0}
     inb && /^  [a-z_]+\(/{
       line=$0; sub(/^  /, "", line)
       op=line; sub(/\(.*/, "", op)
       params=line; sub(/^[a-z_]+\(/, "", params); sub(/\) : .*/, "", params)
       ret=line; sub(/.*\) : /, "", ret)
       print p "|" op "|" params "|" ret }' $core > "$WORK/ops"

prim_of() { # the first primitive with an impl of protocol $1
  for t in Int Real Bool Char String; do
    grep -qE "^impl $1(\[[^]]*\])? for $t\b" $core && { echo "$t"; return; }
  done
}

lit_of() {
  case "$1" in
    Int) echo 3 ;; Real) echo 1.5 ;; Bool) echo true ;; Char) echo "'a'" ;; String) echo '"ab"' ;;
  esac
}

arg_of() { # $1 parameter type, $2 primitive
  case "$1" in
    Self|a) lit_of "$2" ;;
    Int) echo 0 ;;
    String) echo '"x"' ;;
    "Array[Byte]") echo "to_bytes($(lit_of "$2"))" ;;
    *) echo "" ;;
  esac
}

pass=0; skip=0; fail=0

check() { # $1 label, $2 callee, $3 primitive, $4 result type, $5 arguments
  local dir="$WORK/$1"; mkdir -p "$dir"
  printf 'fn main() : Unit / Stdout = {\n  let _v : %s = %s(%s)\n  Stdout.print("ok")\n}\n' "$4" "$2" "$5" > "$dir/main.kai"
  if ! (cd "$dir" && "$KAIC2" --edition "$EDITION" --full-core main.kai > full.c 2> full.err); then
    echo "  skip $1 on $3 — does not build with the whole core"; skip=$((skip+1)); return
  fi
  if ! (cd "$dir" && "$KAIC2" --edition "$EDITION" main.kai > main.c 2> main.err); then
    echo "  FAIL $1 on $3 — builds whole, not pruned:"; sed 's/^/    /' "$dir/main.err" | head -5
    fail=$((fail+1)); return
  fi
  if ! cc -std=c99 -w -O0 -I "$ROOT/stage2" -I "$ROOT/stage0" "$dir/main.c" -o "$dir/main" -lm 2> "$dir/cc.err" \
     || [ "$("$dir/main" 2>/dev/null)" != ok ]; then
    echo "  FAIL $1 on $3 — the pruned build does not compile or run"; fail=$((fail+1)); return
  fi
  pass=$((pass+1))
}
while IFS='|' read -r proto op params ret; do
  prim="$(prim_of "$proto")"
  if [ -z "$prim" ]; then echo "  skip $proto.$op — no primitive impl"; skip=$((skip+1)); continue; fi
  args=""; ok=1
  IFS=',' read -ra ps <<< "$params"
  for pdecl in ${ps[@]+"${ps[@]}"}; do
    [ -z "${pdecl// /}" ] && continue
    ty="${pdecl#*: }"; a="$(arg_of "$ty" "$prim")"
    [ -n "$a" ] || { ok=0; break; }
    args="${args:+$args, }$a"
  done
  [ "$ok" = 1 ] || { echo "  skip $proto.$op — no argument for a parameter"; skip=$((skip+1)); continue; }
  check "$proto.$op" "$op" "$prim" "${ret//Self/$prim}" "$args"
done < "$WORK/ops"

total="$(wc -l < "$WORK/ops" | tr -d ' ')"
[ "$total" -gt 0 ] || { echo "core-ops: FAIL — no protocol operation found in the core"; exit 1; }
[ "$fail" -eq 0 ] || { echo "core-ops: FAIL — $fail of $total operations"; exit 1; }
echo "core-ops OK — $pass of $total operations built pruned and ran; $skip skipped"
