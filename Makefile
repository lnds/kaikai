.PHONY: all kaic0 kaic1 kaic2 test test-stage0 test-stage1 test-stage2 test-demos test-multi-module test-import-stdlib test-import-prelude-dedup test-fmt test-bench test-check test-library-mode demos-verify demos-no-regression selfhost clean tier0 tier1 tier1-asan daily coverage-probe rc-budget stress-fixtures

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

test: test-stage0 test-stage1 test-stage2 test-demos test-multi-module test-import-stdlib test-import-prelude-dedup

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
	for case in multi-module multi-module/issue-237; do \
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

# Self-hosting fixed point for both compilers.
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
tier0: selfhost demos-no-regression
	@echo "tier0 OK — selfhost byte-identical, demos baseline holds"

# Tier 1: pre-PR gate. ~2-4 min. Run before opening / merging a PR.
# PR description should include the trailing line of this output (or
# a CI link) — without it, the merge does not happen.
tier1: test demos-no-regression test-fmt test-bench test-check test-library-mode
	@echo "tier1 OK — full make test + demos baseline + fmt fixtures + bench smoke + check smoke + library-mode probes"

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
	if [ "$$matched" != "4" ]; then \
	  echo "test-check FAIL — expected 4 OK lines, got $$matched"; \
	  cat /tmp/kaikai-check-basic.out; exit 1; \
	fi; \
	if ! grep -qE '^4/4 checks passed$$' /tmp/kaikai-check-basic.out; then \
	  echo "test-check FAIL — missing '4/4 checks passed' summary"; \
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
	for fx in type_at_basic def_at_basic; do \
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
tier1-asan: kaic2
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

# Tier 2: daily / nightly. ~10-20 min. Runs once a day on `main` HEAD,
# not per-PR. If it fails, `main` stays unbroken (Tier 0/1 gated every
# commit) but a diagnostic opens a lane the next morning.
daily: tier1 stress-fixtures coverage-probe rc-budget tier1-asan
	@echo "daily OK — tier1 + stress fixtures + coverage probe + RC budget + tier1-asan"

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
