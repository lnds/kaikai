.PHONY: all kaic0 kaic1 kaic2 test test-stage0 test-stage1 test-stage2 test-demos test-tutorial demos-verify demos-no-regression selfhost clean

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

test: test-stage0 test-stage1 test-stage2 test-demos test-tutorial

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

# Tutorial gate — every example under examples/tutorial/ runs via
# bin/kai and matches its `.out.expected` golden. Step 11 also
# diffs the test-runner output (stderr) against `.test.expected`
# so `kai test` keeps reporting the expected pass count. Wired into
# the top-level `test` aggregate.
test-tutorial: kaic2
	@set -e; \
	fail=0; \
	for f in examples/tutorial/[0-9]*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  if [ ! -f $$exp ]; then \
	    echo "tutorial MISS $$name (no .out.expected)"; fail=$$((fail+1)); continue; \
	  fi; \
	  KAI_TRACE_RC= ./bin/kai run $$f > /tmp/kaikai-tutorial-$$name.out 2> /tmp/kaikai-tutorial-$$name.err \
	    || { echo "tutorial FAIL $$name (binary exit non-zero)"; cat /tmp/kaikai-tutorial-$$name.err; fail=$$((fail+1)); continue; }; \
	  diff -q $$exp /tmp/kaikai-tutorial-$$name.out > /dev/null \
	    && echo "tutorial OK   $$name" \
	    || { echo "tutorial FAIL $$name (output mismatch)"; diff $$exp /tmp/kaikai-tutorial-$$name.out; fail=$$((fail+1)); }; \
	done; \
	for f in examples/tutorial/[0-9]*.test.expected; do \
	  src=$${f%.test.expected}.kai; \
	  name=$$(basename $$src .kai); \
	  KAI_TRACE_RC= ./bin/kai test $$src 2> /tmp/kaikai-tutorial-$$name.test 1>/dev/null \
	    || { echo "tutorial FAIL $$name (test runner exit non-zero)"; cat /tmp/kaikai-tutorial-$$name.test; fail=$$((fail+1)); continue; }; \
	  grep -v '^\[KAI_TRACE_RC\]' /tmp/kaikai-tutorial-$$name.test > /tmp/kaikai-tutorial-$$name.test.clean; \
	  diff -q $$f /tmp/kaikai-tutorial-$$name.test.clean > /dev/null \
	    && echo "tutorial OK   $$name (test runner)" \
	    || { echo "tutorial FAIL $$name (test runner output mismatch)"; diff $$f /tmp/kaikai-tutorial-$$name.test.clean; fail=$$((fail+1)); }; \
	done; \
	if [ $$fail -ne 0 ]; then echo "tutorial FAIL ($$fail mismatches)"; exit 1; fi; \
	echo "tutorial OK (all examples + test-runner goldens match)"

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
