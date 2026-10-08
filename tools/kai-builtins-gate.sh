#!/bin/bash
# Declared-builtin gate.
#
# A core module declares a builtin with `extern "kai"("<symbol>") fn`. A
# call binds `<symbol>` on the C backend and `kaix_core_<name>` on the
# native one, boxed values in and out; nothing else checks either exists.
#
# Two checks over `kaic2 --kai-builtins` (one line per declaration:
# `<name> <symbol> <arity> <signature>`):
#
#   manifest    the lines match tools/kai-builtins.txt, so adding,
#               removing or retyping a declared builtin is a reviewed diff;
#   prototypes  a C unit assigns each `<symbol>` (against stage2/runtime.h)
#               and each `kaix_core_<name>` (against stage0/runtime_llvm.c)
#               to a `KaiValue *(*)(KaiValue *, ...)` of the declared arity,
#               compiled with -Werror: a missing entry, another arity or a
#               void return fails to compile.
#
# `--self-test` declares builtins in a scratch copy of the core modules and
# asserts the manifest check rejects any new declaration, the prototype
# check rejects a wrong arity and a missing native entry, and accepts a
# declaration whose entries exist.
set -u
cd "$(dirname "$0")/.."

KAIC2=${KAIC2:-stage2/kaic2}
BASELINE=tools/kai-builtins.txt
CC=${CC:-cc}

# proto_unit <manifest> <header to include> <entry prefix|""> : a C unit
# binding each declared entry to a typed function pointer.
proto_unit() {
  echo "#include \"$2\""
  local name sym arity sig params i
  while read -r name sym arity sig; do
    params=""
    for ((i = 0; i < arity; i++)); do params="$params${params:+, }KaiValue *"; done
    [ -z "$params" ] && params="void"
    local entry="$sym"
    [ -n "$3" ] && entry="$3$name"
    echo "KaiValue *(*const kai_builtin_proto_${name})($params) = $entry;"
  done < "$1"
}

# prototypes <manifest>: compile both units; prints cc's diagnostics.
prototypes() {
  local tmp rc=0
  tmp=$(mktemp -d)
  proto_unit "$1" runtime.h "" > "$tmp/c.c"
  proto_unit "$1" runtime_llvm.c kaix_core_ > "$tmp/native.c"
  for u in c native; do
    "$CC" -std=c99 -fsyntax-only -Werror -Wno-unused-function -Wno-unused-variable \
      -I stage2 -I stage0 "$tmp/$u.c" 2>&1 || rc=1
  done
  rm -rf "$tmp"
  return $rc
}

# manifest <stdlib dir|""> > lines
manifest() {
  if [ -n "$1" ]; then KAIKAI_STDLIB_PATH="$1" "$KAIC2" --kai-builtins
  else "$KAIC2" --kai-builtins; fi
}

# scratch_core <dir> <declarations>: the core modules, with the declarations
# appended to the last core module.
scratch_core() {
  mkdir -p "$1"
  cp -R stdlib "$1/stdlib"
  printf '\n%s\n' "$2" >> "$1/stdlib/effects/os.kai"
}

self_test() {
  local tmp rc=0
  tmp=$(mktemp -d)
  scratch_core "$tmp/good" 'extern "kai"("kai_core_string_concat") pub fn string_concat(a: String, b: String) : String'
  scratch_core "$tmp/arity" 'extern "kai"("kai_core_string_concat") pub fn string_concat(a: String) : String'
  scratch_core "$tmp/native" 'extern "kai"("kai_core_string_concat") pub fn kai_no_native_entry(a: String, b: String) : String'
  mkdir -p "$tmp/m"
  manifest "$tmp/good/stdlib" > "$tmp/m/good" || { echo "kai-builtins self-test FAIL — kaic2 rejected a core declaration"; rc=1; }
  if diff -q "$BASELINE" "$tmp/m/good" > /dev/null; then
    echo "kai-builtins self-test FAIL — a new declaration left the manifest unchanged"; rc=1
  fi
  prototypes "$tmp/m/good" > /dev/null || { echo "kai-builtins self-test FAIL — a declaration whose entries exist was rejected"; rc=1; }
  for bad in arity native; do
    manifest "$tmp/$bad/stdlib" > "$tmp/m/$bad"
    if prototypes "$tmp/m/$bad" > /dev/null; then
      echo "kai-builtins self-test FAIL — a declaration with the $bad defect passed the prototype check"; rc=1
    fi
  done
  rm -rf "$tmp"
  [ $rc -eq 0 ] && echo "kai-builtins self-test OK (a new declaration, a wrong arity and a missing native entry are rejected)"
  return $rc
}

[ -x "$KAIC2" ] || { echo "kai-builtins: SKIP — no $KAIC2"; exit 0; }
if [ "${1:-}" = "--self-test" ]; then
  self_test || exit 1
fi

rows=$(mktemp)
trap 'rm -f "$rows"' EXIT
manifest "" > "$rows" || { echo "kai-builtins FAIL — kaic2 --kai-builtins errored"; exit 1; }
if ! diff -u "$BASELINE" "$rows"; then
  echo "kai-builtins FAIL — the declared builtins differ from $BASELINE; review the diff and update the baseline"
  exit 1
fi
if ! out=$(prototypes "$rows"); then
  echo "kai-builtins FAIL — a declared builtin's runtime entry does not match its declaration:"
  echo "$out"
  exit 1
fi
echo "kai-builtins OK ($(wc -l < "$rows" | tr -d ' ') declared builtins, each matching its C and native entry)"
