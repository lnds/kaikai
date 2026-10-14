#!/bin/sh
# The modular C link takes its macOS-only stack flag from the target the
# C compiler builds for, not from the host: a Mac cross-linking for Linux
# must not hand its linker `-stack_size`, and a compiler that targets
# macOS gets it on any host. The fake compiler answers `-dumpmachine`
# with the triple under test, logs every line, and forwards the rest to
# the real `cc` without the flag, so the binary links on either host.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

real_cc="$(command -v cc)"
cat > "$work/fakecc" <<EOF
#!/bin/sh
echo "\$*" >> "$work/cc.log"
for a in "\$@"; do [ "\$a" = -dumpmachine ] && { echo "\$FAKE_TRIPLE"; exit 0; }; done
for a in "\$@"; do
  shift
  case "\$a" in -Wl,-stack_size,*) ;; *) set -- "\$@" "\$a" ;; esac
done
exec "$real_cc" "\$@"
EOF
chmod +x "$work/fakecc"
printf 'fn main() {\n  println("linked")\n}\n' > "$work/hello.kai"

fail=0
# $1 triple, $2 "with" or "without" the stack flag on the link line.
case_for() {
  : > "$work/cc.log"
  out="$work/hello-$1"
  (cd "$work" && FAKE_TRIPLE="$1" CC="$work/fakecc" KAI_MODULAR=1 XDG_CACHE_HOME="$work/cache-$1" \
    "$KAI" build --backend=c hello.kai -o "$out") || { echo "cc-target-link FAIL ($1): build failed"; fail=1; return; }
  [ "$("$out")" = linked ] || { echo "cc-target-link FAIL ($1): the binary did not run"; fail=1; }
  link="$(grep -e "-o $out" "$work/cc.log" || true)"
  [ -n "$link" ] || { echo "cc-target-link FAIL ($1): no link line logged"; fail=1; return; }
  has=without
  case "$link" in *-Wl,-stack_size,*) has=with ;; esac
  [ "$has" = "$2" ] || { echo "cc-target-link FAIL ($1): linked $has the stack flag"; fail=1; }
}

case_for x86_64-unknown-linux-gnu without
case_for arm64-apple-darwin23.0.0 with
[ "$fail" = 0 ] && echo "cc-target-link OK"
exit "$fail"
