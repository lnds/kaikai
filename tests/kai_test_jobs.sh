#!/bin/sh
# `kai test` runs at most as many compilers at once as its job count.
#
# Every compiler `kai test` starts (the per-file checks and the builds)
# goes through a shim kaic2 that logs when it starts and ends and holds
# each check for a second, so concurrent ones overlap. The run must stay
# within the job count it was given, spelled `-j <n>` or `-j<n>`, and
# reach it; without one, within the default of one job per CPU and at
# most one per 3 GiB of memory.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

# A dev layout whose kaic2 is the shim; `kai` resolves its root from its
# own path, so it is copied, not linked.
lay="$work/root"
mkdir -p "$lay/stage2" "$lay/bin"
for e in "$ROOT"/*; do
  case "$(basename "$e")" in
    stage2|bin) ;;
    *) ln -s "$e" "$lay/$(basename "$e")" ;;
  esac
done
for e in "$ROOT"/stage2/*; do
  [ "$(basename "$e")" = kaic2 ] || ln -s "$e" "$lay/stage2/$(basename "$e")"
done
cp "$ROOT/bin/kai" "$lay/bin/kai"
log="$work/calls.log"
cat > "$lay/stage2/kaic2" <<EOF
#!/bin/sh
echo "start \$\$" >> "$log"
case " \$* " in *" --check "*) sleep 1 ;; esac
"$ROOT/stage2/kaic2" "\$@"
rc=\$?
echo "end \$\$" >> "$log"
exit \$rc
EOF
chmod +x "$lay/stage2/kaic2"

pkg="$work/pkg"
mkdir -p "$pkg/tests"
printf 'name = "jobs"\nentry = "main.kai"\n' > "$pkg/kai.toml"
printf 'fn main() : Unit / Stdout = Stdout.print("")\n' > "$pkg/main.kai"
for i in 1 2 3 4 5 6; do
  printf 'test "t%s" {\n  assert %s == %s\n}\n' "$i" "$i" "$i" > "$pkg/tests/t${i}_test.kai"
done

# The most compilers alive at once, from the start/end log.
peak() {
  awk '$1 == "start" { n++; if (n > m) m = n } $1 == "end" { n-- } END { print m + 0 }' "$log"
}

fail=0
peak_seen=0
run() {
  label="$1"; want="$2"; shift 2
  : > "$log"
  rc=0
  (cd "$pkg" && rm -rf .kai-cache && KAI_BACKEND=c "$lay/bin/kai" test "$@") >"$work/out" 2>&1 || rc=$?
  got=$(peak)
  if [ "$rc" -ne 0 ]; then
    echo "  FAIL $label: kai test exited $rc"; tail -5 "$work/out"; fail=$((fail + 1)); return
  fi
  if [ "$got" -gt "$want" ]; then
    echo "  FAIL $label: $got compilers at once, limit $want"; fail=$((fail + 1)); return
  fi
  echo "  ok   $label: at most $got at once (limit $want)"
  peak_seen=$got
}

run "-j 2" 2 -j 2
[ "$peak_seen" -eq 2 ] || { echo "  FAIL -j 2 never ran two at once; the check proves nothing"; fail=$((fail + 1)); }
run "-j2" 2 -j2
run "-j 1" 1 -j 1

cpus=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
mem=$(sysctl -n hw.memsize 2>/dev/null || awk '/MemTotal/ { print $2 * 1024 }' /proc/meminfo)
fit=$((mem / (3 * 1024 * 1024 * 1024)))
[ "$fit" -ge 1 ] || fit=1
[ "$fit" -lt "$cpus" ] || fit=$cpus
run "default" "$fit"

if [ "$fail" -ne 0 ]; then
  echo "kai-test-jobs FAIL ($fail)"
  exit 1
fi
echo "kai-test-jobs OK"
