CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0
# issue #343 — math/real libm bindings live in stage0/runtime.h.
# Linux glibc requires explicit `-lm` after the .c inputs; macOS
# libSystem already exposes those symbols. Trailed at the end of
# every $(CC) compile-and-link line that consumes runtime.h.
LDLIBS_M := -lm
# Hanga Roa: compile-time absolute path to `stdlib/` so the core
# loader can find core/*.kai, protocols.kai, effects.kai, array.kai
# without any --prelude flag at runtime. The path is resolved from
# this Makefile's location (../stdlib relative to stage1/).
STDLIB_ABS := $(abspath ../stdlib)
CPPFLAGS_CORE := -DKAI_STDLIB_PATH=\"$(STDLIB_ABS)\"
STAGE0  := ../stage0/kaic0
TARGET  := kaic1
SRC     := compiler.kai

.PHONY: all clean test

all: $(TARGET)

$(STAGE0):
	$(MAKE) -C ../stage0 kaic0

build:
	@mkdir -p build

build/stage1.c: $(SRC) $(STAGE0) | build
	$(STAGE0) $(SRC) > $@

$(TARGET): build/stage1.c
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 $< -o $@ $(LDLIBS_M)

# Compare stage 1's --tokens output against stage 0's (normalized to
# ignore whitespace differences, since stage 0 uses fixed-width columns).
# Milestones 6+ extend this to parser/emitter checks against stage 0.
test-tokens: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai compiler.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --tokens $$f | awk '{$$1=$$1; print}' > build/s1-$$name.tok; \
	  $(STAGE0) --tokens $$f   | awk '{$$1=$$1; print}' > build/s0-$$name.tok; \
	  diff -q build/s0-$$name.tok build/s1-$$name.tok > /dev/null \
	    && echo "tokens OK $$name" \
	    || { echo "tokens DIFF $$name"; diff build/s0-$$name.tok build/s1-$$name.tok | head; exit 1; }; \
	done

# Verify parser: --ast must produce some output without crashing on every
# minimal example and on stage 1's own source. Byte-for-byte AST
# equivalence against stage 0 is not attempted — stage 0 uses a different
# dump format. Real equivalence is checked downstream when the emitter
# compares C output.
test-ast: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai compiler.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --ast $$f > build/s1-$$name.ast && echo "ast OK $$name" \
	    || { echo "ast FAIL $$name"; exit 1; }; \
	done

# Run the full pipeline (lex + parse + check) without emission on every
# example. Exit status is the check result; no undefined names allowed.
test-check: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai ../examples/attributes/attr_doc_multiline.kai compiler.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > /dev/null \
	    && echo "check OK $$name" \
	    || { echo "check FAIL $$name"; exit 1; }; \
	done

# End-to-end: compile a .kai file via kaic1, feed the C to cc, run the
# binary, and compare its stdout against stage 0's equivalent. For now
# only hello.kai; milestones 7-8 extend this.
test-run: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	  ./build/$$name > build/$$name.out; \
	  $(STAGE0) $$f > build/$$name-s0.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-s0.c -o build/$$name-s0 $(LDLIBS_M); \
	  ./build/$$name-s0 > build/$$name-s0.out; \
	  diff -q build/$$name.out build/$$name-s0.out > /dev/null \
	    && echo "run OK $$name" \
	    || { echo "run DIFF $$name"; diff build/$$name-s0.out build/$$name.out; exit 1; }; \
	done

# Compile each example with --test and run its tests. Compare stderr
# (which is where kai_test_* writes) against stage 0's equivalent.
test-blocks: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --test $$f > build/$$name-t.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-t.c -o build/$$name-t $(LDLIBS_M); \
	  ./build/$$name-t 2> build/$$name-t.err || true; \
	  $(STAGE0) --test $$f > build/$$name-t-s0.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-t-s0.c -o build/$$name-t-s0 $(LDLIBS_M); \
	  ./build/$$name-t-s0 2> build/$$name-t-s0.err || true; \
	  diff -q build/$$name-t.err build/$$name-t-s0.err > /dev/null \
	    && echo "tests OK $$name" \
	    || { echo "tests DIFF $$name"; diff build/$$name-t-s0.err build/$$name-t.err; exit 1; }; \
	done

# Self-hosting: stage 1 per-compiler determinism.
#
# Build kaic1b (stage 1 compiled by stage 1), have it compile its own
# source again to produce kaic1c.c, and require kaic1b.c == kaic1c.c.
# This catches non-determinism inside stage 1 (hashmap iteration,
# pointer addresses, timestamps) — the same canary `make -C stage2
# selfhost` provides for stage 2. See
# docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md.
#
# The trailing per-example diff (kaic1 vs kaic1b emitting the same
# minimal-language samples) is an additional check that stage 1's
# emitter is consistent across its own bootstrap. This compares
# kaic1 against kaic1b — same compiler, two builds — not stage 1
# against stage 2.
selfhost: $(TARGET) build
	@set -e; \
	./$(TARGET) compiler.kai > build/kaic1b.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/kaic1b.c -o build/kaic1b $(LDLIBS_M); \
	./build/kaic1b compiler.kai > build/kaic1c.c; \
	diff -q build/kaic1b.c build/kaic1c.c > /dev/null \
	  && echo "stage1 selfhost determinism: OK (kaic1b.c == kaic1c.c)" \
	  || { echo "stage1 selfhost determinism: DIFF (kaic1b.c != kaic1c.c)"; exit 1; }; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./build/kaic1b $$f    > build/$$name-b.c; \
	  ./$(TARGET)    $$f    > build/$$name-a.c; \
	  diff -q build/$$name-a.c build/$$name-b.c > /dev/null \
	    && echo "emit OK $$name" \
	    || { echo "emit DIFF $$name (kaic1 vs kaic1b)"; diff build/$$name-a.c build/$$name-b.c | head; exit 1; }; \
	done

test: test-tokens test-ast test-check test-run test-blocks

clean:
	rm -rf build $(TARGET)
