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
#   prototypes  a C unit assigns each `<symbol>` and its value thunk (against stage2/runtime.h)
#               and each `kaix_core_<name>` (against stage0/runtime_llvm.c)
#               to a `KaiValue *(*)(KaiValue *, ...)` of the declared arity,
#               compiled with -Werror: a missing entry, another arity or a
#               void return fails to compile. A line ending `raw=<params>><ret>`
#               (`i` an int64_t, `b` a KaiValue *, as the compiler's raw table
#               gives it) also binds `<symbol>_raw` and `kaix_core_<name>_raw` to
#               that exact shape. A `raw!=` line, a table shape contradicting the
#               declaration, fails outright; and every `_raw` entry the runtime
#               defines must be a manifest line, so the table cannot drift.
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
    case "$sig" in
      *" raw="*)
        local shape="${sig##* raw=}" rparams="" rret="" ch
        local pshape="${shape%%>*}" rshape="${shape##*>}"
        for ((i = 0; i < ${#pshape}; i++)); do
          ch="${pshape:$i:1}"
          case "$ch" in i) rparams="$rparams${rparams:+, }int64_t" ;; *) rparams="$rparams${rparams:+, }KaiValue *" ;; esac
        done
        [ -z "$rparams" ] && rparams="void"
        case "$rshape" in i) rret="int64_t" ;; *) rret="KaiValue *" ;; esac
        echo "$rret (*const kai_builtin_raw_${name})($rparams) = ${entry}_raw;" ;;
    esac
    case "$sym" in
      kai_core_*)
        local thunk="_${sym}_thunk"
        [ -n "$3" ] && thunk="kaix_${name}_thunk"
        echo "KaiValue *(*const kai_builtin_thunk_${name})(KaiValue *, KaiValue **, int) = $thunk;" ;;
    esac
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

# raw_symmetry <manifest>: every `<symbol>_raw` / `kaix_core_<name>_raw` the runtime defines has
# a manifest line carrying `raw=`, so a raw entry cannot exist without the compiler knowing it.
raw_symmetry() {
  local rc=0 sym
  for sym in $(grep -ho 'kai_core_[a-z0-9_]*_raw(' stage2/runtime.h stage2/runtime/*.h stage2/llvm/*.h | tr -d '(' | sort -u); do
    grep -q " ${sym%_raw} .* raw=" "$1" || { echo "raw entry $sym has no manifest line with raw="; rc=1; }
  done
  for sym in $(grep -ho 'kaix_core_[a-z0-9_]*_raw(' stage0/runtime_llvm.c | tr -d '(' | sort -u); do
    local name=${sym#kaix_core_}; name=${name%_raw}
    grep -q "^$name .* raw=" "$1" || { echo "native raw entry $sym has no manifest line with raw="; rc=1; }
  done
  if grep -q ' raw!=' "$1"; then grep ' raw!=' "$1"; echo "a raw shape contradicts its declaration"; rc=1; fi
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
  scratch_core "$tmp/good" 'extern "kai"("kai_core_vec_empty") pub fn vec_empty[a]() : Vec[a]'
  scratch_core "$tmp/arity" 'extern "kai"("kai_core_vec_empty") pub fn vec_empty[a](x: Int) : Vec[a]'
  scratch_core "$tmp/native" 'extern "kai"("kai_core_vec_empty") pub fn kai_no_native_entry[a]() : Vec[a]'
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
if ! out=$(raw_symmetry "$rows"); then
  echo "kai-builtins FAIL — the raw entries and the manifest disagree:"
  echo "$out"
  exit 1
fi
echo "kai-builtins OK ($(wc -l < "$rows" | tr -d ' ') declared builtins, each matching its C and native entry)"
