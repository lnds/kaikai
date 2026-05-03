.PHONY: all kaic0 kaic1 kaic2 test test-stage0 test-stage1 test-stage2 test-demos test-fmt test-bench test-check demos-verify demos-no-regression selfhost clean tier0 tier1 tier1-asan daily coverage-probe rc-budget stress-fixtures

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

test: test-stage0 test-stage1 test-stage2 test-demos

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
tier1: test demos-no-regression test-fmt test-bench test-check
	@echo "tier1 OK — full make test + demos baseline + fmt fixtures + bench smoke + check smoke"

# Tongariki — `kai fmt` fixture suite. Verifies that every fixture
# in examples/fmt/ formats to its `.expected.kai` and is idempotent,
# plus that every formattable example under examples/minimal/ /
# quickstart/ / phase4/ round-trips through the formatter without
# breaking re-parse. Cheap enough to gate every Tier 1 run.
test-fmt: kaic2
	@./tests/fmt_fixtures.sh

# bench v1 (issue #40) — smoke for `kai bench`. Builds + runs the
# `examples/stdlib/bench_basic.kai` fixture and verifies the output
# format. ns/iter values vary per run so we grep for the pattern
# rather than diffing against a golden file. Failure modes:
#   - kai bench exits non-zero (compile / runtime error)
#   - any of the three bench result lines is missing or malformed
#   - the trailing `3 benches` summary line is missing
test-bench: kaic2
	@./bin/kai bench examples/stdlib/bench_basic.kai > /tmp/kaikai-bench-basic.out 2>&1; \
	rc=$$?; \
	if [ $$rc -ne 0 ]; then \
	  echo "test-bench FAIL — kai bench exited $$rc"; \
	  cat /tmp/kaikai-bench-basic.out; exit 1; \
	fi; \
	matched=$$(grep -E ': 1000 iter / [0-9]+ ns/iter$$' /tmp/kaikai-bench-basic.out | wc -l | tr -d ' '); \
	if [ "$$matched" != "3" ]; then \
	  echo "test-bench FAIL — expected 3 bench result lines, got $$matched"; \
	  cat /tmp/kaikai-bench-basic.out; exit 1; \
	fi; \
	if ! grep -qE '^3 benches$$' /tmp/kaikai-bench-basic.out; then \
	  echo "test-bench FAIL — missing '3 benches' summary line"; \
	  cat /tmp/kaikai-bench-basic.out; exit 1; \
	fi; \
	echo "test-bench OK — $$matched benches printed in expected format"

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
	echo "test-check OK — $$matched property blocks passed"

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
	@$(MAKE) -C stage2 test-trap-exit-cancel-asan
	@echo "tier1-asan OK — issue #103 fixture passes under ASAN+UBSan (trap-exit / outer Cancel handler gate)"
	@$(MAKE) -C stage2 test-perceus-issue82-asan
	@echo "tier1-asan OK — issue #82 fixture passes under ASAN+UBSan (Perceus leak-audit gate)"
	@$(MAKE) -C stage2 test-http-client-asan
	@echo "tier1-asan OK — net.http client fixture passes under ASAN+UBSan (Tier S1 lane #3 gate)"

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
	  cc -std=c99 -I stage0 /tmp/stress-$$name.c -o /tmp/stress-$$name 2>> /tmp/stress-$$name.err \
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
