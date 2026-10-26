#!/bin/sh
# tools/fixture-meta.sh <KEY> <fixture> — print one per-fixture fact from its
# sibling env file (see kai_corpus_meta in tools/lib/corpus.sh). KEY `CFLAGS`
# prints the shim paths absolute; non-zero when the fixture declares none.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. tools/lib/corpus.sh
key=${1:?usage: fixture-meta.sh <KEY> <fixture>}
fixture=${2:?usage: fixture-meta.sh <KEY> <fixture>}
fixture=${fixture#"$ROOT"/}
fixture=${fixture#../}
if [ "$key" = CFLAGS ]; then kai_corpus_cflags "$fixture"; else kai_corpus_meta "$fixture" "$key"; fi
