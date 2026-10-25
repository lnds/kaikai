#!/usr/bin/env bash
# Linux side: emit each probe's C through a capturing CC, and record the oracle's output.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:?usage: gen.sh <out-dir>}"
mkdir -p "$OUT/probes"
cp -R "$HOME/.kaikai/share/kaikai/include" "$OUT/include"

cat > "$OUT/cc-capture.sh" <<'EOF'
#!/bin/sh
D=${CAPTURE_DIR:?}
echo "$@" >> "$D/argv.txt"
for a in "$@"; do case "$a" in *.c) cp "$a" "$D/";; esac; done
out=""; prev=""; for a in "$@"; do [ "$prev" = "-o" ] && out=$a; prev=$a; done
[ -n "$out" ] && : > "$out" && chmod +x "$out"
exit 0
EOF
chmod +x "$OUT/cc-capture.sh"

for p in "$HERE"/probes/*.kai; do
  n=$(basename "$p" .kai)
  d="$OUT/probes/$n"; mkdir -p "$d"
  XDG_CACHE_HOME="$(mktemp -d)" CAPTURE_DIR="$d" CC="$OUT/cc-capture.sh" KAI_MODULAR=0 KAI_BACKEND=c \
    kai build "$p" -o "$d/ignored" >/dev/null
  rm -f "$d/ignored" "$d/runtime_owner_c.c"
  kai build --backend=c "$p" -o "$d/oracle" >/dev/null
  set +e
  (cd "$d" && printf 'first line\nsecond line\n' | KAI_THREADS=1 ./oracle > expected.out 2>expected.err; echo $? > expected.rc)
  set -e
  rm -f "$d/oracle"
  echo "$n: $(wc -c < "$d/out.c") bytes of C, rc $(cat "$d/expected.rc")"
done
