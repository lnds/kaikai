#!/bin/sh
# usage: gen-escape-literal.sh N
# A program whose one string literal holds N copies of `a\tb\n` (2N
# escapes) and prints its decoded length, 4N.
awk -v n="$1" 'BEGIN { printf "fn main() : Unit / Stdout {\n  let s = \""; for (i = 0; i < n; i++) printf "a\\tb\\n"; printf "\"\n  Stdout.print(\"#{s.length()}\")\n}\n" }'
