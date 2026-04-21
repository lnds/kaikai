CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0
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
	$(CC) $(CFLAGS) -I ../stage0 $< -o $@

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
	for f in ../examples/minimal/*.kai compiler.kai; do \
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
	for f in ../examples/minimal/hello.kai ../examples/minimal/fizzbuzz.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name; \
	  ./build/$$name > build/$$name.out; \
	  $(STAGE0) $$f > build/$$name-s0.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-s0.c -o build/$$name-s0; \
	  ./build/$$name-s0 > build/$$name-s0.out; \
	  diff -q build/$$name.out build/$$name-s0.out > /dev/null \
	    && echo "run OK $$name" \
	    || { echo "run DIFF $$name"; diff build/$$name-s0.out build/$$name.out; exit 1; }; \
	done

test: test-tokens test-ast test-check test-run

clean:
	rm -rf build $(TARGET)
