#!/bin/sh
# Gate for the kai binary (tools/kai): `kai env`, the plugin contract, the
# dispatch of an unknown verb to `kai-<verb>` on PATH (`kai upgrade` among
# them), build/run, the dev-loop and source-tool verbs — in a dev checkout
# and in an installed prefix.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
PASS=0
FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() { printf '  FAIL  %s\n' "$1"; FAIL=$((FAIL + 1)); }
ok()   { printf '  ok    %s\n' "$1"; PASS=$((PASS + 1)); }

# expect <label> <want-status> <want-output> <command...>: stdout+stderr
# must equal <want-output> exactly.
expect() {
  label="$1"; want_status="$2"; want="$3"; shift 3
  status=0
  got="$("$@" 2>&1)" || status=$?
  if [ "$status" -ne "$want_status" ]; then
    fail "$label (exit $status, want $want_status)"
    printf '%s\n' "$got" | sed 's/^/        /'
  elif [ "$got" != "$want" ]; then
    fail "$label (output differs)"
    printf '        want: %s\n        got:  %s\n' "$want" "$got"
  else
    ok "$label"
  fi
}

# status <command...>: the exit status alone, output discarded.
status_of() {
  st=0
  "$@" >/dev/null 2>&1 || st=$?
  echo "$st"
}

# test_ids <dir> <kai test args...>: the ids `kai test --json` reports, sorted, space-separated.
test_ids() {
  (cd "$1" && shift && "$KAI" test --backend=c --json "$@" 2>/dev/null) \
    | sed -n 's/^{"type":"test","id":"\([^"]*\)".*/\1/p' | LC_ALL=C sort | tr '\n' ' '
}

# expect_line <label> <want-status> <want-first-line> <command...>
expect_line() {
  label="$1"; want_status="$2"; want="$3"; shift 3
  status=0
  got="$("$@" 2>&1)" || status=$?
  first="$(printf '%s\n' "$got" | sed -n 1p)"
  if [ "$status" -ne "$want_status" ] || [ "$first" != "$want" ]; then
    fail "$label (exit $status, first line '$first')"
  else
    ok "$label"
  fi
}

case "$(uname -s)" in
  Darwin) tid="$(stat -f '%m-%z' "$ROOT/stage2/kaic2")" ;;
  *)      tid="$(stat -c '%Y-%s' "$ROOT/stage2/kaic2")" ;;
esac

# The dev checkout's resolution, with no overrides in play.
unset KAI_STDLIB KAIKAI_HOME
expect "env: dev checkout" 0 "KAIKAI_HOME=$ROOT
KAI_STDLIB=$ROOT/stdlib
KAI_TOOLCHAIN_ID=$tid
KAI_KAIC2=$ROOT/stage2/kaic2" "$KAI" env
expect "env <name>: one value per line" 0 "$tid
$ROOT" "$KAI" env KAI_TOOLCHAIN_ID KAIKAI_HOME
expect "env: unknown name" 2 \
  "kai: error: unknown variable 'NOPE' (known: KAIKAI_HOME, KAI_STDLIB, KAI_TOOLCHAIN_ID, KAI_KAIC2)" \
  "$KAI" env KAI_STDLIB NOPE
expect "env: KAI_STDLIB overrides" 0 "/elsewhere" env KAI_STDLIB=/elsewhere "$KAI" env KAI_STDLIB
expect "env: KAIKAI_HOME names another prefix" 0 "$TMP/home" \
  env KAIKAI_HOME="$TMP/home" "$KAI" env KAIKAI_HOME

# Plugins: argv verbatim, the `kai env` variables exported, the exit
# status passed through.
mkdir -p "$TMP/plugins" "$TMP/noexec"
cat > "$TMP/plugins/kai-hello" <<'EOF'
#!/bin/sh
for a in "$@"; do printf '[%s]' "$a"; done
printf '\n%s|%s|%s|%s\n' "$KAIKAI_HOME" "$KAI_STDLIB" "$KAI_TOOLCHAIN_ID" "$KAI_KAIC2"
exit 7
EOF
chmod +x "$TMP/plugins/kai-hello"
printf '#!/bin/sh\necho hijacked\n' > "$TMP/plugins/kai-fmt"
chmod +x "$TMP/plugins/kai-fmt"
printf '#!/bin/sh\necho ran\n' > "$TMP/noexec/kai-quiet"

expect "plugin: argv, env, exit status" 7 "[a b][--flag][]
$ROOT|$ROOT/stdlib|$tid|$ROOT/stage2/kaic2" env PATH="$TMP/plugins:$PATH" "$KAI" hello "a b" --flag ""
expect_line "plugin: never shadows a core verb" 0 "usage: kai fmt [--width N] <file.kai>            # rewrite file in place" \
  env PATH="$TMP/plugins:$PATH" "$KAI" fmt --help
expect_line "plugin: a non-executable file is skipped" 2 "kai: error: unknown command: quiet" \
  env PATH="$TMP/noexec:$PATH" "$KAI" quiet
expect_line "unknown command" 2 "kai: error: unknown command: frobnicate" "$KAI" frobnicate
expect_line "a flag is never a plugin" 2 "kai: error: unknown command: --hello" \
  env PATH="$TMP/plugins:$PATH" "$KAI" --hello
expect_line "help" 0 " _      _ _      _" "$KAI" help
expect_line "--version" 0 "kaikai $(cat "$ROOT/VERSION") - $(cat "$ROOT/EDITION") (stage 2, self-hosted)" "$KAI" --version
expect_line "info --list" 0 "$(ls "$ROOT/docs/info" | sed -n 's/\.md$//p' | LC_ALL=C sort | sed -n 1p)" "$KAI" info --list
expect "fmt --stdin" 0 "fn main() : Int = 0" sh -c 'printf "fn main()   :  Int = 0\n" | "$1" fmt --stdin' _ "$KAI"
expect "fmt --stdin --check: unformatted is exit 1, nothing printed" 1 "" \
  sh -c 'printf "fn main()   :  Int = 0\n" | "$1" fmt --stdin --check' _ "$KAI"
expect "fmt --stdin --check: formatted is exit 0" 0 "" \
  sh -c 'printf "fn main() : Int = 0\n" | "$1" fmt --stdin --check' _ "$KAI"
expect_line "no command: usage, exit 2" 2 " _      _ _      _" "$KAI"

# build/run through the binary.
mkdir -p "$TMP/prog"
cat > "$TMP/prog/args.kai" <<'KAI'
import os.args
fn main() : Int / Stdout + Env = {
  args.argv() |> foreach(a => Stdout.print("[#{a}]"))
  7
}
KAI
printf 'fn main() : Unit / Stdout = Stdout.print("hi")\n' > "$TMP/prog/hello.kai"
expect "run (c): args verbatim, exit status passed through" 7 "[a b]
[--x]" "$KAI" run --backend=c "$TMP/prog/args.kai" "a b" --x
# The default backend: native, or C with a note on a kaic2 without libLLVM.
status=0
got="$("$KAI" run "$TMP/prog/args.kai" "a b" --x 2>/dev/null)" || status=$?
if [ "$status" -eq 7 ] && [ "$got" = "[a b]
[--x]" ]; then ok "run (default backend): args, exit status"; else fail "run (default backend): exit $status, got '$got'"; fi
# Parallel compiles into a cold cache: every status must reach kai even though
# the runtime reaps children whenever a fiber parks on file I/O.
stats="$(cd "$TMP/prog" && KAI_MODULAR=1 KAI_MODULAR_STATS=1 KAI_MODULAR_JOBS=8 \
  KAI_MODULAR_CACHE_DIR="$TMP/mc" "$KAI" build --backend=c hello.kai -o hello 2>&1 && ./hello)" || true
case "$stats" in
  *"cache hits=0 compiled="*"hi") ok "build: parallel c-modular compiles into a cold cache" ;;
  *) fail "build: parallel c-modular compiles into a cold cache"; printf '%s\n' "$stats" | sed 's/^/        /' ;;
esac
# watch passes the args after the spec to every run of the program.
"$KAI" watch --backend=c "$TMP/prog/args.kai" "a b" --x > "$TMP/watch.out" 2>&1 &
wpid=$!
i=0
while [ "$i" -lt 240 ] && ! grep -q "Program exited with status 7" "$TMP/watch.out"; do
  sleep 0.5
  i=$((i + 1))
done
kill "$wpid" 2>/dev/null || true
wait "$wpid" 2>/dev/null || true
case "$(cat "$TMP/watch.out")" in
  *"[a b]"*"[--x]"*"Program exited with status 7"*) ok "watch: the args after the spec reach the program" ;;
  *) fail "watch: program args"; sed 's/^/        /' "$TMP/watch.out" ;;
esac
# --strict-holes: a hole fails the build and the typecheck instead of warning.
printf 'fn f(x: Int) : Int = ?\nfn main() : Int = 0\n' > "$TMP/prog/hole.kai"
got="$(status_of "$KAI" build --backend=c "$TMP/prog/hole.kai" -o "$TMP/prog/hole") $(status_of "$KAI" build --backend=c --strict-holes "$TMP/prog/hole.kai" -o "$TMP/prog/hole")"
got="$got $(status_of "$KAI" typecheck "$TMP/prog/hole.kai") $(status_of "$KAI" typecheck --strict-holes "$TMP/prog/hole.kai")"
if [ "$got" = "0 1 0 1" ]; then ok "--strict-holes fails build and typecheck on a hole"; else fail "--strict-holes: statuses '$got', want '0 1 0 1'"; fi
printf 'fn f(x: Int) : Int = x\n# @probe type 1:22\nfn main() : Int = 0\n' > "$TMP/prog/probe.kai"
case "$("$KAI" typecheck --library-mode "$TMP/prog/probe.kai" 2>&1)" in
  '{"file": '*'"type": "Int"}]}') ok "typecheck --library-mode answers the probes" ;;
  *) fail "typecheck --library-mode" ;;
esac
traces="$(KAI_TRACE_RC=1 "$KAI" run "$TMP/prog/hello.kai" 2>&1 | grep -c 'KAI_TRACE_RC\] alloc_total=' || true)"
if [ "$traces" = "1" ]; then ok "run: the RC trace is the program's alone"; else fail "run: $traces RC trace lines, want 1"; fi

# The dev-loop verbs over a package: entry, tests/ sibling, discovered *_test.kai.
Q="$TMP/ws/pkg"
mkdir -p "$Q/tests" "$TMP/ws/lib/tests"
printf 'name = "tp"\n' > "$Q/kai.toml"
printf 'pub fn one() : Int = 1\n' > "$Q/util.kai"
printf 'import util\nfn main() : Int = util.one() - 1\ntest "entry" {\n  assert util.one() == 1\n}\n' > "$Q/main.kai"
printf 'import util\ntest "extra" {\n  assert util.one() == 1\n}\n' > "$Q/extra_test.kai"
printf 'test "sibling" {\n  assert true\n}\nfn main() : Int = 0\n' > "$Q/tests/a.kai"
printf 'name = "tl"\n' > "$TMP/ws/lib/kai.toml"
printf 'pub fn one() : Int = 1\n' > "$TMP/ws/lib/lib.kai"
printf 'import lib\ntest "lib" {\n  assert lib.one() == 1\n}\n' > "$TMP/ws/lib/lib_test.kai"
printf 'fn main() : Int = 0\n' > "$TMP/ws/lib/tests/t.kai"
got="$(cd "$Q" && "$KAI" test --backend=c 2>&1)" && status=0 || status=$?
case "$status:$got" in
  *"not reachable"*) fail "test: the entry is never reported unreachable"; printf '%s\n' "$got" | sed 's/^/        /' ;;
  0:*"/tests/a.kai"*"== kai test extra_test.kai"*) ok "test: entry, tests/ sibling, discovered *_test.kai" ;;
  *) fail "test: package run (exit $status)"; printf '%s\n' "$got" | sed 's/^/        /' ;;
esac
expect_line "test --json --only: no match is exit 1" 1 "kai: no test matched --only" \
  sh -c 'cd "$1" && { "$2" test --backend=c --json --only nomatch 2>err >/dev/null; rc=$?; tail -1 err; exit $rc; }' _ "$Q" "$KAI"
expect_line "test --only: no match is exit 1" 1 "kai: no test matched --only" \
  sh -c 'cd "$1" && { "$2" test --backend=c --only nomatch >err 2>&1; rc=$?; tail -1 err; exit $rc; }' _ "$Q" "$KAI"
expect_line "test ./...: every package" 0 "kai: all package tests passed (2 package(s))" \
  sh -c 'cd "$1" && { "$2" test --backend=c ./... >"$3" 2>&1; rc=$?; tail -1 "$3"; exit $rc; }' _ "$TMP/ws" "$KAI" "$TMP/rec.log"
expect "typecheck: a library checks every module it owns" 0 "kai: lib.kai
kai: lib_test.kai
kai: tests/t.kai" sh -c 'cd "$1" && "$2" typecheck' _ "$TMP/ws/lib" "$KAI"
# A JSON report is one document per line, one line per compilation root.
got="$(cd "$TMP/ws/lib" && "$KAI" lint --json 2>/dev/null)"
if [ "$(printf '%s\n' "$got" | grep -c '^\[.*\]$')" = 3 ] && [ "$(printf '%s\n' "$got" | wc -l | tr -d ' ')" = 3 ]; then
  ok "lint --json: a library is one array per module, one per line"
else
  fail "lint --json over a library"; printf '%s\n' "$got" | sed 's/^/        /'
fi
# An entry-less package is linted module by module; every module's findings are reported.
L="$ROOT/examples/lint/pkg_entry"
expect "lint: a library lints every module it owns" 0 "$(cat "$L/lint.expected")" \
  sh -c 'cd "$1" && "$2" lint . 2>/dev/null | sed "s|^.*/pkg_entry/||"' _ "$L" "$KAI"
# A package with an entry is one unit, and a non-entry file's own findings
# are still reported.
U="$ROOT/examples/lint/pkg_unit_scope"
expect "lint: a package with an entry is linted as one unit" 0 "$(cat "$U/lint.expected")" \
  sh -c 'cd "$1" && "$2" lint . 2>/dev/null | sed "s|^.*/pkg_unit_scope/||"' _ "$U" "$KAI"
got="$(cd "$TMP/ws/lib" && "$KAI" typecheck --diags-json 2>/dev/null)"
if [ "$(printf '%s\n' "$got" | grep -c '^{"file": .*"diagnostics": .*}$')" = 3 ] && [ "$(printf '%s\n' "$got" | wc -l | tr -d ' ')" = 3 ]; then
  ok "typecheck --diags-json: a library is one object per module, one per line"
else
  fail "typecheck --diags-json over a library"; printf '%s\n' "$got" | sed 's/^/        /'
fi
# Block ids name the file relative to where kai test runs; ./... owns --json and --only.
expect "test ids: relative to the package run" 0 "extra_test.kai:extra main.kai:entry tests/a.kai:sibling " test_ids "$Q"
expect "test ids: led by the sub-package dir" 0 "pkg/extra_test.kai:extra pkg/main.kai:entry pkg/tests/a.kai:sibling " test_ids "$TMP/ws" ./pkg
expect "test ./... --json: every package's records, ids led by its dir" 0 \
  "lib/lib_test.kai:lib pkg/extra_test.kai:extra pkg/main.kai:entry pkg/tests/a.kai:sibling " test_ids "$TMP/ws" ./...
expect "test ./... --only: selects across packages" 0 "lib/lib_test.kai:lib pkg/main.kai:entry " \
  test_ids "$TMP/ws" --only lib/lib_test.kai:lib --only pkg/main.kai:entry ./...
expect_line "test ./... --only: no match is exit 1" 1 "kai: no test matched --only" \
  sh -c 'cd "$1" && { "$2" test --backend=c --only nomatch ./... >"$3" 2>&1; rc=$?; tail -1 "$3"; exit $rc; }' _ "$TMP/ws" "$KAI" "$TMP/rec.log"
expect "bench: --iters must be positive" 2 \
  "kai: error: --iters value must be a positive integer (got: 0)" "$KAI" bench --iters 0 x.kai
expect "check: --backend is validated" 2 \
  "kai: error: --backend must be 'c' or 'native' (got: llvm)" "$KAI" check --backend llvm x.kai

# A package's cache lives under the compiler's toolchain id: an entry another
# compiler wrote is removed, never read, and both the parse and the typed
# blobs land in the current toolchain's directory.
C="$TMP/cachekey"
mkdir -p "$C/.kai-cache/0-0"
printf 'name = "ck"\n' > "$C/kai.toml"
printf 'pub fn two() : Int = 2\n' > "$C/util.kai"
printf 'import util\nfn main() : Int = util.two() - 2\n' > "$C/main.kai"
printf 'x' > "$C/.kai-cache/0-0/tm-0.kab"
printf 'x' > "$C/.kai-cache/stale.kab"
tid="$("$KAI" env KAI_TOOLCHAIN_ID)"
(cd "$C" && "$KAI" build --backend=c . -o "$TMP/ck.bin" >/dev/null 2>&1) || true
got="$(ls "$C/.kai-cache")"
if [ "$got" = "$tid" ] && ls "$C/.kai-cache/$tid" | grep -q '^tm-' \
   && ls "$C/.kai-cache/$tid" | grep -q '^[0-9a-f]*-[0-9a-f]*\.kab$'; then
  ok "cache: a package's entries live under the toolchain id; another toolchain's are removed"
else
  fail "cache keyed by toolchain id"; printf '        tid %s, .kai-cache: %s\n' "$tid" "$(ls -R "$C/.kai-cache" | tr '\n' ' ')"
fi

# mutate checks each mutant with the package's search paths, so a module in a
# subdirectory still resolves the package root and its dependencies.
M="$TMP/mut"
mkdir -p "$M/app/sub" "$M/dep"
printf 'name = "td"\n' > "$M/dep/kai.toml"
printf 'pub fn one() : Int = 1\n' > "$M/dep/depone.kai"
printf 'name = "tm"\n\n[dependencies]\ntd = { path = "../dep" }\n' > "$M/app/kai.toml"
printf 'pub fn two() : Int = 2\n' > "$M/app/base.kai"
printf 'import base\nimport depone\npub fn five() : Int = base.two() * 2 + depone.one()\n' > "$M/app/sub/leaf.kai"
printf 'import nosuch\npub fn six() : Int = nosuch.six() + 1\n' > "$M/app/sub/broken.kai"
got="$(cd "$M/app" && "$KAI" mutate --module sub/leaf.kai --operator literal --oracle false --json 2>/dev/null)" || true
case "$got" in
  '{"mutants": 1, "killed": 1, "compile_failed": 0,'*) ok "mutate: a subdirectory module resolves the package root and its dependencies" ;;
  *) fail "mutate: subdirectory module"; printf '        got: %s\n' "$got" ;;
esac
got="$(cd "$M/app" && "$KAI" mutate --module sub/broken.kai --operator literal --oracle false --json 2>&1)" || true
case "$got" in
  *"sub/broken.kai does not typecheck as it stands; skipped"*'{"mutants": 0,'*) ok "mutate: a module that does not typecheck unmutated is skipped" ;;
  *) fail "mutate: unmutated module that does not typecheck"; printf '%s\n' "$got" | sed 's/^/        /' ;;
esac

# A module without sites lists nothing and counts no mutant.
N="$ROOT/examples/mutate"
expect "mutate: --list over a module without sites is empty" 0 "$(cat "$N/no_sites.sites.expected")" \
  sh -c 'cd "$1" && "$2" mutate --list --module no_sites.kai 2>/dev/null' _ "$N" "$KAI"
got="$(cd "$N" && "$KAI" mutate --module no_sites.kai --oracle true --json 2>/dev/null)" || true
case "$got" in
  '{"mutants": 0, "killed": 0, "compile_failed": 0,'*) ok "mutate: a module without sites counts no mutant" ;;
  *) fail "mutate: module without sites"; printf '        got: %s\n' "$got" ;;
esac

# The driver surface: `--list --json` prints one JSON object per site and
# line, and `--apply` prints the module with exactly that site's byte span
# replaced. Multibyte text ahead of every site keeps bytes and characters apart.
U="$TMP/mutdrv"
mkdir -p "$U" "$TMP/mutant"
printf 'name = "tdrv"\n' > "$U/kai.toml"
cat > "$U/greet.kai" <<'EOF'
# Saludo: ñandú, café, 🎉 — multibyte text before every site.
fn label(n: Int) : String = if "ñ" != "é" and n >= 2 { "muchos 🎉" } else { "uno ñ" }

fn main() = print(label(2))
EOF
printf 'pub fn two() : Int = 1 + 1\n' > "$U/helper.kai"
if command -v python3 >/dev/null 2>&1; then
  status=0
  got="$(cd "$U" && python3 - "$KAI" "$TMP/mutant/greet.kai" 2>&1 <<'PY'
import json, subprocess, sys
kai, mutant = sys.argv[1], sys.argv[2]
def mutate(*args):
    r = subprocess.run([kai, "mutate", *args], capture_output=True)
    assert r.returncode == 0, (args, r.stderr)
    return r.stdout
def ndjson(out):
    return [json.loads(l) for l in out.decode().splitlines()]
src = open("greet.kai", "rb").read()
sites = ndjson(mutate("--list", "--json", "--module", "greet.kai"))
keys = {"id", "file", "line", "col", "operator", "start", "end", "original",
        "replacement", "enclosing", "ordinal", "description"}
assert sites, "no sites listed"
for s in sites:
    assert set(s) == keys, s
    a, b = s["start"]["byte"], s["end"]["byte"]
    assert src[a:b].decode() == s["original"], s
    assert s["start"]["col"] == a - src.rfind(b"\n", 0, a), s
    want = src[:a] + s["replacement"].encode() + src[b:]
    assert mutate("--apply", str(s["id"]), "--module", "greet.kai") == want, s
cmp = [s for s in sites if s["operator"] == "compare"]
assert [(s["original"], s["replacement"], s["enclosing"]) for s in cmp] == [(">=", ">", "greet.label/1")], cmp
line = src[src.rfind(b"\n", 0, cmp[0]["start"]["byte"]) + 1:cmp[0]["start"]["byte"]]
assert len(line.decode()) != len(line), "no multibyte text ahead of the site on its line"
assert ndjson(mutate("--list", "--json", "--operator", "compare", "--module", "greet.kai")) == cmp
listed = {s["file"].rsplit("/", 1)[-1] for s in ndjson(mutate("--list", "--json"))}
assert listed == {"greet.kai", "helper.kai"}, listed
open(mutant, "wb").write(mutate("--apply", str(cmp[0]["id"]), "--module", "greet.kai"))
PY
)" || status=$?
  if [ "$status" -eq 0 ]; then
    ok "mutate: --list --json carries each site's byte span, and --apply splices exactly it"
  else
    fail "mutate: --list --json / --apply roundtrip"; printf '%s\n' "$got" | sed 's/^/        /'
  fi
  expect "mutate: the applied mutant builds and behaves as the listed site says" 0 "uno ñ" \
    "$KAI" run --backend=c "$TMP/mutant/greet.kai"
else
  echo "test-kai-cli: warning: python3 not found; skipping the mutate JSON roundtrip" >&2
fi
expect_line "mutate: --apply names a site the module does not have" 1 "mutate: no site 99 (the file has 7)" \
  sh -c 'cd "$1" && "$2" mutate --apply 99 --module greet.kai' _ "$U" "$KAI"
# `enclosing` names the module by its path in the package, so two files of
# one name in different directories never share a key.
H="$TMP/homonym_util"
mkdir -p "$H/a" "$H/b"
printf 'name = "homonym_util"\n' > "$H/kai.toml"
printf '#[doc("Two or more.")]\npub fn f(n: Int) : Bool = n >= 2\n' > "$H/a/util.kai"
cp "$H/a/util.kai" "$H/b/util.kai"
got="$(cd "$H" && for m in a/util.kai b/util.kai; do "$KAI" mutate --list --json --operator compare --module "$m"; done 2>&1 | grep -o '"enclosing": "[^"]*"' | tr '\n' ' ')"
case "$got" in
  '"enclosing": "a.util.f/1" "enclosing": "b.util.f/1" ') ok "mutate: enclosing names the module by its package path" ;;
  *) fail "mutate: enclosing of same-named modules"; printf '        got: %s\n' "$got" ;;
esac
# `--limit` caps a listing across modules as it caps a run: greet.kai has 7 sites, helper.kai 2.
all="$(cd "$U" && "$KAI" mutate --list --json 2>&1)"
expect "mutate: --list --json --limit caps the sites across modules" 0 "$(printf '%s\n' "$all" | head -n 8)" \
  sh -c 'cd "$1" && "$2" mutate --list --json --limit 8' _ "$U" "$KAI"
all="$(cd "$U" && "$KAI" mutate --list 2>&1)"
expect "mutate: --list --limit caps the text listing" 0 "$(printf '%s\n' "$all" | head -n 3)" \
  sh -c 'cd "$1" && "$2" mutate --list --limit 3' _ "$U" "$KAI"

# A dev checkout whose kaic2 exists needs nothing from stages 0-1: its
# stage0/ holds no Makefile, so any attempt to rebuild kaic0 fails.
D="$TMP/dev"
mkdir -p "$D/bin" "$D/stage0" "$D/stage2"
cp "$ROOT/bin/kai" "$D/bin/kai"
ln -s "$ROOT/stdlib" "$D/stdlib"
ln -s "$ROOT/stage2/kaic2" "$D/stage2/kaic2"
expect "dev checkout: an existing kaic2 skips stages 0-1" 0 "fn main() = 0" \
  sh -c 'printf "fn main()=0\n" | "$1" fmt --stdin' _ "$D/bin/kai"

# An installed prefix: the binary resolves the prefix it sits in, never
# the checkout it was built in, and ignores a KAIKAI_HOME naming another.
P="$TMP/prefix"
mkdir -p "$P/bin" "$P/libexec/kaikai" "$P/share/kaikai/stdlib"
cp "$ROOT/bin/kai" "$P/bin/kai"
printf '#!/bin/sh\n' > "$P/libexec/kaikai/kaic2"
chmod +x "$P/libexec/kaikai/kaic2"
case "$(uname -s)" in
  Darwin) ptid="$(stat -f '%m-%z' "$P/libexec/kaikai/kaic2")" ;;
  *)      ptid="$(stat -c '%Y-%s' "$P/libexec/kaikai/kaic2")" ;;
esac
expect "env: installed prefix" 0 "KAIKAI_HOME=$P
KAI_STDLIB=$P/share/kaikai/stdlib
KAI_TOOLCHAIN_ID=$ptid
KAI_KAIC2=$P/libexec/kaikai/kaic2" env KAIKAI_HOME="$TMP/home" "$P/bin/kai" env
got="$("$P/bin/kai" --version | sed -n 's/^native p2: *//p')"
if [ "$got" = "optout (this installation ships no runtime bitcode)" ]; then
  ok "--version: an installation without the bitcode says so"
else
  fail "--version p2 line in an installation: '$got'"
fi
# Every name a release installs is reserved, whether or not this prefix has it.
for name in $(sed -n -E 's#.*"\$STAGE/(bin|libexec/kaikai)/([^"]+)".*#\2#p' "$ROOT/scripts/build-release.sh" \
  | sed 's#.*/##' | LC_ALL=C sort -u); do
  mkdir -p "$TMP/reserved/$name"
  printf 'name = "%s"\n' "$name" > "$TMP/reserved/$name/kai.toml"
  printf 'fn main() : Int = 0\n' > "$TMP/reserved/$name/main.kai"
  expect_line "install: '$name' is reserved in an installation" 1 \
    "kai: error: refusing to install '$name': the name is reserved by the kaikai toolchain" \
    "$P/bin/kai" install "$TMP/reserved/$name"
done
ln -s "$P/bin/kai" "$TMP/kai-link"
expect "env: through a symlinked bin/kai" 0 "$P" "$TMP/kai-link" env KAIKAI_HOME
mkdir -p "$TMP/brew/bin"
ln -s ../../prefix/bin/kai "$TMP/brew/bin/kai"
expect "env: through a relative symlink" 0 "$P" "$TMP/brew/bin/kai" env KAIKAI_HOME
expect "env: found on PATH by name" 0 "$P" sh -c 'cd / && PATH="$1:$PATH" kai env KAIKAI_HOME' _ "$TMP/brew/bin"
mkdir -p "$TMP/loose/bin"
cp "$ROOT/bin/kai" "$TMP/loose/bin/kai"
expect_line "not an installation" 2 "kai: error: cannot find kaikai installation under $TMP/loose" \
  "$TMP/loose/bin/kai" env

# `kai upgrade` is a plugin the toolchain ships: found before PATH, it
# upgrades the prefix kai exports.
C="$TMP/Cellar/kaikai/0.0.1"
mkdir -p "$C/bin" "$C/libexec/kaikai/plugins" "$C/share/kaikai/stdlib"
cp "$ROOT/bin/kai" "$C/bin/kai"
cp "$ROOT/tools/kai/plugins/kai-upgrade" "$C/libexec/kaikai/plugins/kai-upgrade"
printf '#!/bin/sh\n' > "$C/libexec/kaikai/kaic2"
chmod +x "$C/libexec/kaikai/kaic2"
expect "upgrade: the plugin acts on the prefix kai exports" 0 "kai is installed via Homebrew ($C/bin/kai).
Run 'brew upgrade kaikai' to update it." "$C/bin/kai" upgrade
printf '#!/bin/sh\necho hijacked\n' > "$TMP/plugins/kai-upgrade"
chmod +x "$TMP/plugins/kai-upgrade"
expect_line "plugin: the toolchain's own comes before PATH" 0 "usage: kai upgrade" \
  env PATH="$TMP/plugins:$PATH" "$KAI" upgrade --help

printf 'test-kai-cli: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
