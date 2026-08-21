#!/bin/sh
# tools/test-posix-shell.sh — every script that declares #!/bin/sh must
# parse under a strict POSIX shell. macOS /bin/sh is bash 3.2 in POSIX
# mode and accepts constructs dash (the Linux sh running these scripts
# in CI) rejects at parse time — e.g. an unescaped backquote command
# substitution inside a heredoc, which made every bin/kai invocation
# die with "Syntax error: end of file unexpected".
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if command -v dash >/dev/null 2>&1; then
  checker="dash -n"
else
  checker="sh -n"
  echo "posix-shell: dash not found; falling back to sh -n (weaker — install dash for the CI-exact check)" >&2
fi

err="$(mktemp)"
trap 'rm -f "$err"' EXIT INT TERM

fail=0
pass=0
for f in "$ROOT"/bin/kai "$ROOT"/tests/*.sh; do
  [ -f "$f" ] || continue
  head -1 "$f" | grep -q '^#!/bin/sh' || continue
  name="${f#"$ROOT"/}"
  if $checker "$f" 2>"$err"; then
    pass=$((pass + 1))
  else
    echo "  FAIL $name — not POSIX-parseable:"
    sed 's/^/      /' "$err"
    fail=$((fail + 1))
  fi
done

if [ "$fail" -gt 0 ]; then
  echo "posix-shell: $pass passed, $fail failed"
  exit 1
fi
echo "posix-shell: $pass #!/bin/sh scripts parse clean ($checker)"
