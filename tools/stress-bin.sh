#!/bin/sh
# stress-bin.sh <bin> <runs> <threads>: run a binary serially under the deadline, count failures.
bin=$1; runs=$2; threads=$3
here="$(cd "$(dirname "$0")" && pwd)"
fails=0
i=0
while [ "$i" -lt "$runs" ]; do
  KAI_THREADS=$threads "$here/kai-bounded.sh" 10 "$bin" > /dev/null 2> "$bin.err" || {
    fails=$((fails + 1))
    tail -1 "$bin.err"
  }
  i=$((i + 1))
done
echo "threads=$threads runs=$runs failures=$fails"
