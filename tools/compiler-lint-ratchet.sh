#!/usr/bin/env bash
# `kai lint` findings over the compiler may only go down, entry by entry.
#
# tools/baselines/compiler-lint holds one file per finding class: the key is
# (file, rule, hash of the message and the flagged source line), the value how
# many findings share it. A finding moving lines keeps its key; a finding
# whose line is edited, or one swapped for another, changes it. A key with no
# entry or a higher count fails, and so does an entry whose findings are gone
# or fewer: delete or lower its file in the change that fixed them.
#
# Usage: compiler-lint-ratchet.sh [--update]   (--update rewrites the entries)

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/tools/baselines/compiler-lint"
KAI="${KAI:-$ROOT/bin/kai}"
[ -x "$KAI" ] || { echo "compiler-lint-ratchet: no $KAI; make bin/kai first" >&2; exit 2; }

(cd "$ROOT/stage2" && "$KAI" lint --json main.kai) | python3 -I "$ROOT/tools/compiler-lint-ratchet.py" "$ROOT" "${1:-}"
