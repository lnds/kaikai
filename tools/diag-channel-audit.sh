#!/bin/bash
# Every diagnostic the compiler prints goes through the one structured channel.
#
# A pass builds a `Diagnostic` (`mk_diag`), prints it with `render_diag` and
# returns it, so `--diags-json` carries what stderr shows. A function that
# prints an `error:` / `warning:` line itself (`diag_error`, `diag_warning`,
# `diag_error_from_src`, or an `eprint` of such a line) prints without
# returning anything the JSON document can hold.
#
# The few functions that print and record the diagnostic by hand are pinned
# in tools/baselines/diag-channel, one file per `<file>__<fn>` holding its
# reason. A site not listed fails; a listed site that no longer prints fails
# too (delete its file in that commit).
set -u
export LC_ALL=C
cd "$(dirname "$0")/.."

BASE=tools/baselines/diag-channel
PAT='(^|[^A-Za-z_])(diag_error|diag_warning|diag_error_from_src)[(]|eprint[(].*("|: )(error|warning): '
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

observed() {
  for f in stage2/compiler/*.kai; do
    b=$(basename "$f")
    [ "$b" = "diag.kai" ] && continue
    awk -v b="$b" -v pat="$PAT" '
      /^(pub(\([a-z]+\))? )?fn [A-Za-z0-9_]+/ { s=$0; sub(/^(pub(\([a-z]+\))? )?fn /, "", s); sub(/[^A-Za-z0-9_].*/, "", s); fn=s }
      /^[ \t]*#/ { next }
      $0 ~ pat { print b "__" fn }' "$f"
  done | sort -u
}

observed > "$tmp/obs" || exit 1
ls "$BASE" 2>/dev/null | grep -v '^README' | sort -u > "$tmp/pin"
new=$(comm -23 "$tmp/obs" "$tmp/pin")
stale=$(comm -13 "$tmp/obs" "$tmp/pin")
n=$(wc -l < "$tmp/obs" | tr -d ' ')
rc=0
if [ -n "$new" ]; then
  echo "diag-channel-audit FAIL — function(s) printing a diagnostic outside the channel; build it with mk_diag and print it with render_diag:"
  echo "$new" | sed 's/^/  /'
  rc=1
fi
if [ -n "$stale" ]; then
  echo "diag-channel-audit FAIL — stale baseline entry(ies), delete its file in $BASE:"
  echo "$stale" | sed 's/^/  /'
  rc=1
fi
[ $rc -eq 0 ] && echo "diag-channel-audit OK ($n pinned sites)"
exit $rc
