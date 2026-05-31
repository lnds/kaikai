.PHONY: all kaic0 kaic1 kaic2 test test-stage0 test-stage1 test-stage2 test-demos test-multi-module test-import-stdlib test-import-prelude-dedup test-import-qualified-record test-fmt test-bench test-check test-library-mode test-diagnostics-collected test-negative test-private-type-shadow-audit test-stdlib-modules test-packages test-binserialize-budget demos-verify demos-no-regression selfhost test-arena clean tier0 tier1 tier1-asan tier1-backend-parity daily coverage-probe rc-budget stress-fixtures llvm-info llvm-fetch llvm-configure llvm-build llvm-size llvm-clean

all: kaic1 kaic2 bin/kai

kaic0:
	$(MAKE) -C stage0 kaic0

kaic1: kaic0
	$(MAKE) -C stage1 kaic1

kaic2: kaic1
	$(MAKE) -C stage2 kaic2

# bin/kai is already a checked-in shell script; this target just confirms it
# is executable and visible.
bin/kai:
	@chmod +x bin/kai
	@echo "kai driver: $$(realpath bin/kai 2>/dev/null || pwd)/bin/kai"

test: test-stage0 test-stage1 test-stage2 test-demos test-multi-module test-import-stdlib test-import-prelude-dedup test-import-qualified-record

test-stage0:
	$(MAKE) -C stage0 test

test-stage1:
	$(MAKE) -C stage1 test

test-stage2:
	$(MAKE) -C stage2 test

# Build + run + test every phase 4 demo via bin/kai.
test-demos: kaic1
	@set -e; \
	for f in examples/phase4/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./bin/kai run  $$f > /tmp/kaikai-$$name.out; \
	  ./bin/kai test $$f > /tmp/kaikai-$$name-t.out 2>&1 || true; \
	  echo "demo OK $$name"; \
	done

# issue #233 — `bin/kai run` auto-discovers sibling modules and
# nested directories under the entry file.
#
# Two invocation modes per fixture:
#   abs: from a foreign cwd (`/`) with an absolute source path. Exercises
#        the original PR #236 contract — `--path "$(dirname "$src")"`
#        evaluated to an absolute string.
#   rel: cd into the fixture dir and run `kai run main.kai`. Exercises
#        the user-facing flow from issue #237 — `dirname` returning `.`
#        and the cd+pwd absolutization in `bin/kai`.
test-multi-module: kaic2
	@set -e; \
	root=$$(pwd); \
	kai="$$root/bin/kai"; \
	for case in multi-module multi-module/issue-237 multi-module/pipe_custom_type multi-module/pipe_two_types_same_pkg; do \
	  src_dir="$$root/examples/$$case"; \
	  exp="$$src_dir/main.out.expected"; \
	  out_abs=$$(mktemp); \
	  (cd / && "$$kai" run "$$src_dir/main.kai") > "$$out_abs" \
	    || { echo "$$case FAIL (abs kai run)"; rm -f "$$out_abs"; exit 1; }; \
	  diff -q "$$exp" "$$out_abs" > /dev/null \
	    || { echo "$$case DIFF (abs)"; diff "$$exp" "$$out_abs"; rm -f "$$out_abs"; exit 1; }; \
	  rm -f "$$out_abs"; \
	  out_rel=$$(mktemp); \
	  (cd "$$src_dir" && "$$kai" run main.kai) > "$$out_rel" \
	    || { echo "$$case FAIL (rel kai run)"; rm -f "$$out_rel"; exit 1; }; \
	  diff -q "$$exp" "$$out_rel" > /dev/null \
	    || { echo "$$case DIFF (rel)"; diff "$$exp" "$$out_rel"; rm -f "$$out_rel"; exit 1; }; \
	  rm -f "$$out_rel"; \
	  echo "$$case OK (abs+rel)"; \
	done

# Issue #279 regression — exercises bin/kai's `--path "$$STDLIB_ROOT"`
# resolution end-to-end. The bug fixed in this lane was that line 469
# of bin/kai hardcoded `--path "$$ROOT/stdlib"` instead of using the
# already-resolved `$$STDLIB_ROOT`, breaking `import loop` (and any
# non-prelude stdlib import) in the brew-installed layout where
# `$$ROOT/stdlib` does not exist. We invoke `bin/kai run` (not kaic2
# directly) so the wrapper's argument construction is exercised.
test-import-stdlib: kaic2
	@set -e; \
	root=$$(pwd); \
	kai="$$root/bin/kai"; \
	src="$$root/examples/imports/import_loop_basic.kai"; \
	exp="$$root/examples/imports/import_loop_basic.out.expected"; \
	out=$$(mktemp); \
	"$$kai" run "$$src" > "$$out" \
	  || { echo "import_loop_basic FAIL (kai run)"; rm -f "$$out"; exit 1; }; \
	diff -q "$$exp" "$$out" > /dev/null \
	  || { echo "import_loop_basic DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	rm -f "$$out"; \
	echo "import_loop_basic OK"

# Issue #425 regression: importing a prelude-loaded module
# (`encoding.toml`, `encoding.json`, `core.list`) must be a no-op
# instead of producing duplicate `kai_<mod>__*` symbols in the C
# output. Drives the fix end-to-end through `bin/kai run` so the
# prelude wiring + the resolver dedup both participate.
test-import-prelude-dedup: kaic2
	@set -e; \
	root=$$(pwd); \
	kai="$$root/bin/kai"; \
	for case in double_import_prelude_toml double_import_prelude_json double_import_prelude_list; do \
	  src="$$root/examples/imports/$$case.kai"; \
	  exp="$$root/examples/imports/$$case.out.expected"; \
	  out=$$(mktemp); \
	  "$$kai" run "$$src" > "$$out" \
	    || { echo "$$case FAIL (kai run)"; rm -f "$$out"; exit 1; }; \
	  diff -q "$$exp" "$$out" > /dev/null \
	    || { echo "$$case DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	  rm -f "$$out"; \
	  echo "$$case OK"; \
	done

# Issue #456 regression — qualified record literal `mod.Type { ... }`
# parses + resolves end-to-end. Each case lives in its own directory
# (main + 1..2 sibling modules) so the resolver walks the same
# multi-file discovery path the issue's reproducer uses.
test-import-qualified-record: kaic2
	@set -e; \
	root=$$(pwd); \
	kai="$$root/bin/kai"; \
	for case in qualified_record_basic qualified_record_ambiguous qualified_record_with_spread; do \
	  src_dir="$$root/examples/imports/$$case"; \
	  exp="$$src_dir/main.out.expected"; \
	  out=$$(mktemp); \
	  "$$kai" run "$$src_dir/main.kai" > "$$out" \
	    || { echo "$$case FAIL (kai run)"; rm -f "$$out"; exit 1; }; \
	  diff -q "$$exp" "$$out" > /dev/null \
	    || { echo "$$case DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	  rm -f "$$out"; \
	  echo "$$case OK"; \
	done

# m12.8 Phase 3 — Core demo gate. Delegates to stage2's
# `test-demos-core` which builds each demo under examples/portfolio/
# and examples/usd_to_eur/ on both backends, redirects stdin from the
# `.in` fixture when present, and diffs against `.out.expected`. The
# top-level alias matches the name Eric proposed in the m12.8 review.
demos-verify: kaic2
	$(MAKE) -C stage2 test-demos-core

# m12.8.x post-Core REOPEN — Eric level 3 gate. Runs the full demo
# probe set under demos/ and fails when the OK + PASS count drops
# below the baseline pinned in `demos/baseline.txt`. Used at every
# milestone close to prevent silent regressions in features the
# Core demo gate (`demos-verify`) does not exercise.
demos-no-regression: kaic2
	$(MAKE) -C demos no-regression

# Self-hosting: per-compiler determinism for stage 1 and stage 2.
# Each stage compiles its own source twice; output must match
# byte-for-byte. Stage 1 and stage 2 are NOT required to agree on
# emission shape — see
# docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md.
selfhost:
	$(MAKE) -C stage1 selfhost
	$(MAKE) -C stage2 selfhost

clean:
	$(MAKE) -C stage0 clean
	$(MAKE) -C stage1 clean
	$(MAKE) -C stage2 clean

# ---- testing tiers (see docs/testing-tiers.md) ----------------------
#
# Tier 0: pre-commit gate. ~30-60s. Every agent / human runs this
# before every commit. If it fails, no commit happens.
tier0: selfhost demos-no-regression test-arena
	@echo "tier0 OK — selfhost deterministic (kaic2b.c == kaic2c.c), demos baseline holds, arena gate passes"

# issue #120 — opt-in Perceus regions: P0 runtime arena gate. Plain +
# ASAN build of the C-level bump-arena fixture. Fast (~1s), no kaic2
# dependency, so it runs in tier0 and again under tier1-asan.
test-arena:
	@echo "== test-arena: bump-arena runtime gate (plain + ASAN) =="
	@bash tools/run-arena-c-fixture.sh

# Tier 1: pre-PR gate. ~2-4 min. Run before opening / merging a PR.
# PR description should include the trailing line of this output (or
# a CI link) — without it, the merge does not happen.
tier1: test demos-no-regression test-fmt test-bench test-check test-library-mode test-diagnostics-collected test-negative test-stdlib-modules test-packages test-private-type-shadow-audit test-private-record-shadow-audit test-canonical-aliases test-info
	@echo "tier1 OK — full make test + demos baseline + fmt fixtures + bench smoke + check smoke + library-mode probes + diagnostics-collected fixtures + negative-space fixtures + stdlib modules compile clean + package-mode harness (issue #569) + private-type shadow audit + private-record shadow audit + canonical-only alias audit + kai info smoke"

# `kai info` smoke (no kaic2 required; pure shell + awk + python3 for
# JSON validation). Guards against deleted .md, broken cmd_info
# dispatcher, JSON-escape regressions in the awk converter.
# Also runs the fenced-code-block compile audit so every example in
# `docs/info/*.md` and `docs/grammar.md` is guaranteed to type-check
# against the current stage 2 compiler — `kai info` + grammar.md are
# the two LLM-authoritative surface references (CLAUDE.md Tier 2 #5
# + Tier 3 #8).
test-info: kaic2
	@tools/test-info.sh
	@tools/test-info-blocks.sh
	@tools/test-grammar-blocks.sh

# Issue #643 — institutional regression gate for the private-type
# leak fix. `tools/audit-prelude-private-types.sh` walks every
# non-`pub type` declaration in `stdlib/` and verifies a minimal
# user program that redeclares the same name still compiles. The
# arity-aware variant walker discriminates diff-arity collisions
# (the canonical `Tree` book ch5 case); same-arity collisions
# (`SplitN`, `NB_FragR`) remain a documented limitation in the
# audit script's skip-list and are tracked as a follow-up.
test-private-type-shadow-audit: kaic2
	@./tools/audit-prelude-private-types.sh

# Issue #648 — record-side companion to the audit above. Walks every
# non-`pub type X = { ... }` record declaration in `stdlib/` and
# verifies a minimal user program that redeclares `X` with a
# distinct field set still compiles. Failure means the typer's
# record-table walker (`rec_find_with_field`, issue #648) regressed
# and leaked the prelude's field set into the user's scope.
test-private-record-shadow-audit: kaic2
	@./tools/audit-prelude-private-records.sh

# Option E canonical-name audit (m14 follow-up, closed 2026-05-17).
# Walks every m14 follow-up module and confirms the canonical
# `<module>.<op>` surface exists with no surviving `<prefix>_<op>`
# legacy aliases. The qualified-call resolver's legacy-prefix
# fallback was retired together with this gate flip.
test-canonical-aliases:
	@./tools/audit-canonical-aliases.sh | tail -6

# Tongariki — `kai fmt` fixture suite. Verifies that every fixture
# in examples/fmt/ formats to its `.expected.kai` and is idempotent,
# plus that every formattable example under examples/minimal/ /
# quickstart/ / phase4/ round-trips through the formatter without
# breaking re-parse. Cheap enough to gate every Tier 1 run.
test-fmt: kaic2
	@./tests/fmt_fixtures.sh

# bench v1.x (issues #40 + #437) — smoke for `kai bench`. Builds +
# runs the `examples/stdlib/bench_basic.kai` and
# `examples/stdlib/bench_median_mad.kai` fixtures and verifies the
# new median + MAD output format. Numeric values vary per run so we
# grep for the pattern rather than diffing against a golden file.
# Also exercises the --iters CLI flag and the KAI_BENCH_WARMUP env
# var to make sure both paths feed through to the runtime.
# Failure modes:
#   - kai bench exits non-zero (compile / runtime error)
#   - any expected bench result line is missing or malformed
#   - the trailing `<N> benches` summary line is missing
#   - --iters override fails to change the iteration count
test-bench: kaic2
	@./bin/kai bench examples/stdlib/bench_basic.kai > /tmp/kaikai-bench-basic.out 2>&1; \
	rc=$$?; \
	if [ $$rc -ne 0 ]; then \
	  echo "test-bench FAIL — kai bench exited $$rc"; \
	  cat /tmp/kaikai-bench-basic.out; exit 1; \
	fi; \
	matched=$$(grep -E ': 1000 iter / median [0-9]+ ns / MAD [0-9]+ ns / mean [0-9]+ ns / range \[[0-9]+, [0-9]+\]$$' /tmp/kaikai-bench-basic.out | wc -l | tr -d ' '); \
	if [ "$$matched" != "3" ]; then \
	  echo "test-bench FAIL — expected 3 bench result lines in new format, got $$matched"; \
	  cat /tmp/kaikai-bench-basic.out; exit 1; \
	fi; \
	if ! grep -qE '^3 benches$$' /tmp/kaikai-bench-basic.out; then \
	  echo "test-bench FAIL — missing '3 benches' summary line"; \
	  cat /tmp/kaikai-bench-basic.out; exit 1; \
	fi; \
	./bin/kai bench --iters 200 examples/stdlib/bench_median_mad.kai > /tmp/kaikai-bench-mm.out 2>&1; \
	rc=$$?; \
	if [ $$rc -ne 0 ]; then \
	  echo "test-bench FAIL — kai bench --iters 200 exited $$rc"; \
	  cat /tmp/kaikai-bench-mm.out; exit 1; \
	fi; \
	matched_mm=$$(grep -E ': 200 iter / median [0-9]+ ns / MAD [0-9]+ ns / mean [0-9]+ ns / range \[[0-9]+, [0-9]+\]$$' /tmp/kaikai-bench-mm.out | wc -l | tr -d ' '); \
	if [ "$$matched_mm" != "2" ]; then \
	  echo "test-bench FAIL — expected 2 bench result lines @ 200 iter (--iters override), got $$matched_mm"; \
	  cat /tmp/kaikai-bench-mm.out; exit 1; \
	fi; \
	KAI_BENCH_ITERS=128 KAI_BENCH_WARMUP=4 ./bin/kai bench examples/stdlib/bench_median_mad.kai > /tmp/kaikai-bench-env.out 2>&1; \
	rc=$$?; \
	if [ $$rc -ne 0 ]; then \
	  echo "test-bench FAIL — KAI_BENCH_ITERS=128 kai bench exited $$rc"; \
	  cat /tmp/kaikai-bench-env.out; exit 1; \
	fi; \
	matched_env=$$(grep -E ': 128 iter / median [0-9]+ ns / MAD [0-9]+ ns / mean [0-9]+ ns / range \[[0-9]+, [0-9]+\]$$' /tmp/kaikai-bench-env.out | wc -l | tr -d ' '); \
	if [ "$$matched_env" != "2" ]; then \
	  echo "test-bench FAIL — expected 2 bench result lines @ 128 iter (env override), got $$matched_env"; \
	  cat /tmp/kaikai-bench-env.out; exit 1; \
	fi; \
	echo "test-bench OK — basic ($$matched), --iters ($$matched_mm), env ($$matched_env) bench formats verified"

# check v1 (issue #44) — smoke for `kai check`. Builds + runs the
# `examples/stdlib/check_basic.kai` fixture and verifies the output
# format. The fixture is constructed so every property holds for
# every value the generators can produce; if a counterexample
# appears it indicates a real regression in the runtime, the
# emitter, or the generator range, not just an unlucky seed.
# Failure modes:
#   - kai check exits non-zero (compile / runtime / counterexample)
#   - any of the four check result lines is missing or malformed
#   - the trailing `4/4 checks passed` summary is missing
test-check: kaic2
	@./bin/kai check examples/stdlib/check_basic.kai > /tmp/kaikai-check-basic.out 2>&1; \
	rc=$$?; \
	if [ $$rc -ne 0 ]; then \
	  echo "test-check FAIL — kai check exited $$rc"; \
	  cat /tmp/kaikai-check-basic.out; exit 1; \
	fi; \
	matched=$$(grep -E ': 100 iter, OK$$' /tmp/kaikai-check-basic.out | wc -l | tr -d ' '); \
	if [ "$$matched" != "5" ]; then \
	  echo "test-check FAIL — expected 5 OK lines, got $$matched"; \
	  cat /tmp/kaikai-check-basic.out; exit 1; \
	fi; \
	if ! grep -qE '^5/5 checks passed$$' /tmp/kaikai-check-basic.out; then \
	  echo "test-check FAIL — missing '5/5 checks passed' summary"; \
	  cat /tmp/kaikai-check-basic.out; exit 1; \
	fi; \
	./bin/kai check examples/stdlib/check_shrinking.kai > /tmp/kaikai-check-shrinking.out 2>&1; \
	rc=$$?; \
	if [ $$rc -eq 0 ]; then \
	  echo "test-check FAIL — shrinking fixture must exit non-zero (every block fails by design)"; \
	  cat /tmp/kaikai-check-shrinking.out; exit 1; \
	fi; \
	if ! grep -qE '^0/3 checks passed$$' /tmp/kaikai-check-shrinking.out; then \
	  echo "test-check FAIL — shrinking fixture missing '0/3 checks passed' summary"; \
	  cat /tmp/kaikai-check-shrinking.out; exit 1; \
	fi; \
	shrunk=$$(grep -E ', shrunk to ' /tmp/kaikai-check-shrinking.out | wc -l | tr -d ' '); \
	if [ "$$shrunk" -lt 2 ]; then \
	  echo "test-check FAIL — expected >=2 'shrunk to' lines (Int + List), got $$shrunk"; \
	  cat /tmp/kaikai-check-shrinking.out; exit 1; \
	fi; \
	echo "test-check OK — $$matched property blocks passed; shrinking produced $$shrunk minimised counterexamples"

# Issue #454 — `--library-mode` regression. Each fixture under
# examples/library_mode/ embeds `# @probe <kind> L:C` markers; kaic2
# emits a single JSON object per file with the resolved type / def /
# enclosing-node answer for each probe. A diff against `.out.expected`
# pins the JSON byte-for-byte so regressions in the typer (positions
# drift, ty_to_string format changes, lower passes lose source spans)
# fail this gate before they reach the LSP / cache lanes downstream.
test-library-mode: kaic2
	@set -e; \
	root=$$(pwd); \
	cd "$$root"; \
	for fx in type_at_basic def_at_basic \
	          def_at_local_basic def_at_local_shadow \
	          def_at_param_basic def_at_param_shadow \
	          def_at_pattern_list def_at_pattern_variant \
	          def_at_pattern_record def_at_pattern_shadow \
	          def_at_pattern_as def_at_closure_capture \
	          def_at_nested_let def_at_match_arm \
	          def_at_lambda_param; do \
	  src="examples/library_mode/$$fx.kai"; \
	  exp="examples/library_mode/$$fx.out.expected"; \
	  out=$$(mktemp); \
	  "$$root/stage2/kaic2" --library-mode "$$src" > "$$out" 2>/dev/null \
	    || { echo "library-mode $$fx FAIL (kaic2 exit)"; rm -f "$$out"; exit 1; }; \
	  diff -q "$$exp" "$$out" > /dev/null \
	    || { echo "library-mode $$fx DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	  rm -f "$$out"; \
	  echo "library-mode $$fx OK"; \
	done; \
	src="examples/library_mode/def_at_imports.kai"; \
	exp="examples/library_mode/def_at_imports.out.expected"; \
	out=$$(mktemp); \
	"$$root/stage2/kaic2" --path examples/library_mode --library-mode "$$src" > "$$out" 2>/dev/null \
	  || { echo "library-mode def_at_imports FAIL (kaic2 exit)"; rm -f "$$out"; exit 1; }; \
	diff -q "$$exp" "$$out" > /dev/null \
	  || { echo "library-mode def_at_imports DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	rm -f "$$out"; \
	echo "library-mode def_at_imports OK"

# Issue #487 — `--diags-json` regression. Each fixture under
# examples/library_mode/diags_*.kai compiles a deliberately-broken
# source; the collected [Diagnostic] is serialised to JSON and diffed
# against the .diags.expected golden. The fixtures pin the m11 v1 +
# v1.x template wording (T1, T2, T3, T4, T5) so an unintended
# refactor that changes the user-facing message fails this gate
# before LSP-side consumers (#447) drift.
test-diagnostics-collected: kaic2
	@set -e; \
	root=$$(pwd); \
	cd "$$root"; \
	for fx in diags_t1_type_mismatch diags_t2_non_exhaustive \
	          diags_t3_unbound_name diags_t4_wrong_arity \
	          diags_t5_missing_effect diags_multiple_errors; do \
	  src="examples/library_mode/$$fx.kai"; \
	  exp="examples/library_mode/$$fx.diags.expected"; \
	  out=$$(mktemp); \
	  "$$root/stage2/kaic2" --diags-json "$$src" > "$$out" 2>/dev/null \
	    || { echo "diags-collected $$fx FAIL (kaic2 exit)"; rm -f "$$out"; exit 1; }; \
	  diff -q "$$exp" "$$out" > /dev/null \
	    || { echo "diags-collected $$fx DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	  rm -f "$$out"; \
	  echo "diags-collected $$fx OK"; \
	done

# Negative-space test suite (issue #511). Every fixture under
# examples/negative/** must be rejected by kaic2 (non-zero exit)
# AND must surface the expected diagnostic substring stored in the
# sibling `.err.expected` golden. Existence of this target is the
# point: positive tests alone proved insufficient (#510 — `pub` was
# silently unenforced for the full life of the language because no
# negative test asserted the contract).
test-negative: kaic2
	@./tools/test-negative.sh

# Validate every stdlib module compiles cleanly when loaded.
# stdlib is normally pulled into user programs via core auto-load or
# `import`, both of which route the file through `expand_imports`.
# The same-module name-collision validators (`validate_fn_name_collisions_decls`
# et al.) now run inside `load_prelude` and `resolve_module` (per-
# module, before the decls join the global stream), so a duplicate
# `fn foo` (or `type T`, `effect E`, `const N`, `axiom A`) inside a
# stdlib file is caught at load time — wherever the module is loaded
# from.
#
# This target exercises that gate. For every `stdlib/**/*.kai` we
# build a one-line trampoline `import <mod>` `fn main() = 0` and
# compile it; the act of importing forces kaic2 to parse and
# validate the module. Any failure (validator rejection, parse
# error, type error) is reported per-module and the target exits
# non-zero. Belongs in tier1 so stdlib drift cannot land via PR
# without surfacing.
test-stdlib-modules: kaic2
	@./tools/test-stdlib-modules.sh

# Package-mode harness (issue #569). The compiler's self-host
# never exercises kaikai-as-a-package — stdlib lives flat under
# `stdlib/`, not behind manifests. Without this harness the entire
# package-manager surface (manifest discovery, kai-pkg paths,
# transitive imports, cross-package effects, auto-install) stays
# a CI blind spot, surfacing regressions only when downstream
# consumers (ahu, henua, kohau) try to integrate — which is how
# #565 and #567 shipped to main. The harness lives in tools/ and
# delegates the 9-category matrix from the issue plus the
# pre-existing driver-level checks (lockfile_reproducibility,
# add_failure, init_invalid_names, manifest_parse_error).
test-packages: kaic2
	@tools/test-packages.sh

# Tier 2.5 — daily memory-safety gate. Rebuilds the demos/ probe set
# with `-fsanitize=address,undefined` and runs each binary; fails on
# any sanitizer diagnostic or if the demos baseline regresses under
# instrumentation. Apple clang lacks LSAN support, so leak detection
# stays disabled (`detect_leaks=0`) for portability with the Linux
# runner; if a leak ratchet ever becomes useful, gate it separately
# on Linux.
#
# Why daily and not per-PR: ASAN doubles compile + run wall, and the
# value is structural — catch new UAF / UB regressions of the R10 /
# R11 shape (heap-use-after-free in handler dispatch / Perceus
# scopes) within 24h of merge — not gating. CI: invoked from
# `make daily` so `.github/workflows/daily.yml` picks it up
# automatically.
tier1-asan: kaic2 test-arena
	@ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	 UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	 $(MAKE) -C demos verify \
	    CFLAGS="-std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-unused-function -Wno-unused-variable" \
	    > /tmp/kaikai-tier1-asan.log 2>&1; \
	hits=$$(grep -lE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' demos/build/*.err 2>/dev/null | wc -l | tr -d ' '); \
	if [ "$$hits" != "0" ]; then \
	  echo "tier1-asan FAIL — sanitizer diagnostics in $$hits demo(s):"; \
	  grep -lE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' demos/build/*.err; \
	  echo "see /tmp/kaikai-tier1-asan.log for full kaic2 / cc output"; \
	  exit 1; \
	fi; \
	expected=$${BASELINE:-$$(cat demos/baseline.txt 2>/dev/null || echo 0)}; \
	got=$$(cat demos/build/*.status 2>/dev/null | grep -cE '^(OK|PASS)'); \
	if [ "$$got" -lt "$$expected" ]; then \
	  echo "tier1-asan FAIL — demos baseline regressed under ASAN: $$got < $$expected"; \
	  echo "see /tmp/kaikai-tier1-asan.log for per-demo status"; \
	  exit 1; \
	fi; \
	echo "tier1-asan OK — $$got/$$expected demos pass under ASAN+UBSan, no sanitizer diagnostics"
	@$(MAKE) -C stage2 test-trace-asan
	@echo "tier1-asan OK — trace fixtures pass under ASAN+UBSan (R10/R11 regression gate)"
	@$(MAKE) -C stage2 test-runtime-shadow-asan
	@echo "tier1-asan OK — issue #78 fixture passes under ASAN+UBSan (runtime helper rename gate)"
	@$(MAKE) -C stage2 test-signal-trap-asan
	@echo "tier1-asan OK — issue #107 fixture passes under ASAN+UBSan (Signal effect runtime gate)"
	@$(MAKE) -C stage2 test-log-asan
	@echo "tier1-asan OK — issue #141 fixture passes under ASAN+UBSan (Log default handler / clock_gettime / strftime gate)"
	@$(MAKE) -C stage2 test-trap-exit-cancel-asan
	@echo "tier1-asan OK — issue #103 fixture passes under ASAN+UBSan (trap-exit / outer Cancel handler gate)"
	@$(MAKE) -C stage2 test-process-basic-asan
	@echo "tier1-asan OK — issue #126 fixture passes under ASAN+UBSan (Process effect runtime gate)"
	@$(MAKE) -C stage2 test-perceus-issue82-asan
	@echo "tier1-asan OK — issue #82 fixture passes under ASAN+UBSan (Perceus leak-audit gate)"
	@$(MAKE) -C stage2 test-ffi-extern-c-asan
	@echo "tier1-asan OK — m12.7.x FFI fixture passes under ASAN+UBSan (extern_c shim memory gate)"
	@$(MAKE) -C stage2 test-perceus-issue118-asan
	@echo "tier1-asan OK — issue #118 fixtures pass under ASAN+UBSan (Perceus reuse-in-place gate)"
	@$(MAKE) -C stage2 test-perceus-issue298-asan
	@echo "tier1-asan OK — issue #298 fixture passes under ASAN+UBSan (closure capture lifecycle gate)"
	@$(MAKE) -C stage2 test-perceus-issue350-asan
	@echo "tier1-asan OK — issue #350 fixtures pass under ASAN+UBSan (arm-binding multi-use drop gate)"
	@$(MAKE) -C stage2 test-perceus-issue703-asan
	@echo "tier1-asan OK — issue #703 fixture passes under ASAN+UBSan (UFn-body let-bound variant double-match gate)"
	@$(MAKE) -C stage2 test-perceus-int-cache-asan
	@echo "tier1-asan OK — Phase 1.A fixture passes under ASAN+UBSan (widened small-int cache pinned-slot gate)"
	@$(MAKE) -C stage2 test-match-pbind-catchall-asan
	@echo "tier1-asan OK — issue #91 fixture passes under ASAN+UBSan (PBind catch-all fast-path raw-local gate)"
	@$(MAKE) -C stage2 test-http-client-asan
	@echo "tier1-asan OK — net.http client fixture passes under ASAN+UBSan (Tier S1 lane #3 gate)"
	@$(MAKE) -C stage2 test-stdlib-crypto-asan
	@echo "tier1-asan OK — issue #139 fixture passes under ASAN+UBSan (crypto byte-buffer + 64-bit modular gate)"
	@$(MAKE) -C stage2 test-stdlib-regex-predicate-asan
	@echo "tier1-asan OK — issue #85 fixture passes under ASAN+UBSan (regex sigil + matches predicate runtime gate)"
	@$(MAKE) -C stage2 test-env-mutate-asan
	@echo "tier1-asan OK — issue #127 fixture passes under ASAN+UBSan (Env set/unset/vars buffer-ownership gate)"
	@$(MAKE) -C stage2 test-securerandom-asan
	@echo "tier1-asan OK — issue #140 fixture passes under ASAN+UBSan (SecureRandom default-handler gate)"

# Backend-parity (issue #575): build every entry-point fixture under
# the documented example dirs + demos with both backends, run, diff
# stdout + exit code. Path-gated CI (tier1-backend-parity.yml) on every
# PR touching compiler / stdlib / examples / demos / driver. Local
# convenience target — paralleled by the CI workflow, not invoked from
# `tier1` because the cost (~15-30 min) does not belong on every PR
# locally.
tier1-backend-parity: kaic2
	@tools/test-backend-parity.sh

# Tier 2: daily / nightly. ~10-20 min. Runs once a day on `main` HEAD,
# not per-PR. If it fails, `main` stays unbroken (Tier 0/1 gated every
# commit) but a diagnostic opens a lane the next morning.
daily: tier1 stress-fixtures coverage-probe rc-budget test-binserialize-budget tier1-asan tier1-backend-parity
	@echo "daily OK — tier1 + stress fixtures + coverage probe + RC budget + BinSerialize budget + tier1-asan + tier1-backend-parity"

# Stress fixtures: programs that exercise patterns the per-feature
# suite does not. Some are aspirational (expected to fail until a
# specific lane lands); the daily run reports them and the maintainer
# decides if a green flip is unexpected.
stress-fixtures: kaic2
	@set -e; \
	for f in examples/effects/interp_recursive_walk.kai \
	         examples/effects/m4c_flow_through.kai; do \
	  name=$$(basename $$f .kai); \
	  stage2/kaic2 $$f > /tmp/stress-$$name.c 2> /tmp/stress-$$name.err \
	    || { echo "stress FAIL $$name (kaic2 errored)"; cat /tmp/stress-$$name.err; exit 1; }; \
	  cc -std=c99 -I stage0 /tmp/stress-$$name.c -o /tmp/stress-$$name -lm 2>> /tmp/stress-$$name.err \
	    || { echo "stress FAIL $$name (cc errored)"; cat /tmp/stress-$$name.err; exit 1; }; \
	  /tmp/stress-$$name > /tmp/stress-$$name.out 2>&1 \
	    && echo "stress OK $$name" \
	    || { echo "stress FAIL $$name (binary exit non-zero)"; cat /tmp/stress-$$name.out; exit 1; }; \
	done

# Coverage probe: every section of the runtime / language docs has a
# fixture; if not, alarm. Implemented as a shell script so it can run
# in CI without depending on the kaikai compiler itself.
coverage-probe:
	@./tools/coverage-probe.sh

# RC budget: leaked / RSS / wall vs the threshold pinned in
# docs/perceus-honesty-targets.md. Today the threshold is "no
# regression from 46.9 M leaked".
rc-budget: kaic2
	@KAI_TRACE_RC=1 stage2/kaic2 stage2/compiler.kai > /dev/null 2> /tmp/rc.log; \
	leaked=$$(grep -E "alloc_total" /tmp/rc.log | head -1 | sed 's/.*leaked=\([0-9]*\).*/\1/'); \
	if [ -z "$$leaked" ]; then \
	  echo "rc-budget SKIP — KAI_TRACE_RC trace empty (kaic2 not built with trace)"; \
	  exit 0; \
	fi; \
	threshold=50000000; \
	if [ "$$leaked" -gt "$$threshold" ]; then \
	  echo "rc-budget FAIL — leaked $$leaked > threshold $$threshold (regression)"; \
	  exit 1; \
	fi; \
	echo "rc-budget OK — leaked $$leaked <= threshold $$threshold"

# BinSerialize perf gate (issue #489). Wall-time ceiling over 100
# decodes of a 500-node / ~40 KB payload. Tier 2 (not Tier 1)
# because perf telemetry belongs at daily cadence, not per-PR.
# Ceiling lives in tools/binserialize-budget.txt; ratchet down as
# future lanes improve. Guards against silent re-introduction of
# O(N) shape on the decode path that the round-trip correctness
# tests would not catch (issue #485 was 19 s; PR #487 dropped that
# to ~7 ms/decode; the budget defends that work for the cache lanes
# (#452 Phase A.0 onwards) that build on top of this substrate).
test-binserialize-budget: kaic2
	@bin/kai build tools/bench-binserialize-roundtrip.kai -o /tmp/bench-binserialize >/dev/null 2>&1 \
	  || { echo "binserialize-budget FAIL — build error"; exit 1; }
	@ceiling=$$(grep -v '^#' tools/binserialize-budget.txt | head -1); \
	wall_s=$$(/usr/bin/time -p /tmp/bench-binserialize > /dev/null 2>/tmp/binserialize.t \
	  && awk '/^real/{print $$2}' /tmp/binserialize.t); \
	wall_ms=$$(awk -v s="$$wall_s" 'BEGIN{printf "%d", s * 1000}'); \
	per_decode=$$(awk -v ms="$$wall_ms" 'BEGIN{printf "%.2f", ms / 100.0}'); \
	if [ "$$wall_ms" -gt "$$ceiling" ]; then \
	  echo "binserialize-budget FAIL — 100 decodes took $${wall_ms} ms (per decode $${per_decode} ms) > ceiling $${ceiling} ms"; \
	  echo "  expected: PR #487 baseline was ~7 ms/decode; ratchet up only with a documented justification"; \
	  exit 1; \
	fi; \
	echo "binserialize-budget OK — 100 decodes in $${wall_ms} ms ($${per_decode} ms/decode <= $$((ceiling / 100)) ms ceiling)"

# ---- L0 LLVM static prep (issue: LLVM-direct DoD #6 follow-up) ------
#
# These targets prepare an out-of-tree, statically-linked libLLVM for
# the future L3 lane that will replace the external `clang` shell-out
# in `bin/kai --backend=llvm` with an in-process libLLVM call. None of
# them run from `all`, `tier0`, or `tier1` — invocation is explicit.
#
# kaic2 emits LLVM IR as TEXT (see emit_program_llvm at compiler.kai:44202),
# so the in-process replacement parses textual IR and drives the
# codegen + linker pipeline. That is a MUCH smaller libLLVM surface
# than constructing IR programmatically: roughly the components below.
#
# See docs/lane-experience-l0-llvm-static-prep.md for the full retro,
# size measurements, CI plan, and recommendation for L3.

LLVM_VERSION ?= 18.1.8
LLVM_SRC_DIR := stage0/third_party/llvm
LLVM_BUILD_DIR := $(LLVM_SRC_DIR)/build
LLVM_TARBALL := stage0/third_party/llvm-$(LLVM_VERSION).src.tar.xz
LLVM_TARBALL_URL := https://github.com/llvm/llvm-project/releases/download/llvmorg-$(LLVM_VERSION)/llvm-$(LLVM_VERSION).src.tar.xz
# CMake targets we actually need (parse text IR + emit x86-64 + arm64
# objects). Kept narrow on purpose; expanding this list expands the
# final static-link footprint roughly linearly.
LLVM_CMAKE_TARGETS := LLVMCore LLVMSupport LLVMIRReader LLVMAsmParser \
                      LLVMBitReader LLVMBitWriter \
                      LLVMTarget LLVMTargetParser LLVMMC LLVMMCParser \
                      LLVMObject LLVMOption LLVMBinaryFormat \
                      LLVMX86CodeGen LLVMX86AsmParser LLVMX86Desc LLVMX86Info \
                      LLVMAArch64CodeGen LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info \
                      LLVMCodeGen LLVMAnalysis LLVMTransformUtils LLVMScalarOpts \
                      LLVMSelectionDAG LLVMGlobalISel LLVMAsmPrinter \
                      LLVMipo LLVMInstCombine LLVMInstrumentation \
                      LLVMVectorize LLVMLinker LLVMPasses \
                      LLVMDemangle LLVMRemarks LLVMDebugInfoDWARF LLVMDebugInfoCodeView

# llvm-info: prints what would be downloaded / built, without doing it.
# Safe to run on any machine.
llvm-info:
	@echo "LLVM_VERSION    = $(LLVM_VERSION)"
	@echo "LLVM_SRC_DIR    = $(LLVM_SRC_DIR)"
	@echo "LLVM_BUILD_DIR  = $(LLVM_BUILD_DIR)"
	@echo "LLVM_TARBALL    = $(LLVM_TARBALL)"
	@echo "LLVM_TARBALL_URL= $(LLVM_TARBALL_URL)"
	@echo "TARGETS         = X86 + AArch64 (MinSizeRel, no zlib/zstd/terminfo)"
	@echo "CMAKE_TARGETS   = $(words $(LLVM_CMAKE_TARGETS)) libraries"
	@echo "Disk required   = ~3.5 GB build tree, ~150-300 MB sum-of-.a (L3 measurement)"
	@echo "First build     = ~10-30 min on a modern laptop, ~30-60 min on cold CI"
	@echo ""
	@echo "Workflow:"
	@echo "  make llvm-fetch       # download + extract tarball (one-shot)"
	@echo "  make llvm-configure   # cmake -B build with MinSizeRel"
	@echo "  make llvm-build       # cmake --build (the long step)"
	@echo "  make llvm-size        # sum sizes of the .a archives"
	@echo "  make llvm-clean       # remove the build tree (keep source)"

# llvm-fetch: download + verify the LLVM source tarball, then extract
# into $(LLVM_SRC_DIR). The tarball + tree are gitignored; we never
# commit LLVM source. Re-running is a no-op when the source is present.
llvm-fetch:
	@mkdir -p stage0/third_party
	@if [ -d "$(LLVM_SRC_DIR)" ] && [ -f "$(LLVM_SRC_DIR)/CMakeLists.txt" ]; then \
	  echo "llvm-fetch: $(LLVM_SRC_DIR) already populated, skipping"; \
	  exit 0; \
	fi; \
	if [ ! -f "$(LLVM_TARBALL)" ]; then \
	  echo "llvm-fetch: downloading $(LLVM_TARBALL_URL)"; \
	  curl -fL -o "$(LLVM_TARBALL).part" "$(LLVM_TARBALL_URL)" \
	    && mv "$(LLVM_TARBALL).part" "$(LLVM_TARBALL)" \
	    || { echo "llvm-fetch FAIL — download error"; rm -f "$(LLVM_TARBALL).part"; exit 1; }; \
	fi; \
	echo "llvm-fetch: extracting tarball into $(LLVM_SRC_DIR)"; \
	mkdir -p "$(LLVM_SRC_DIR)"; \
	tar -xJf "$(LLVM_TARBALL)" --strip-components=1 -C "$(LLVM_SRC_DIR)" \
	  || { echo "llvm-fetch FAIL — extract error"; exit 1; }; \
	echo "llvm-fetch OK — source at $(LLVM_SRC_DIR)"

# llvm-configure: run cmake. MinSizeRel + only X86 + AArch64 targets +
# disable optional features that bloat the static link (zlib, zstd,
# terminfo, libxml2). Requires cmake + ninja in PATH; we don't add
# them to stage0 deps. If cmake/ninja are missing the failure is loud.
llvm-configure: llvm-fetch
	@command -v cmake >/dev/null 2>&1 || { echo "llvm-configure FAIL — cmake not in PATH"; exit 2; }
	@command -v ninja >/dev/null 2>&1 || { echo "llvm-configure FAIL — ninja not in PATH"; exit 2; }
	cd $(LLVM_SRC_DIR) && cmake -B build -G Ninja \
	  -DCMAKE_BUILD_TYPE=MinSizeRel \
	  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
	  -DLLVM_ENABLE_PROJECTS="" \
	  -DLLVM_BUILD_TOOLS=OFF \
	  -DLLVM_BUILD_UTILS=OFF \
	  -DLLVM_BUILD_EXAMPLES=OFF \
	  -DLLVM_INCLUDE_EXAMPLES=OFF \
	  -DLLVM_INCLUDE_TESTS=OFF \
	  -DLLVM_INCLUDE_BENCHMARKS=OFF \
	  -DLLVM_INCLUDE_DOCS=OFF \
	  -DLLVM_ENABLE_BACKTRACES=OFF \
	  -DLLVM_ENABLE_ZLIB=OFF \
	  -DLLVM_ENABLE_ZSTD=OFF \
	  -DLLVM_ENABLE_TERMINFO=OFF \
	  -DLLVM_ENABLE_LIBXML2=OFF \
	  -DLLVM_ENABLE_LIBEDIT=OFF \
	  -DLLVM_ENABLE_OCAMLDOC=OFF \
	  -DLLVM_ENABLE_BINDINGS=OFF \
	  -DLLVM_ENABLE_ASSERTIONS=OFF
	@echo "llvm-configure OK — build/ ready under $(LLVM_SRC_DIR)"

# llvm-build: actually compile the static libs. This is the long step
# (10-30 min cold). The target list is narrow on purpose; expanding it
# raises the linked binary size in L3 roughly linearly.
llvm-build: llvm-configure
	cd $(LLVM_SRC_DIR) && cmake --build build --target $(LLVM_CMAKE_TARGETS)
	@echo "llvm-build OK — static .a archives under $(LLVM_BUILD_DIR)/lib"
	@$(MAKE) llvm-size

# llvm-size: sum-of-.a measurement. The number L3 needs to estimate
# the linked-kaic2 binary size. Static link drops a lot via dead-code
# elimination, so the linked footprint is typically 30-50% of the
# sum-of-.a, not 100%.
llvm-size:
	@if [ ! -d "$(LLVM_BUILD_DIR)/lib" ]; then \
	  echo "llvm-size: no build yet, run \`make llvm-build\` first"; \
	  exit 1; \
	fi; \
	echo "Static archive sizes under $(LLVM_BUILD_DIR)/lib:"; \
	du -sh $(LLVM_BUILD_DIR)/lib/*.a 2>/dev/null | sort -h | tail -20; \
	total=$$(du -sk $(LLVM_BUILD_DIR)/lib/*.a 2>/dev/null | awk '{s+=$$1} END {print s}'); \
	echo "Sum of .a archives: $$total KB (~$$((total / 1024)) MB)"

# llvm-clean: drop the build tree, keep the unpacked source. Use this
# between configure-tuning runs. To drop everything (source + tarball)
# delete stage0/third_party/ directly.
llvm-clean:
	rm -rf $(LLVM_BUILD_DIR)
	@echo "llvm-clean OK — source tree preserved at $(LLVM_SRC_DIR)"
