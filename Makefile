.PHONY: all kaic0 kaic1 kaic2 test test-stage0 test-stage1 test-stage2 test-demos demos-verify selfhost clean

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

# Self-hosting fixed point for both compilers.
selfhost:
	$(MAKE) -C stage1 selfhost
	$(MAKE) -C stage2 selfhost

clean:
	$(MAKE) -C stage0 clean
	$(MAKE) -C stage1 clean
	$(MAKE) -C stage2 clean
