#!/bin/sh
# A module whose handle inherits a default carries a copy of the default's
# clauses, so its typed key moves with the default's body even when the edit
# leaves every type alone:
#
# 1. cold and warm builds equal the plain build;
# 2. after the default's body is edited, the cached build equals the plain
#    build of the edited tree and differs from the build before the edit.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

write_effect() {
  cat > "$PROJ/d.kai" <<EOF
pub effect Gauge {
  read() : Int
  default {
    read(resume) -> resume(string_length("$1"))
  }
}
EOF
}

cat > "$PROJ/e.kai" <<'EOF'
import d

pub fn run() : Int = handle {
  Gauge.read()
} with Gauge {
}
EOF

cat > "$PROJ/main.kai" <<'EOF'
import e

fn main() : Unit / Console {
  println(int_to_string(e.run()))
}
EOF

mkdir -p "$PROJ/.kai-cache"

build() {
  out="$1"; shift
  if ! "$KAIC2" $EDITION_FLAG "$@" --path "$PROJ" "$PROJ/main.kai" > "$out" 2>"$PROJ/err"; then
    echo "typedc_default_clause_edit: FAIL — build exited non-zero ($*)"
    cat "$PROJ/err"
    exit 1
  fi
}

same() {
  if ! cmp -s "$1" "$2"; then
    echo "typedc_default_clause_edit: FAIL — $3"
    exit 1
  fi
}

write_effect abc
build "$PROJ/plain.c"
build "$PROJ/cold.c" --user-cache --user-cache-dir "$PROJ/.kai-cache"
build "$PROJ/warm.c" --user-cache --user-cache-dir "$PROJ/.kai-cache"
same "$PROJ/plain.c" "$PROJ/cold.c" "cold --user-cache C differs from plain C"
same "$PROJ/plain.c" "$PROJ/warm.c" "warm --user-cache C differs from plain C"

write_effect abcd
build "$PROJ/edit.c" --user-cache --user-cache-dir "$PROJ/.kai-cache"
build "$PROJ/edit_plain.c"
same "$PROJ/edit_plain.c" "$PROJ/edit.c" "edited default: --user-cache C differs from plain C"
if cmp -s "$PROJ/plain.c" "$PROJ/edit.c"; then
  echo "typedc_default_clause_edit: FAIL — the edit of the default did not change the build"
  exit 1
fi

echo "typedc_default_clause_edit: OK"
exit 0
