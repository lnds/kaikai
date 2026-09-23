#!/bin/bash
# Closure-capture shape generator, fed to the native-vs-C parity harness.
#
# Emits one program per point of the product
#
#   origin  param | let | flat | ctor     where the captured binder `v` comes from
#   capture none | read | cmp | ret       what a lambda does with `v`
#   call    norec | tail | nontail | mutual   the capturing arm's recursive call
#   value   rec | var | list | int        the shape of `v`
#
# and runs tools/test-backend-parity.sh over them. Each program is a list
# walk `drain(acc, msgs)` whose arm binds `v` and feeds `acc` through the
# capture into the call; the lambda is built in the call's argument list.
# C is the oracle: a native divergence in stdout or exit code is a native
# bug, whatever the right answer is. Exits non-zero when any shape diverges.
#
# Usage: tools/closure-shape-gen.sh [emit <dir>]

set -eu

ORIGINS="param let flat ctor"
CAPTURES="none read cmp ret"
CALLS="norec tail nontail mutual"
VALUES="rec var list int"

value_decls() {
  case "$1" in
    rec) printf '#[derive(Eq, Show)]\ntype Rec = { a: Int, b: Int }\n\n' ;;
    var) printf '#[derive(Eq, Show)]\ntype Var = Lo(Int) | Hi(Int)\n\n' ;;
  esac
}

value_type() {
  case "$1" in
    rec) echo "Rec" ;;
    var) echo "Var" ;;
    list) echo "[Int]" ;;
    int) echo "Int" ;;
  esac
}

# The value built from Int expression $2; `value_of <shape> 0` is the one
# the walk removes, `value_other` the one it keeps.
value_of() {
  case "$1" in
    rec) echo "Rec { a: $2, b: 1 }" ;;
    var) echo "Lo($2)" ;;
    list) echo "[$2]" ;;
    int) echo "$2" ;;
  esac
}

value_other() {
  case "$1" in
    rec) echo "Rec { a: 1, b: 1 }" ;;
    var) echo "Hi(1)" ;;
    list) echo "[1, 1]" ;;
    int) echo "1" ;;
  esac
}

# An Int read out of binder $2, by projection rather than by equality.
value_key() {
  case "$1" in
    rec) echo "$2.a" ;;
    var) echo "match $2 { Lo(k) -> k Hi(k) -> k }" ;;
    list) echo "length($2)" ;;
    int) echo "$2" ;;
  esac
}

capture_op() {
  case "$1" in
    none) echo "list_append(acc, [v])" ;;
    read) echo "filter(acc, l => $(value_key "$2" l) != $(value_key "$2" v))" ;;
    cmp) echo "filter(acc, l => l != v)" ;;
    ret) echo "map(acc, l => v)" ;;
  esac
}

# The arm's result: the new accumulator $2 handed to the call shape $1;
# $3 is the extra argument the `param` origin threads through.
call_expr() {
  case "$1" in
    norec) echo "$2" ;;
    tail) echo "drain($2$3, rest)" ;;
    nontail) echo "list_append(drain($2$3, rest), [])" ;;
    mutual) echo "relay($2$3, rest)" ;;
  esac
}

drive_type() {
  case "$1" in
    param|let) echo "[Int]" ;;
    flat) echo "[$2]" ;;
    ctor) echo "[Msg]" ;;
  esac
}

emit_program() {
  local origin="$1" capture="$2" call="$3" value="$4"
  local t v0 extra="" sig_extra="" tail="rest" body
  t="$(value_type "$value")"
  v0="$(value_of "$value" 0)"
  [ "$origin" = param ] && extra=", v" && sig_extra=", v: $t"
  [ "$call" = norec ] && tail="_"
  body="$(call_expr "$call" "$(capture_op "$capture" "$value")" "$extra")"

  value_decls "$value"
  [ "$origin" = ctor ] && printf 'type Msg = Wire(%s) | Skip\n\n' "$t"
  printf 'fn drain(acc: [%s]%s, msgs: %s) : [%s] = match msgs {\n' \
    "$t" "$sig_extra" "$(drive_type "$origin" "$t")" "$t"
  printf '  [] -> acc\n'
  case "$origin" in
    param) printf '  [_, ...%s] -> %s\n' "$tail" "$body" ;;
    let) printf '  [h, ...%s] -> {\n    let v = %s\n    %s\n  }\n' \
      "$tail" "$(value_of "$value" h)" "$body" ;;
    flat) printf '  [v, ...%s] -> %s\n' "$tail" "$body" ;;
    ctor)
      printf '  [Skip, ...%s] -> %s\n' "$tail" "$(call_expr "$call" acc "")"
      printf '  [Wire(v), ...%s] -> %s\n' "$tail" "$body" ;;
  esac
  printf '}\n\n'
  if [ "$call" = mutual ]; then
    printf 'fn relay(acc: [%s]%s, msgs: %s) : [%s] = drain(acc%s, msgs)\n\n' \
      "$t" "$sig_extra" "$(drive_type "$origin" "$t")" "$t" "$extra"
  fi
  printf 'fn main() : Unit / Stdout {\n'
  printf '  let all = [%s, %s]\n' "$v0" "$(value_other "$value")"
  case "$origin" in
    param) printf '  Stdout.print("left: #{drain(all, %s, [0, 0])}")\n' "$v0" ;;
    let) printf '  Stdout.print("left: #{drain(all, [0, 0])}")\n' ;;
    flat) printf '  Stdout.print("left: #{drain(all, [%s, %s])}")\n' "$v0" "$v0" ;;
    ctor) printf '  Stdout.print("left: #{drain(all, [Wire(%s), Skip])}")\n' "$v0" ;;
  esac
  printf '}\n'
}

emit_all() {
  local dir="$1" o c p v
  mkdir -p "$dir"
  for o in $ORIGINS; do for c in $CAPTURES; do for p in $CALLS; do for v in $VALUES; do
    emit_program "$o" "$c" "$p" "$v" > "$dir/${o}_${c}_${p}_${v}.kai"
  done; done; done; done
}

# Per-axis tally of the divergent programs, read from their file names.
axis_tally() {
  local list="$1" idx name
  for idx in 1 2 3 4; do
    name="$(echo "origin capture call value" | cut -d' ' -f"$idx")"
    printf '  %-8s' "$name"
    sed 's/\.kai$//' "$list" | cut -d_ -f"$idx" | sort | uniq -c \
      | awk '{ printf " %s=%s", $2, $1 }'
    printf '\n'
  done
}

report() {
  local log="$1" dir="$2" work="$3" n m k
  n="$(ls "$dir"/*.kai | wc -l | tr -d ' ')"
  grep '^FAIL .* (oracle) build failed' "$log" | awk '{print $2}' \
    | xargs -n1 basename 2>/dev/null | sort > "$work/nocompile" || true
  grep '^FAIL ' "$log" | grep -v '(oracle) build failed' | awk '{print $2}' \
    | xargs -n1 basename 2>/dev/null | sort > "$work/diverge" || true
  m=$((n - $(wc -l < "$work/nocompile")))
  k="$(wc -l < "$work/diverge" | tr -d ' ')"
  echo "closure-shape-gen: generated=$n compile=$m diverge=$k"
  if [ "$k" -gt 0 ]; then
    echo "divergent shapes (origin_capture_call_value):"
    sed 's/^/  /' "$work/diverge"
    echo "per-axis tally of the divergent shapes:"
    axis_tally "$work/diverge"
  fi
  if [ -s "$work/nocompile" ]; then
    echo "shapes the C oracle rejects:"
    sed 's/^/  /' "$work/nocompile"
  fi
}

WORK=""

main() {
  local root out log
  root="$(cd "$(dirname "$0")/.." && pwd)"
  if [ "${1:-}" = emit ]; then
    emit_all "${2:?usage: closure-shape-gen.sh emit <dir>}"
    return
  fi
  out="$root/stage2/build/closure-shapes"
  WORK="$(mktemp -d)"
  trap 'rm -rf "$WORK"' EXIT INT TERM
  rm -rf "$out"
  emit_all "$out"
  log="$WORK/parity.log"
  BACKEND_PARITY_DIRS="$out" "$root/tools/test-backend-parity.sh" > "$log" 2>&1 || true
  if grep -q '^test-backend-parity: SKIP' "$log"; then
    cat "$log"
    return 1
  fi
  if [ -n "${CLOSURE_SHAPE_LOG:-}" ]; then
    cp "$log" "$CLOSURE_SHAPE_LOG"
  fi
  report "$log" "$out" "$WORK"
  [ ! -s "$WORK/diverge" ]
}

main "$@"
