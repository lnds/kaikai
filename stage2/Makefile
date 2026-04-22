CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0
KAIC1   := ../stage1/kaic1
TARGET  := kaic2
SRC     := compiler.kai

.PHONY: all clean test test-tokens test-ast test-check test-run test-blocks selfhost

all: $(TARGET)

$(KAIC1):
	$(MAKE) -C ../stage1 kaic1

build:
	@mkdir -p build

build/stage2.c: $(SRC) $(KAIC1) | build
	$(KAIC1) $(SRC) > $@

$(TARGET): build/stage2.c
	$(CC) $(CFLAGS) -I ../stage0 $< -o $@

# Token stream must match stage 1 (which already matches stage 0).
test-tokens: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai ../stage1/compiler.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --tokens $$f | awk '{$$1=$$1; print}' > build/s2-$$name.tok; \
	  ../stage1/kaic1 --tokens $$f | awk '{$$1=$$1; print}' > build/s1-$$name.tok; \
	  diff -q build/s1-$$name.tok build/s2-$$name.tok > /dev/null \
	    && echo "tokens OK $$name" \
	    || { echo "tokens DIFF $$name"; diff build/s1-$$name.tok build/s2-$$name.tok | head; exit 1; }; \
	done

# --ast smoke test (format is indented, readable, not byte-compared).
test-ast: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --ast $$f > build/s2-$$name.ast && echo "ast OK $$name" \
	    || { echo "ast FAIL $$name"; exit 1; }; \
	done

# --types smoke test: every top-level decl gets a signature dumped.
test-types: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --types $$f > build/s2-$$name.types && echo "types OK $$name" \
	    || { echo "types FAIL $$name"; exit 1; }; \
	done

# --env smoke test: full type environment (prelude + variants + user decls).
test-env: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --env $$f > build/s2-$$name.env && echo "env OK $$name" \
	    || { echo "env FAIL $$name"; exit 1; }; \
	done

# --infer smoke test: HM inference over every top-level fn body. Each
# example is expected to pass without unification errors; stderr lines
# other than routine diagnostics would indicate a regression.
test-infer: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --infer $$f > build/s2-$$name.infer 2> build/s2-$$name.infer.err \
	    && [ ! -s build/s2-$$name.infer.err ] \
	    && echo "infer OK $$name" \
	    || { echo "infer FAIL $$name"; cat build/s2-$$name.infer.err; exit 1; }; \
	done

# --check: name resolution + HM inference, with no --infer noise on stdout.
# A clean run must leave both stdout and stderr empty.
test-infer-check: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --check $$f > build/s2-$$name.check 2> build/s2-$$name.check.err \
	    && [ ! -s build/s2-$$name.check ] \
	    && [ ! -s build/s2-$$name.check.err ] \
	    && echo "check-types OK $$name" \
	    || { echo "check-types FAIL $$name"; cat build/s2-$$name.check.err; exit 1; }; \
	done

# Checker: undefined-name resolution runs clean on every example.
test-check: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > /dev/null \
	    && echo "check OK $$name" \
	    || { echo "check FAIL $$name"; exit 1; }; \
	done

# LLVM backend round-trip: emitted .ll is compiled with clang against
# the runtime shim, and the resulting binary's output must match the
# C backend's. `minimal/hello.kai` exercises the M3a skeleton; files
# under `examples/llvm/` cover the incremental milestones with just
# the constructs each one targets.
LLVM_MIN_EXAMPLES := hello fizzbuzz interp
LLVM_EXAMPLES     := m3b
test-llvm: $(TARGET) build
	@set -e; \
	for name in $(LLVM_MIN_EXAMPLES); do \
	  f=../examples/minimal/$$name.kai; \
	  ./$(TARGET) --emit=llvm $$f > build/$$name.ll; \
	  clang -w -I ../stage0 build/$$name.ll ../stage0/runtime_llvm.c -o build/$$name-llvm; \
	  ./build/$$name-llvm > build/$$name-llvm.out; \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name-c; \
	  ./build/$$name-c > build/$$name-c.out; \
	  diff -q build/$$name-llvm.out build/$$name-c.out > /dev/null \
	    && echo "llvm OK $$name" \
	    || { echo "llvm DIFF $$name"; diff build/$$name-llvm.out build/$$name-c.out | head; exit 1; }; \
	done; \
	for name in $(LLVM_EXAMPLES); do \
	  f=../examples/llvm/$$name.kai; \
	  ./$(TARGET) --emit=llvm $$f > build/$$name.ll; \
	  clang -w -I ../stage0 build/$$name.ll ../stage0/runtime_llvm.c -o build/$$name-llvm; \
	  ./build/$$name-llvm > build/$$name-llvm.out; \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name-c; \
	  ./build/$$name-c > build/$$name-c.out; \
	  diff -q build/$$name-llvm.out build/$$name-c.out > /dev/null \
	    && echo "llvm OK $$name" \
	    || { echo "llvm DIFF $$name"; diff build/$$name-llvm.out build/$$name-c.out | head; exit 1; }; \
	done

# End-to-end: emitted C from kaic2 matches stage 1's stdout byte for byte.
test-run: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name; \
	  ./build/$$name > build/$$name.out; \
	  ../stage1/kaic1 $$f > build/$$name-s1.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-s1.c -o build/$$name-s1; \
	  ./build/$$name-s1 > build/$$name-s1.out; \
	  diff -q build/$$name.out build/$$name-s1.out > /dev/null \
	    && echo "run OK $$name" \
	    || { echo "run DIFF $$name"; exit 1; }; \
	done

# --test binaries produce the same stderr as stage 1's.
test-blocks: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --test $$f > build/$$name-t.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-t.c -o build/$$name-t; \
	  ./build/$$name-t 2> build/$$name-t.err || true; \
	  ../stage1/kaic1 --test $$f > build/$$name-t-s1.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-t-s1.c -o build/$$name-t-s1; \
	  ./build/$$name-t-s1 2> build/$$name-t-s1.err || true; \
	  diff -q build/$$name-t.err build/$$name-t-s1.err > /dev/null \
	    && echo "tests OK $$name" \
	    || { echo "tests DIFF $$name"; exit 1; }; \
	done

# Self-hosting: kaic2 compiles its own source, and the round-trip reaches
# a fixed point.
selfhost: $(TARGET) build
	@set -e; \
	./$(TARGET) $(SRC) > build/kaic2b.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/kaic2b.c -o build/kaic2b; \
	./build/kaic2b $(SRC) > build/kaic2c.c; \
	diff -q build/kaic2b.c build/kaic2c.c > /dev/null \
	  && echo "self-hosting fixed point: OK" \
	  || { echo "self-hosting fixed point: DIFF"; exit 1; }

test: test-tokens test-ast test-types test-env test-infer test-infer-check test-check test-run test-blocks test-llvm

clean:
	rm -rf build $(TARGET)
