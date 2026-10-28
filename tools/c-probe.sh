#!/bin/sh
# tools/c-probe.sh — structural probes over the C the compiler prints.
#
#   c-probe.sh raw  <file.c> <ctype> <name>   a raw local of that C type, named after binder <name>
#   c-probe.sh fn   <file.c> <name-regex>     print the body of the first function matching
#
# A raw register is declared in a grouped line (`int64_t t0, r_a_0, t1;`), so a
# probe reads the declaration, never an initialiser.

set -eu

case "${1:-}" in
  raw)
    [ $# -eq 4 ] || { echo "usage: c-probe.sh raw <file.c> <ctype> <name>" >&2; exit 2; }
    grep -qE "(^|[[:space:]])$3 ([^;()=]*[ ,])?r_$4_[0-9]+[,;]" "$2"
    ;;
  fn)
    [ $# -eq 3 ] || { echo "usage: c-probe.sh fn <file.c> <name-regex>" >&2; exit 2; }
    awk -v re="^[A-Za-z_0-9 *]+ (kaiu_)?$3\\\\(.*\\\\) \\\\{\$" '$0 ~ re { on = 1 } on { print } on && /^}/ { exit }' "$2"
    ;;
  *)
    echo "usage: c-probe.sh raw|fn ..." >&2
    exit 2
    ;;
esac
