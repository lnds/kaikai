#!/bin/sh
# tools/deep-match-fixture.sh — print a program whose `pick` nests N matches
# in their last arms; it prints `zero` then `deep`.
#
#   deep-match-fixture.sh <N>
#
# Generated rather than kept under examples/: the formatter's layout probes
# cost time exponential in this nesting, and every examples/ file is
# formatted by the fmt property sweep.
set -eu
n="$1"
printf 'fn pick(k: Int) : String =\n  '
i=0
while [ "$i" -lt "$n" ]; do printf 'match k { 0 -> "zero"  _ -> '; i=$((i + 1)); done
printf '"deep"'
i=0
while [ "$i" -lt "$n" ]; do printf ' }'; i=$((i + 1)); done
printf '\n\nfn main() : Unit / Stdout = {\n  Stdout.print(pick(0))\n  Stdout.print(pick(7))\n}\n'
