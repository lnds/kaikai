#!/bin/bash
# Core front gate. Every core and imported library function, passed through
# the front-end passes on its own and resolved against the environment the
# whole stream's resolution ended in, must equal what the stream holds
# (`kaic2 --check-core-front`). The programs cover a record hoist, string
# interpolation, effects, derived protocols and several imported modules.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION="$(cat "$ROOT/EDITION")"
[ -x "$KAIC2" ] || { echo "core-front: SKIP — no stage2/kaic2"; exit 0; }

PROGRAMS="
examples/perceus/vec_write_record_field_linear.kai
examples/perceus/derived_json_pad_and_call_share_site.kai
examples/effects/capability_param_multi_instance.kai
examples/stdlib/money_basic.kai
examples/imports/double_import_prelude_json.kai
examples/protocols/free_fn_bound_satisfied.kai
examples/sugars/kinds_dim_matrix.kai
"

fail=0
libs=0
for p in $PROGRAMS; do
  dir="$(dirname "$p")"
  out="$(cd "$dir" && "$KAIC2" --edition "$EDITION" --path . --path "$ROOT/stdlib" --check-core-front "$(basename "$p")" 2>&1 | grep -E '^core-front|differs')"
  case "$out" in
    *"fused or split 0:"*) ;;
    *) echo "  FAIL — $p: a function was fused or split, which a body completed on its own does not get: $out"; fail=1 ;;
  esac
  line="$(echo "$out" | grep -E '^core-front: [0-9]+ core' | head -1)"
  case "$line" in
    *" 0 differ") ;;
    *) echo "  FAIL — $p: $out"; fail=1 ;;
  esac
  # The tables built from the stream with the bodies emptied are the ones built from the whole stream.
  off="$(cd "$dir" && "$KAIC2" --edition "$EDITION" --path . --path "$ROOT/stdlib" --check-core-front "$(basename "$p")" 2>/dev/null | awk '{print $1, $2}' | sort)"
  on="$(cd "$dir" && "$KAIC2" --edition "$EDITION" --path . --path "$ROOT/stdlib" --check-core-front-stubs "$(basename "$p")" 2>/dev/null | awk '{print $1, $2}' | sort)"
  [ "$off" = "$on" ] || { echo "  FAIL — $p: a function completed against the emptied stream differs from the whole one"; fail=1; }
  n="$(echo "$line" | sed -n 's/.* \([0-9][0-9]*\) library functions.*/\1/p')"
  libs=$((libs + ${n:-0}))
done

[ "$libs" -gt 0 ] || { echo "core-front: FAIL — no library function was checked"; exit 1; }
[ "$fail" -eq 0 ] || { echo "core-front: FAIL"; exit 1; }
echo "core-front OK"
