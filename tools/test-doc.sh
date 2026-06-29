#!/bin/sh
# tools/test-doc.sh — smoke test for `kai doc` and `kai --help`.
#
# Asserts:
#   1. `kai --help` is clean — no "command not found" / stray subprocess
#      output leaking from an unescaped heredoc (the #807 regression).
#   2. `kai doc` (no args) lists stdlib modules.
#   3. `kai doc <module>` lists a module's pub items with synopses.
#   4. `kai doc <module>.<symbol>` shows the signature + doc body.
#   5. The extended --doc-json carries the `sig` field for a fn.
#   6. A bad symbol / bad module exits non-zero with an actionable hint.
#
# Catches: heredoc backtick regressions in usage(), broken cmd_doc
# dispatch, JSON-field offset drift in the awk extractor, a dropped
# `sig` field in doc_attr.kai.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KAI="$ROOT/bin/kai"

fail() {
  echo "test-doc: FAIL: $1" >&2
  exit 1
}

# 1. `kai --help` must be clean. The #807 wiring left unescaped
#    backticks in usage(), so `cat <<EOF` ran `kai build`, `kai run`,
#    and `c`/`native`/`llvm` as commands. Assert none of that leaks.
help_out="$("$KAI" --help 2>&1)" || fail "'kai --help' exited non-zero"
echo "$help_out" | grep -qi "command not found" \
  && fail "'kai --help' leaks 'command not found' (unescaped heredoc backticks)"
echo "$help_out" | grep -q "missing input file" \
  && fail "'kai --help' ran a subcommand from an unescaped heredoc"
echo "$help_out" | grep -q "^usage: kai" || fail "'kai --help' missing usage line"
echo "$help_out" | grep -q "^  doc " || fail "'kai --help' does not list the doc command"

# 2. `kai doc` with no args lists modules.
list_out="$("$KAI" doc 2>&1)" || fail "'kai doc' exited non-zero"
echo "$list_out" | grep -q "^Modules:" || fail "'kai doc' missing 'Modules:' header"
echo "$list_out" | grep -q "core/string" || fail "'kai doc' did not list core/string"
echo "$list_out" | grep -q "stream" || fail "'kai doc' did not list stream"

# 3. `kai doc <module>` lists items with synopses.
mod_out="$("$KAI" doc string 2>&1)" || fail "'kai doc string' exited non-zero"
echo "$mod_out" | grep -q "^# string" || fail "'kai doc string' missing module header"
echo "$mod_out" | grep -q "split" || fail "'kai doc string' did not list split"

# 4. `kai doc <module>.<symbol>` shows signature + doc.
sym_out="$("$KAI" doc string.split 2>&1)" || fail "'kai doc string.split' exited non-zero"
echo "$sym_out" | grep -q "^# string.split" || fail "missing symbol header"
echo "$sym_out" | grep -q "split(s: String, sep: String) -> \[String\]" \
  || fail "'kai doc string.split' missing/incorrect signature"
echo "$sym_out" | grep -q "non-overlapping" || fail "missing doc body text"

# A cross-tree module addressed by basename and by relative path.
"$KAI" doc stream.fold 2>&1 | grep -q "fold(s: Stream" \
  || fail "'kai doc stream.fold' missing signature"
"$KAI" doc core/string.split 2>&1 | grep -q "^# core/string.split" \
  || fail "'kai doc core/string.split' (path form) did not resolve"

# 5. --doc-json carries the `sig` field for a fn (issue #774 follow-up).
if command -v python3 >/dev/null 2>&1; then
  "$ROOT/stage2/kaic2" --doc-json "$ROOT/examples/doc/doc_json_basic.kai" 2>/dev/null \
    | python3 -c "
import sys, json
d = json.load(sys.stdin)
fns = [it for it in d['items'] if it['kind'] == 'fn']
if not fns:
    sys.stderr.write('no fn items\n'); sys.exit(1)
documented = next((it for it in fns if it['name'] == 'documented'), None)
if not documented or documented.get('sig') != '(x: Int) -> Int':
    sys.stderr.write('sig field missing/wrong: %r\n' % (documented and documented.get('sig')))
    sys.exit(1)
wrapper = next((it for it in d['items'] if it['name'] == 'Wrapper'), None)
if wrapper and wrapper.get('sig') is not None:
    sys.stderr.write('non-fn item should carry sig=null\n'); sys.exit(1)
" || fail "--doc-json sig field regressed"
else
  echo "test-doc: warning: python3 not found; skipping --doc-json sig check" >&2
fi

# 6. Error paths exit non-zero with an actionable hint.
if "$KAI" doc string.nosuchsymbol >/dev/null 2>&1; then
  fail "bad symbol should exit non-zero"
fi
"$KAI" doc string.nosuchsymbol 2>&1 | grep -q "no symbol" \
  || fail "bad symbol missing 'no symbol' hint"
if "$KAI" doc nosuchmodule >/dev/null 2>&1; then
  fail "bad module should exit non-zero"
fi
"$KAI" doc nosuchmodule 2>&1 | grep -qE "no module .* in the stdlib" \
  || fail "bad module missing 'no module ... in the stdlib' hint"

# 7. A PACKAGE module that imports a STDLIB module documents (issue #976).
#    `--doc-json` for a package module must see the stdlib search root the
#    same way `kai check` / `kai build` do; otherwise `import spawn` fails
#    to resolve, the extractor exits non-zero, and `kai doc` prints nothing
#    — while `kai check` on the same module passes.
pkg_doc_dir="$ROOT/examples/packages/doc_stdlib_import"
( cd "$pkg_doc_dir" && "$KAI" doc pkg/clean >/dev/null 2>&1 ) \
  || fail "'kai doc pkg/clean' (no-import package module) exited non-zero"
pkg_imp_out="$( cd "$pkg_doc_dir" && "$KAI" doc pkg/withimport 2>&1 )" \
  || fail "'kai doc pkg/withimport' (package module importing stdlib) exited non-zero"
echo "$pkg_imp_out" | grep -q "^# pkg/withimport" \
  || fail "'kai doc pkg/withimport' missing module header"
echo "$pkg_imp_out" | grep -q "Imports a stdlib module" \
  || fail "'kai doc pkg/withimport' missing module doc"
echo "$pkg_imp_out" | grep -q "go" \
  || fail "'kai doc pkg/withimport' did not list the go item"

echo "test-doc: OK — --help clean, doc list/module/symbol views, sig field, error paths, package-module stdlib import"
